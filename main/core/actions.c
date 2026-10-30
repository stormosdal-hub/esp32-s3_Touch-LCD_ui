#include "actions.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "board.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "io_manager.h"
#include "signals.h"
#include "template.h"
#include "ui_engine.h"
#include "util.h"
#include "web_server.h"

static const char *TAG = "actions";

#define QUEUE_LEN 48
#define MAX_DELAY_MS 10000
#define MAX_PAYLOAD 2048

typedef struct {
    char *json;       /* one action, serialised so the layout can change while queued */
    char *text;       /* optional text value */
    double num;
    bool is_text;
    char source[32];
} job_t;

static QueueHandle_t s_queue;

static char *expand(const char *tmpl, const job_t *job)
{
    const tmpl_ctx_t ctx = { .num = job->num, .text = job->text, .is_text = job->is_text, .source = job->source };
    return tmpl_expand(tmpl, &ctx);
}

/* ---------- Action implementations ---------- */

static void do_set_signal(const cJSON *a, const job_t *job)
{
    const char *name = json_str(a, "signal", "");
    const char *mode = json_str(a, "mode", "set");
    int idx = sig_index(name, true);
    if (idx < 0) {
        return;
    }
    char *val = expand(json_str(a, "value", "{value}"), job);
    double cur = 0, v = 0;
    sig_get_num(idx, &cur);
    if (!strcmp(mode, "toggle")) {
        sig_set_num(name, cur != 0 ? 0 : 1, false);
    } else if (!strcmp(mode, "add")) {
        if (util_parse_number(val, &v, true)) {
            sig_set_num(name, cur + v, false);
        }
    } else {
        sig_set_text(name, val, true);
    }
    free(val);
}

static void do_gpio(const cJSON *a, const job_t *job)
{
    int pin = json_int(a, "pin", -1);
    char *lvl = expand(json_str(a, "level", "{value}"), job);
    int level;
    double v;
    if (!strcmp(lvl, "toggle")) {
        level = -1;
    } else if (util_parse_number(lvl, &v, true)) {
        level = v != 0;
    } else {
        level = 0;
    }
    free(lvl);
    io_gpio_write(pin, level);
}

static int build_bytes(const cJSON *a, const char *key, const job_t *job, uint8_t *out, int max, bool hex)
{
    char *data = expand(json_str(a, key, ""), job);
    int n;
    if (hex) {
        n = util_parse_hex(data, out, max);
        if (n < 0) {
            ESP_LOGW(TAG, "invalid hex data: %s", data);
        }
    } else {
        n = (int)util_unescape(data);
        if (n > max) {
            n = max;
        }
        memcpy(out, data, n);
    }
    free(data);
    return n;
}

static void do_serial(const cJSON *a, const job_t *job)
{
    uint8_t *buf = malloc(MAX_PAYLOAD + 4);
    if (!buf) {
        return;
    }
    int n = build_bytes(a, "data", job, buf, MAX_PAYLOAD, json_bool(a, "hex", false));
    if (n >= 0) {
        const char *end = json_str(a, "append", "none");
        if (!strcmp(end, "cr") || !strcmp(end, "crlf")) {
            buf[n++] = '\r';
        }
        if (!strcmp(end, "lf") || !strcmp(end, "crlf")) {
            buf[n++] = '\n';
        }
        if (!strcmp(end, "modbus_crc") && n > 0) {
            uint32_t crc = util_crc16_modbus(buf, n);
            buf[n++] = crc & 0xFF;
            buf[n++] = crc >> 8;
        }
        io_serial_send(buf, n);
    }
    free(buf);
}

static void do_modbus(const cJSON *a, const job_t *job)
{
    char *val = expand(json_str(a, "value", "{value}"), job);
    double v = 0;
    util_parse_number(val, &v, true);
    free(val);
    io_modbus_write(json_int(a, "slave", 1), json_int(a, "fn", 6), json_int(a, "reg", 0),
                    json_str(a, "format", "u16"), v);
}

static void do_i2c(const cJSON *a, const job_t *job)
{
    uint8_t buf[64];
    int n = build_bytes(a, "data", job, buf, sizeof(buf), true);
    if (n > 0) {
        io_i2c_write(json_int(a, "addr", 0), buf, n);
    }
}

static void do_can(const cJSON *a, const job_t *job)
{
    uint8_t buf[8];
    int n = build_bytes(a, "data", job, buf, sizeof(buf), true);
    if (n >= 0) {
        io_can_send((uint32_t)json_num(a, "can_id", 0), json_bool(a, "ext", false), buf, n);
    }
}

static void do_mqtt(const cJSON *a, const job_t *job)
{
    char *topic = expand(json_str(a, "topic", ""), job);
    char *payload = expand(json_str(a, "payload", "{value}"), job);
    io_mqtt_publish(topic, payload, json_int(a, "qos", 0), json_bool(a, "retain", false));
    free(topic);
    free(payload);
}

static void do_http(const cJSON *a, const job_t *job)
{
    char *url = expand(json_str(a, "url", ""), job);
    char *body = expand(json_str(a, "body", ""), job);
    const char *method = json_str(a, "method", "GET");
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 5000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .method = !strcasecmp(method, "POST") ? HTTP_METHOD_POST
                  : !strcasecmp(method, "PUT") ? HTTP_METHOD_PUT : HTTP_METHOD_GET,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c) {
        if (*body) {
            esp_http_client_set_header(c, "Content-Type", json_str(a, "content_type", "application/json"));
            esp_http_client_set_post_field(c, body, strlen(body));
        }
        esp_err_t err = esp_http_client_perform(c);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "HTTP %s %s -> %d", method, url, esp_http_client_get_status_code(c));
        } else {
            ESP_LOGW(TAG, "HTTP %s %s failed: %s", method, url, esp_err_to_name(err));
        }
        esp_http_client_cleanup(c);
    }
    free(url);
    free(body);
}

static void do_ui(const cJSON *a, const job_t *job, bool is_tab)
{
    char *s = expand(json_str(a, is_tab ? "tab" : "text", ""), job);
    if (lvgl_port_lock(0)) {
        if (is_tab) {
            ui_engine_show_tab(s);
        } else {
            ui_engine_log(s);
        }
        lvgl_port_unlock();
    }
    free(s);
}

static void do_backlight(const cJSON *a)
{
    /* via ui.backlight, so the UI also arms/disarms its touch-to-wake layer */
    const char *st = json_str(a, "state", "toggle");
    bool on = !strcmp(st, "on") ? true : !strcmp(st, "off") ? false : !board_backlight_get();
    sig_set_num("ui.backlight", on, false);
}

static void execute(const job_t *job)
{
    cJSON *a = cJSON_Parse(job->json);
    if (!a) {
        return;
    }
    const char *type = json_str(a, "type", "");
    if (!strcmp(type, "set_signal")) {
        do_set_signal(a, job);
    } else if (!strcmp(type, "ws_send")) {
        char *payload = expand(json_str(a, "payload", "{value}"), job);
        web_ws_broadcast(payload);
        free(payload);
    } else if (!strcmp(type, "gpio")) {
        do_gpio(a, job);
    } else if (!strcmp(type, "serial_send")) {
        do_serial(a, job);
    } else if (!strcmp(type, "modbus_write")) {
        do_modbus(a, job);
    } else if (!strcmp(type, "i2c_write")) {
        do_i2c(a, job);
    } else if (!strcmp(type, "can_send")) {
        do_can(a, job);
    } else if (!strcmp(type, "mqtt_publish")) {
        do_mqtt(a, job);
    } else if (!strcmp(type, "http")) {
        do_http(a, job);
    } else if (!strcmp(type, "tab")) {
        do_ui(a, job, true);
    } else if (!strcmp(type, "log")) {
        do_ui(a, job, false);
    } else if (!strcmp(type, "backlight")) {
        do_backlight(a);
    } else if (!strcmp(type, "delay")) {
        int ms = json_int(a, "ms", 100);
        vTaskDelay(pdMS_TO_TICKS(ms < 0 ? 0 : ms > MAX_DELAY_MS ? MAX_DELAY_MS : ms));
    } else {
        ESP_LOGW(TAG, "unknown action type '%s'", type);
    }
    cJSON_Delete(a);
}

static void worker(void *arg)
{
    job_t job;
    for (;;) {
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) == pdTRUE) {
            execute(&job);
            cJSON_free(job.json);
            free(job.text);
        }
    }
}

void actions_init(void)
{
    s_queue = xQueueCreate(QUEUE_LEN, sizeof(job_t));
    xTaskCreatePinnedToCore(worker, "actions", 10 * 1024, NULL, 5, NULL, 0);
}

void actions_run(const cJSON *actions, double num, const char *text, bool value_is_text, const char *source)
{
    const cJSON *a;
    cJSON_ArrayForEach(a, actions) {
        if (!cJSON_IsObject(a)) {
            continue;
        }
        job_t job = {
            .json = cJSON_PrintUnformatted(a),
            .text = text ? strdup(text) : NULL,
            .num = num,
            .is_text = value_is_text && text,
        };
        strlcpy(job.source, source ? source : "", sizeof(job.source));
        if (!job.json || xQueueSend(s_queue, &job, 0) != pdTRUE) {
            ESP_LOGW(TAG, "action queue full, dropping action");
            cJSON_free(job.json);
            free(job.text);
        }
    }
}
