/*
 * RS-485 port: either raw text lines (every line goes to signal "rs485.rx" and
 * to "serial" inputs) or a Modbus RTU master polling "modbus" inputs.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "board_config.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "io_internal.h"
#include "io_manager.h"
#include "util.h"

static const char *TAG = "rs485";

#define UART_PORT   BOARD_RS485_UART
#define RX_LINE_MAX    256
#define TXQ_LEN     16

typedef struct {
    bool enabled;
    bool modbus;
    int baud;
    int timeout_ms;   /* modbus response timeout */
    int idle_ms;      /* raw mode: flush a partial line after this much silence (0 = never) */
    char cfg_text[64];
} serial_cfg_t;

typedef struct {
    uint8_t *data;    /* raw bytes, or NULL for a modbus write */
    size_t len;
    uint8_t slave, fn;
    uint16_t reg;
    uint16_t regs[2];
    uint8_t nregs;
} tx_item_t;

static serial_cfg_t s_cfg;
static bool s_installed;
static SemaphoreHandle_t s_uart_lock;
static QueueHandle_t s_txq;
static uint32_t s_rx_lines, s_tx_bytes, s_mb_ok, s_mb_err;

/* ---------- Modbus RTU ---------- */

static void add_crc(uint8_t *buf, int *len)
{
    uint32_t crc = util_crc16_modbus(buf, *len);
    buf[(*len)++] = crc & 0xFF;
    buf[(*len)++] = crc >> 8;
}

static bool crc_ok(const uint8_t *buf, int len)
{
    if (len < 4) {
        return false;
    }
    uint32_t crc = util_crc16_modbus(buf, len - 2);
    return buf[len - 2] == (crc & 0xFF) && buf[len - 1] == (crc >> 8);
}

/* Send a request and read the reply. Returns reply length or -1. Caller holds s_uart_lock. */
static int mb_transact(const uint8_t *req, int reqlen, uint8_t *resp, int maxlen)
{
    uart_flush_input(UART_PORT);
    uart_write_bytes(UART_PORT, req, reqlen);
    uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(200));
    s_tx_bytes += reqlen;

    TickType_t to = pdMS_TO_TICKS(s_cfg.timeout_ms);
    int n = uart_read_bytes(UART_PORT, resp, 3, to);
    if (n < 3 || resp[0] != req[0]) {
        return -1;
    }
    int total;
    if (resp[1] & 0x80) {
        total = 5;                       /* exception reply */
    } else if (req[1] <= 4) {
        total = 5 + resp[2];             /* read reply: byte count in resp[2] */
    } else {
        total = 8;                       /* write echo */
    }
    if (total > maxlen) {
        return -1;
    }
    n += uart_read_bytes(UART_PORT, resp + 3, total - 3, to);
    vTaskDelay(pdMS_TO_TICKS(5));        /* inter-frame gap */
    if (n != total || !crc_ok(resp, total) || (resp[1] & 0x80)) {
        return -1;
    }
    return total;
}

static void mb_poll(int slave, int fn, int reg, const char *fmt, double scale, double offset, const char *signal)
{
    int size = io_format_size(fmt);
    int count = (fn <= 2) ? 1 : (size == 4 ? 2 : 1);
    uint8_t req[8] = { slave, fn, reg >> 8, reg & 0xFF, count >> 8, count & 0xFF };
    int len = 6;
    add_crc(req, &len);
    uint8_t resp[16];

    xSemaphoreTake(s_uart_lock, portMAX_DELAY);
    int n = (s_installed && s_cfg.modbus) ? mb_transact(req, len, resp, sizeof(resp)) : -1;
    xSemaphoreGive(s_uart_lock);

    if (n < 0) {
        s_mb_err++;
        return;
    }
    s_mb_ok++;
    double v;
    if (fn <= 2) {
        v = resp[3] & 1;
    } else {
        uint8_t data[4];
        memcpy(data, resp + 3, count * 2);
        if (count == 2 && strlen(fmt) > 1 && fmt[strlen(fmt) - 1] == 'r') {
            /* low word first */
            uint8_t t0 = data[0], t1 = data[1];
            data[0] = data[2];
            data[1] = data[3];
            data[2] = t0;
            data[3] = t1;
        }
        if (!io_decode(data, count * 2, fmt, &v)) {
            return;
        }
    }
    sig_set_num(signal, v * scale + offset, false);
}

static void mb_write(const tx_item_t *t)
{
    uint8_t req[16];
    int len = 0;
    req[len++] = t->slave;
    req[len++] = t->fn;
    req[len++] = t->reg >> 8;
    req[len++] = t->reg & 0xFF;
    if (t->fn == 16) {
        req[len++] = 0;
        req[len++] = t->nregs;
        req[len++] = t->nregs * 2;
    }
    for (int i = 0; i < t->nregs; i++) {
        req[len++] = t->regs[i] >> 8;
        req[len++] = t->regs[i] & 0xFF;
    }
    add_crc(req, &len);
    uint8_t resp[16];
    if (mb_transact(req, len, resp, sizeof(resp)) < 0) {
        s_mb_err++;
        ESP_LOGW(TAG, "modbus write slave %d reg %d failed", t->slave, t->reg);
    } else {
        s_mb_ok++;
    }
}

/* Pick the first modbus input that is due and poll it (outside the config lock). */
static void mb_poll_due(void)
{
    char signal[SIG_NAME_MAX], fmt[8];
    int slave = 0, fn = 0, reg = 0;
    double scale = 1, offset = 0;
    bool found = false;
    int64_t now = esp_timer_get_time();

    io_cfg_lock();
    int n;
    io_input_t *in = io_inputs(&n);
    for (int i = 0; i < n; i++) {
        if (in[i].type == IN_MODBUS && now >= in[i].next_due_us) {
            in[i].next_due_us = now + (int64_t)in[i].period_ms * 1000;
            strlcpy(signal, in[i].signal, sizeof(signal));
            strlcpy(fmt, in[i].format, sizeof(fmt));
            slave = in[i].addr;
            fn = in[i].fn;
            reg = in[i].reg;
            scale = in[i].scale;
            offset = in[i].offset;
            found = true;
            break;
        }
    }
    io_cfg_unlock();

    if (found) {
        mb_poll(slave, fn, reg, fmt, scale, offset, signal);
    }
}

/* ---------- Raw lines ---------- */

static void dispatch_line(const char *line)
{
    s_rx_lines++;
    sig_set_text("rs485.rx", line, true);
    io_cfg_lock();
    int n;
    io_input_t *in = io_inputs(&n);
    for (int i = 0; i < n; i++) {
        if (in[i].type == IN_SERIAL) {
            io_store_text(in[i].mode, in[i].signal, line);
        }
    }
    io_cfg_unlock();
}

static void serial_task(void *arg)
{
    static char line[RX_LINE_MAX];
    size_t llen = 0;
    int64_t last_rx = 0;
    uint8_t buf[128];

    for (;;) {
        xSemaphoreTake(s_uart_lock, portMAX_DELAY);
        if (!s_installed) {
            xSemaphoreGive(s_uart_lock);
            llen = 0;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        tx_item_t t;
        while (xQueueReceive(s_txq, &t, 0) == pdTRUE) {
            if (t.data) {
                uart_write_bytes(UART_PORT, t.data, t.len);
                s_tx_bytes += t.len;
                free(t.data);
            } else {
                mb_write(&t);
            }
        }

        if (s_cfg.modbus) {
            xSemaphoreGive(s_uart_lock);
            mb_poll_due();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int n = uart_read_bytes(UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(20));
        int idle_ms = s_cfg.idle_ms;
        xSemaphoreGive(s_uart_lock);

        int64_t now = esp_timer_get_time();
        for (int i = 0; i < n; i++) {
            last_rx = now;
            char c = (char)buf[i];
            if (c == '\n') {
                if (llen && line[llen - 1] == '\r') {
                    llen--;
                }
                line[llen] = '\0';
                if (llen) {
                    dispatch_line(line);
                }
                llen = 0;
            } else if (llen < RX_LINE_MAX - 1) {
                line[llen++] = c;
            }
        }
        if (llen && idle_ms > 0 && now - last_rx > (int64_t)idle_ms * 1000) {
            while (llen && line[llen - 1] == '\r') {
                llen--;
            }
            line[llen] = '\0';
            if (llen) {
                dispatch_line(line);
            }
            llen = 0;
        }
    }
}

/* ---------- Public ---------- */

void io_serial_init(void)
{
    s_uart_lock = xSemaphoreCreateMutex();
    s_txq = xQueueCreate(TXQ_LEN, sizeof(tx_item_t));
    xTaskCreatePinnedToCore(serial_task, "rs485", 4096, NULL, 4, NULL, 0);
}

void io_serial_apply(const cJSON *port)
{
    serial_cfg_t c = {
        .enabled = port ? json_bool(port, "enabled", true) : true,
        .modbus = port && !strcmp(json_str(port, "mode", "raw"), "modbus"),
        .baud = json_int(port, "baud", 9600),
        .timeout_ms = json_int(port, "timeout_ms", 200),
        .idle_ms = json_int(port, "idle_ms", 100),
    };
    const char *parity = json_str(port, "parity", "none");
    int stop = json_int(port, "stop_bits", 1);
    snprintf(c.cfg_text, sizeof(c.cfg_text), "%d 8%c%d %s", c.baud,
             parity[0] == 'e' ? 'E' : parity[0] == 'o' ? 'O' : 'N', stop, c.modbus ? "modbus" : "raw");

    xSemaphoreTake(s_uart_lock, portMAX_DELAY);
    if (s_installed) {
        uart_driver_delete(UART_PORT);
        s_installed = false;
    }
    tx_item_t t;
    while (xQueueReceive(s_txq, &t, 0) == pdTRUE) {
        free(t.data);
    }
    if (c.enabled) {
        uart_config_t ucfg = {
            .baud_rate = c.baud,
            .data_bits = UART_DATA_8_BITS,
            .parity = parity[0] == 'e' ? UART_PARITY_EVEN : parity[0] == 'o' ? UART_PARITY_ODD : UART_PARITY_DISABLE,
            .stop_bits = stop == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        if (uart_driver_install(UART_PORT, 2048, 1024, 0, NULL, 0) == ESP_OK) {
            if (uart_param_config(UART_PORT, &ucfg) == ESP_OK &&
                    uart_set_pin(UART_PORT, BOARD_RS485_TX, BOARD_RS485_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) == ESP_OK) {
                s_installed = true;
            } else {
                uart_driver_delete(UART_PORT);
            }
        }
        if (!s_installed) {
            ESP_LOGE(TAG, "failed to start RS-485 port");
        }
    }
    s_cfg = c;
    xSemaphoreGive(s_uart_lock);
    ESP_LOGI(TAG, "RS-485 %s", c.enabled ? c.cfg_text : "disabled");
}

esp_err_t io_serial_send(const uint8_t *data, size_t len)
{
    if (!s_installed || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    tx_item_t t = { .data = malloc(len), .len = len };
    if (!t.data) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(t.data, data, len);
    if (xQueueSend(s_txq, &t, pdMS_TO_TICKS(50)) != pdTRUE) {
        free(t.data);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t io_modbus_write(int slave, int fn, int reg, const char *format, double value)
{
    if (!s_installed || !s_cfg.modbus) {
        ESP_LOGW(TAG, "modbus write ignored: RS-485 port is not in modbus mode");
        return ESP_ERR_INVALID_STATE;
    }
    tx_item_t t = { .slave = slave, .fn = fn, .reg = reg, .nregs = 1 };
    if (fn == 5) {
        t.regs[0] = value != 0 ? 0xFF00 : 0x0000;
    } else if (fn == 6) {
        t.regs[0] = (uint16_t)(int32_t)lround(value);
    } else if (fn == 16) {
        uint32_t raw;
        bool wide = io_format_size(format) == 4;
        if (format[0] == 'f') {
            float f = (float)value;
            memcpy(&raw, &f, 4);
        } else {
            raw = (uint32_t)(int32_t)lround(value);
        }
        if (wide) {
            bool swap = strlen(format) > 1 && format[strlen(format) - 1] == 'r';
            t.nregs = 2;
            t.regs[0] = swap ? raw & 0xFFFF : raw >> 16;
            t.regs[1] = swap ? raw >> 16 : raw & 0xFFFF;
        } else {
            t.regs[0] = raw & 0xFFFF;
        }
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return xQueueSend(s_txq, &t, pdMS_TO_TICKS(50)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void io_serial_status(cJSON *obj)
{
    cJSON *p = cJSON_AddObjectToObject(obj, "rs485");
    cJSON_AddBoolToObject(p, "running", s_installed);
    cJSON_AddStringToObject(p, "config", s_cfg.enabled ? s_cfg.cfg_text : "disabled");
    cJSON_AddNumberToObject(p, "rx_lines", s_rx_lines);
    cJSON_AddNumberToObject(p, "tx_bytes", s_tx_bytes);
    cJSON_AddNumberToObject(p, "modbus_ok", s_mb_ok);
    cJSON_AddNumberToObject(p, "modbus_errors", s_mb_err);
}
