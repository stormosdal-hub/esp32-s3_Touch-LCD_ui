#include "io_manager.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "board.h"
#include "board_config.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "io_internal.h"
#include "util.h"

static const char *TAG = "io";

#define POLL_MS 10

static SemaphoreHandle_t s_cfg_lock;
static io_input_t *s_inputs;
static int s_count;
static adc_oneshot_unit_handle_t s_adc_units[2];
static uint64_t s_out_pins;     /* GPIOs configured as outputs */
static uint64_t s_in_pins;      /* GPIOs configured as inputs by the current layout */

void io_cfg_lock(void)
{
    xSemaphoreTakeRecursive(s_cfg_lock, portMAX_DELAY);
}

void io_cfg_unlock(void)
{
    xSemaphoreGiveRecursive(s_cfg_lock);
}

io_input_t *io_inputs(int *count)
{
    *count = s_count;
    return s_inputs;
}

/* ---------- Value helpers ---------- */

void io_store(const io_input_t *in, double raw)
{
    sig_set_num(in->signal, raw * in->scale + in->offset, false);
}

void io_store_text(const char *mode, const char *signal, const char *text)
{
    if (!strcmp(mode, "kv")) {
        /* "name=value" or "name:value"; `signal` acts as a name prefix */
        const char *sep = strpbrk(text, "=:");
        if (!sep || sep == text) {
            return;
        }
        char name[SIG_NAME_MAX];
        size_t plen = strlcpy(name, signal ? signal : "", sizeof(name));
        size_t klen = sep - text;
        if (plen + klen >= sizeof(name)) {
            klen = sizeof(name) - plen - 1;
        }
        memcpy(name + plen, text, klen);
        name[plen + klen] = '\0';
        while (klen && name[plen + klen - 1] == ' ') {
            name[plen + --klen] = '\0';
        }
        const char *val = sep + 1;
        while (*val == ' ') {
            val++;
        }
        sig_set_text_remote(name, val);
    } else if (!strcmp(mode, "json")) {
        cJSON *j = util_json_parse(text, strlen(text));
        const cJSON *it;
        cJSON_ArrayForEach(it, j) {
            char name[SIG_NAME_MAX];
            snprintf(name, sizeof(name), "%s%s", signal ? signal : "", it->string ? it->string : "");
            if (cJSON_IsNumber(it)) {
                sig_set_num_remote(name, it->valuedouble);
            } else if (cJSON_IsBool(it)) {
                sig_set_num_remote(name, cJSON_IsTrue(it));
            } else if (cJSON_IsString(it)) {
                sig_set_text_remote(name, it->valuestring);
            }
        }
        cJSON_Delete(j);
    } else if (signal && *signal) {
        sig_set_text(signal, text, true);
    }
}

void io_set_from_text(const char *mode, const char *signal, const char *text)
{
    io_store_text(mode, signal, text);
}

/* ---------- Inputs ---------- */

static bool user_pin(int pin, const char *what)
{
    if (!board_gpio_is_user(pin)) {
        ESP_LOGW(TAG, "%s: GPIO%d is used by the board and cannot be used here", what, pin);
        return false;
    }
    if ((pin == BOARD_RS485_TX || pin == BOARD_RS485_RX) && io_serial_running()) {
        ESP_LOGW(TAG, "%s: GPIO%d belongs to RS-485; disable RS-485 in Settings to use it", what, pin);
        return false;
    }
    if ((pin == BOARD_CAN_TX || pin == BOARD_CAN_RX) && io_can_running()) {
        ESP_LOGW(TAG, "%s: GPIO%d belongs to CAN; disable CAN in Settings to use it", what, pin);
        return false;
    }
    return true;
}

static void parse_common(io_input_t *in, const cJSON *j)
{
    strlcpy(in->id, json_str(j, "id", ""), sizeof(in->id));
    strlcpy(in->signal, json_str(j, "signal", ""), sizeof(in->signal));
    strlcpy(in->format, json_str(j, "format", "u16"), sizeof(in->format));
    in->scale = json_num(j, "scale", 1);
    in->offset = json_num(j, "offset", 0);
    in->period_ms = json_int(j, "period_ms", 1000);
    if (in->period_ms < 20) {
        in->period_ms = 20;
    }
    in->stable = -1;
}

static bool setup_gpio(io_input_t *in, const cJSON *j)
{
    in->pin = json_int(j, "pin", -1);
    if (!user_pin(in->pin, "gpio input")) {
        return false;
    }
    in->invert = json_bool(j, "invert", false);
    in->debounce_ms = json_int(j, "debounce_ms", 30);
    const char *pull = json_str(j, "pull", "up");
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << in->pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = !strcmp(pull, "up"),
        .pull_down_en = !strcmp(pull, "down"),
    };
    s_out_pins &= ~(1ULL << in->pin);
    s_in_pins |= 1ULL << in->pin;
    return gpio_config(&cfg) == ESP_OK;
}

static bool setup_adc(io_input_t *in, const cJSON *j)
{
    in->pin = json_int(j, "pin", 6);
    if (!user_pin(in->pin, "adc input")) {
        return false;
    }
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(in->pin, &unit, &in->adc_chan) != ESP_OK || unit > ADC_UNIT_2) {
        ESP_LOGW(TAG, "GPIO%d has no ADC channel", in->pin);
        return false;
    }
    if (!s_adc_units[unit]) {
        adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = unit };
        if (adc_oneshot_new_unit(&ucfg, &s_adc_units[unit]) != ESP_OK) {
            return false;
        }
    }
    in->adc = s_adc_units[unit];
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_oneshot_config_channel(in->adc, in->adc_chan, &ccfg) != ESP_OK) {
        return false;
    }
    in->millivolts = !strcmp(json_str(j, "mode", "mv"), "mv");
    if (in->millivolts) {
        adc_cali_curve_fitting_config_t cal = {
            .unit_id = unit, .chan = in->adc_chan, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_curve_fitting(&cal, &in->cali) != ESP_OK) {
            in->cali = NULL;
        }
    }
    return true;
}

static bool setup_i2c(io_input_t *in, const cJSON *j)
{
    in->addr = json_int(j, "addr", 0);
    in->reg = json_int(j, "reg", -1);
    in->len = json_int(j, "len", 0);
    int need = io_format_size(in->format);
    if (need == 0) {
        ESP_LOGW(TAG, "i2c input: bad format '%s'", in->format);
        return false;
    }
    if (in->len < need) {
        in->len = need;
    }
    if (in->len > 16 || in->addr < 0x08 || in->addr > 0x77) {
        return false;
    }
    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = in->addr,
        .scl_speed_hz = json_int(j, "speed_hz", 100000),
    };
    return i2c_master_bus_add_device(board_i2c_bus(), &dcfg, &in->i2c_dev) == ESP_OK;
}

static bool parse_input(io_input_t *in, const cJSON *j)
{
    memset(in, 0, sizeof(*in));
    parse_common(in, j);
    const char *type = json_str(j, "type", "");
    if (!strcmp(type, "gpio")) {
        in->type = IN_GPIO;
        return setup_gpio(in, j);
    }
    if (!strcmp(type, "adc")) {
        in->type = IN_ADC;
        return setup_adc(in, j);
    }
    if (!strcmp(type, "i2c")) {
        in->type = IN_I2C;
        return setup_i2c(in, j);
    }
    if (!strcmp(type, "serial")) {
        in->type = IN_SERIAL;
        strlcpy(in->mode, json_str(j, "mode", "line"), sizeof(in->mode));
        return true;
    }
    if (!strcmp(type, "modbus")) {
        in->type = IN_MODBUS;
        in->addr = json_int(j, "slave", 1);
        in->fn = json_int(j, "fn", 3);
        in->reg = json_int(j, "reg", 0);
        return in->fn >= 1 && in->fn <= 4;
    }
    if (!strcmp(type, "can")) {
        in->type = IN_CAN;
        in->can_id = (uint32_t)json_num(j, "can_id", 0);
        in->ext = json_bool(j, "ext", false);
        in->start = json_int(j, "start", 0);
        strlcpy(in->format, json_str(j, "format", "u8"), sizeof(in->format));
        return true;
    }
    if (!strcmp(type, "mqtt")) {
        in->type = IN_MQTT;
        strlcpy(in->topic, json_str(j, "topic", ""), sizeof(in->topic));
        strlcpy(in->json_key, json_str(j, "json_key", ""), sizeof(in->json_key));
        return in->topic[0] != '\0';
    }
    ESP_LOGW(TAG, "unknown input type '%s'", type);
    return false;
}

static void release_inputs(io_input_t *list, int n)
{
    for (int i = 0; i < n; i++) {
        if (list[i].i2c_dev) {
            i2c_master_bus_rm_device(list[i].i2c_dev);
        }
        if (list[i].cali) {
            adc_cali_delete_scheme_curve_fitting(list[i].cali);
        }
    }
    free(list);
}

static void poll_gpio(io_input_t *in, int64_t now)
{
    int raw = gpio_get_level(in->pin) ^ (in->invert ? 1 : 0);
    if (in->raw_since_us == 0 || raw != in->last_raw) {
        in->last_raw = raw;
        in->raw_since_us = now;
    }
    if (raw != in->stable && now - in->raw_since_us >= (int64_t)in->debounce_ms * 1000) {
        in->stable = raw;
        sig_set_num(in->signal, raw, false);
    }
}

static void poll_adc(io_input_t *in)
{
    int raw = 0, mv = 0;
    if (adc_oneshot_read(in->adc, in->adc_chan, &raw) != ESP_OK) {
        return;
    }
    if (in->millivolts && in->cali && adc_cali_raw_to_voltage(in->cali, raw, &mv) == ESP_OK) {
        raw = mv;
    }
    io_store(in, raw);
}

static void poll_i2c(io_input_t *in)
{
    uint8_t buf[16];
    esp_err_t err;
    if (in->reg >= 0) {
        uint8_t reg = (uint8_t)in->reg;
        err = i2c_master_transmit_receive(in->i2c_dev, &reg, 1, buf, in->len, 50);
    } else {
        err = i2c_master_receive(in->i2c_dev, buf, in->len, 50);
    }
    double v;
    if (err == ESP_OK && io_decode(buf, in->len, in->format, &v)) {
        io_store(in, v);
    }
}

static void poll_task(void *arg)
{
    for (;;) {
        int64_t now = esp_timer_get_time();
        io_cfg_lock();
        for (int i = 0; i < s_count; i++) {
            io_input_t *in = &s_inputs[i];
            if (in->type == IN_GPIO) {
                poll_gpio(in, now);
                continue;
            }
            if ((in->type != IN_ADC && in->type != IN_I2C) || now < in->next_due_us) {
                continue;
            }
            in->next_due_us = now + (int64_t)in->period_ms * 1000;
            if (in->type == IN_ADC) {
                poll_adc(in);
            } else {
                poll_i2c(in);
            }
        }
        io_cfg_unlock();
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t io_manager_init(void)
{
    s_cfg_lock = xSemaphoreCreateRecursiveMutex();
    io_serial_init();
    io_can_init();
    io_mqtt_init();
    xTaskCreatePinnedToCore(poll_task, "io_poll", 4096, NULL, 3, NULL, 0);
    return ESP_OK;
}

void io_manager_apply(const cJSON *layout)
{
    const cJSON *ports = cJSON_GetObjectItem(layout, "ports");
    const cJSON *list = cJSON_GetObjectItem(layout, "inputs");

    io_cfg_lock();
    for (int pin = 0; pin < 64; pin++) {
        if (s_in_pins & (1ULL << pin)) {
            gpio_reset_pin(pin);
        }
    }
    s_in_pins = 0;
    release_inputs(s_inputs, s_count);
    s_inputs = NULL;
    s_count = 0;

    /* Ports before inputs, so input pins can be checked against them. */
    io_serial_apply(cJSON_GetObjectItem(ports, "rs485"));
    io_can_apply(cJSON_GetObjectItem(ports, "can"));

    int n = cJSON_GetArraySize(list);
    if (n > IO_MAX_INPUTS) {
        ESP_LOGW(TAG, "only the first %d inputs are used", IO_MAX_INPUTS);
        n = IO_MAX_INPUTS;
    }
    if (n > 0) {
        s_inputs = heap_caps_calloc(n, sizeof(io_input_t), MALLOC_CAP_SPIRAM);
    }
    for (int i = 0; s_inputs && i < n; i++) {
        const cJSON *j = cJSON_GetArrayItem(list, i);
        if (!json_bool(j, "enabled", true)) {
            continue;
        }
        if (parse_input(&s_inputs[s_count], j)) {
            s_count++;
        } else {
            ESP_LOGW(TAG, "input %d (%s) skipped", i, json_str(j, "id", "?"));
            if (s_inputs[s_count].i2c_dev) {
                i2c_master_bus_rm_device(s_inputs[s_count].i2c_dev);
            }
        }
    }
    ESP_LOGI(TAG, "%d inputs active", s_count);

    io_cfg_unlock();
    /* Outside the lock: stopping the MQTT client waits for its task, which may need the lock. */
    io_mqtt_apply(cJSON_GetObjectItem(ports, "mqtt"));
}

/* ---------- Outputs ---------- */

esp_err_t io_gpio_write(int pin, int level)
{
    if (!user_pin(pin, "gpio action")) {
        return ESP_ERR_INVALID_ARG;
    }
    io_cfg_lock();
    if (!(s_out_pins & (1ULL << pin))) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << pin,
            .mode = GPIO_MODE_INPUT_OUTPUT,
        };
        gpio_config(&cfg);
        s_out_pins |= 1ULL << pin;
        s_in_pins &= ~(1ULL << pin);
    }
    if (level < 0) {
        level = !gpio_get_level(pin);
    }
    esp_err_t err = gpio_set_level(pin, level);
    io_cfg_unlock();
    return err;
}

esp_err_t io_i2c_write(int addr, const uint8_t *data, size_t len)
{
    if (addr < 0x08 || addr > 0x77) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t dev;
    esp_err_t err = i2c_master_bus_add_device(board_i2c_bus(), &dcfg, &dev);
    if (err == ESP_OK) {
        err = i2c_master_transmit(dev, data, len, 100);
        i2c_master_bus_rm_device(dev);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2c write to 0x%02X failed: %s", addr, esp_err_to_name(err));
    }
    return err;
}

void io_status_json(cJSON *obj)
{
    cJSON *ports = cJSON_AddObjectToObject(obj, "ports");
    io_serial_status(ports);
    io_can_status(ports);
    io_mqtt_status(ports);
    io_cfg_lock();
    cJSON_AddNumberToObject(obj, "inputs_active", s_count);
    io_cfg_unlock();
}
