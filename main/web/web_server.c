#include "web_server.h"

#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "board.h"
#include "board_config.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "io_manager.h"
#include "net.h"
#include "signals.h"
#include "storage.h"
#include "ui_engine.h"
#include "util.h"

static const char *TAG = "web";

#define MAX_CLIENTS   10
#define MAX_WS_FRAME  4096

static httpd_handle_t s_server;
static volatile bool s_have_ws;

/* ---------- Embedded editor files (see main/CMakeLists.txt) ---------- */

extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_index_html_gz_end");
extern const uint8_t app_js_gz_start[] asm("_binary_app_js_gz_start");
extern const uint8_t app_js_gz_end[] asm("_binary_app_js_gz_end");
extern const uint8_t style_css_gz_start[] asm("_binary_style_css_gz_start");
extern const uint8_t style_css_gz_end[] asm("_binary_style_css_gz_end");

typedef struct {
    const char *uri;
    const uint8_t *start;
    const uint8_t *end;
    const char *type;
} static_file_t;

static const static_file_t s_files[] = {
    { "/", index_html_gz_start, index_html_gz_end, "text/html; charset=utf-8" },
    { "/index.html", index_html_gz_start, index_html_gz_end, "text/html; charset=utf-8" },
    { "/app.js", app_js_gz_start, app_js_gz_end, "application/javascript" },
    { "/style.css", style_css_gz_start, style_css_gz_end, "text/css" },
};

/* ---------- Helpers ---------- */

static void cors(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
}

static esp_err_t send_json(httpd_req_t *req, cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!s) {
        return httpd_resp_send_500(req);
    }
    cors(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, s);
    cJSON_free(s);
    return r;
}

static esp_err_t send_result(httpd_req_t *req, bool ok, const char *error)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", ok);
    if (!ok) {
        httpd_resp_set_status(req, "400 Bad Request");
        cJSON_AddStringToObject(o, "error", error ? error : "failed");
    }
    return send_json(req, o);
}

/* Read the whole request body into a NUL-terminated PSRAM buffer. */
static char *read_body(httpd_req_t *req, size_t max, size_t *out_len)
{
    if (req->content_len > max) {
        return NULL;
    }
    char *buf = util_malloc_psram(req->content_len + 1);
    if (!buf) {
        return NULL;
    }
    size_t got = 0;
    int retries = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++retries < 5) {
            continue;
        }
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        got += r;
    }
    buf[got] = '\0';
    *out_len = got;
    return buf;
}

static void restart_cb(void *arg)
{
    esp_restart();
}

static void restart_later(void)
{
    const esp_timer_create_args_t args = { .callback = restart_cb, .name = "restart" };
    esp_timer_handle_t t;
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, 1000 * 1000);
    }
}

/* ---------- Static files ---------- */

static esp_err_t static_handler(httpd_req_t *req)
{
    size_t ulen = strcspn(req->uri, "?#");
    for (size_t i = 0; i < sizeof(s_files) / sizeof(s_files[0]); i++) {
        if (strlen(s_files[i].uri) == ulen && !strncmp(req->uri, s_files[i].uri, ulen)) {
            httpd_resp_set_type(req, s_files[i].type);
            httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
            httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
            return httpd_resp_send(req, (const char *)s_files[i].start, s_files[i].end - s_files[i].start);
        }
    }
    return httpd_resp_send_404(req);
}

/* ---------- Layout ---------- */

static esp_err_t layout_get(httpd_req_t *req)
{
    size_t len = 0;
    char *data = storage_read(APP_LAYOUT_PATH, &len);
    cors(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    esp_err_t r;
    if (data) {
        r = httpd_resp_send(req, data, len);
        free(data);
    } else {
        r = httpd_resp_sendstr(req, app_default_layout());
    }
    return r;
}

static esp_err_t layout_default_get(httpd_req_t *req)
{
    cors(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, app_default_layout());
}

static esp_err_t layout_post(httpd_req_t *req)
{
    size_t len;
    char *body = read_body(req, APP_MAX_LAYOUT, &len);
    if (!body) {
        return send_result(req, false, "layout missing or too large");
    }
    char err[128] = "";
    esp_err_t r = app_apply_json(body, len, true, err, sizeof(err));
    free(body);
    return send_result(req, r == ESP_OK, err);
}

static esp_err_t rollback_post(httpd_req_t *req)
{
    char err[128] = "";
    esp_err_t r = app_rollback(err, sizeof(err));
    return send_result(req, r == ESP_OK, err);
}

/* ---------- Info / signals ---------- */

static esp_err_t info_get(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON_AddStringToObject(o, "project", app->project_name);
    cJSON_AddStringToObject(o, "version", app->version);
    cJSON_AddStringToObject(o, "idf", app->idf_ver);
    cJSON_AddStringToObject(o, "built", app->date);
    cJSON_AddNumberToObject(o, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(o, "heap_internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(o, "heap_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(o, "width", BOARD_LCD_H_RES);
    cJSON_AddNumberToObject(o, "height", BOARD_LCD_V_RES);
    static const int gpios[] = BOARD_USER_GPIOS;
    cJSON *g = cJSON_AddArrayToObject(o, "user_gpios");
    for (size_t i = 0; i < sizeof(gpios) / sizeof(gpios[0]); i++) {
        cJSON_AddItemToArray(g, cJSON_CreateNumber(gpios[i]));
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    cJSON_AddStringToObject(o, "partition", running ? running->label : "?");
    net_info_json(o);
    storage_info_json(o);
    io_status_json(o);
    return send_json(req, o);
}

static esp_err_t signals_get(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    sig_to_json(o);
    return send_json(req, o);
}

static void set_signals_from_json(const cJSON *obj)
{
    const cJSON *it;
    cJSON_ArrayForEach(it, obj) {
        if (!it->string) {
            continue;
        }
        if (cJSON_IsNumber(it)) {
            sig_set_num(it->string, it->valuedouble, true);
        } else if (cJSON_IsBool(it)) {
            sig_set_num(it->string, cJSON_IsTrue(it), true);
        } else if (cJSON_IsString(it)) {
            sig_set_text(it->string, it->valuestring, true);
        }
    }
}

static esp_err_t signals_post(httpd_req_t *req)
{
    size_t len;
    char *body = read_body(req, 16 * 1024, &len);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        return send_result(req, false, "expected a JSON object");
    }
    set_signals_from_json(j);
    cJSON_Delete(j);
    return send_result(req, true, NULL);
}

/* ---------- WiFi ---------- */

static esp_err_t wifi_scan_get(httpd_req_t *req)
{
    cJSON *arr = cJSON_CreateArray();
    net_scan_json(arr);
    return send_json(req, arr);
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    size_t len;
    char *body = read_body(req, 1024, &len);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    const char *ssid = json_str(j, "ssid", NULL);
    esp_err_t r = ESP_ERR_INVALID_ARG;
    if (ssid) {
        r = net_set_sta(ssid, json_str(j, "password", ""), json_str(j, "hostname", NULL));
    }
    cJSON_Delete(j);
    return send_result(req, r == ESP_OK, "invalid WiFi settings");
}

/* ---------- System ---------- */

static esp_err_t reboot_post(httpd_req_t *req)
{
    send_result(req, true, NULL);
    restart_later();
    return ESP_OK;
}

static esp_err_t ota_post(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        return send_result(req, false, "no OTA partition");
    }
    esp_ota_handle_t ota;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
        return send_result(req, false, "OTA begin failed");
    }
    char *buf = malloc(4096);
    size_t remaining = req->content_len;
    esp_err_t err = buf ? ESP_OK : ESP_ERR_NO_MEM;
    int retries = 0;
    while (err == ESP_OK && remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining < 4096 ? remaining : 4096);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++retries < 5) {
            continue;
        }
        if (r <= 0) {
            err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(ota, buf, r);
        remaining -= r;
    }
    free(buf);
    if (err != ESP_OK) {
        esp_ota_abort(ota);
        return send_result(req, false, "upload or flash write failed");
    }
    if (esp_ota_end(ota) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
        return send_result(req, false, "image is not a valid firmware for this device");
    }
    ESP_LOGI(TAG, "OTA written to %s, restarting", part->label);
    send_result(req, true, NULL);
    restart_later();
    return ESP_OK;
}

/* Screenshot as a 16-bit RGB565 BMP (top-down). */
static esp_err_t screenshot_get(httpd_req_t *req)
{
    lvgl_port_lock(0);
    lv_draw_buf_t *snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    lvgl_port_unlock();
    if (!snap) {
        return httpd_resp_send_500(req);
    }
    uint32_t w = snap->header.w, h = snap->header.h, stride = snap->header.stride;
    uint32_t row = w * 2;
    uint32_t img = row * h;
    uint8_t hdr[66] = { 'B', 'M' };
    uint32_t file = sizeof(hdr) + img;
    #define PUT32(o, v) do { hdr[o] = (v) & 0xFF; hdr[o + 1] = ((v) >> 8) & 0xFF; \
                             hdr[o + 2] = ((v) >> 16) & 0xFF; hdr[o + 3] = ((v) >> 24) & 0xFF; } while (0)
    PUT32(2, file);
    PUT32(10, sizeof(hdr));
    PUT32(14, 40);
    PUT32(18, w);
    PUT32(22, (uint32_t)(-(int32_t)h));   /* negative height = top-down rows */
    hdr[26] = 1;                         /* planes */
    hdr[28] = 16;                        /* bpp */
    PUT32(30, 3);                        /* BI_BITFIELDS */
    PUT32(34, img);
    PUT32(54, 0xF800);                   /* red mask */
    PUT32(58, 0x07E0);                   /* green mask */
    PUT32(62, 0x001F);                   /* blue mask */
    #undef PUT32

    httpd_resp_set_type(req, "image/bmp");
    cors(req);
    esp_err_t r = httpd_resp_send_chunk(req, (const char *)hdr, sizeof(hdr));
    for (uint32_t y = 0; r == ESP_OK && y < h; ) {
        uint32_t rows = 1;
        if (stride == row) {
            rows = h - y < 32 ? h - y : 32;
        }
        r = httpd_resp_send_chunk(req, (const char *)snap->data + (size_t)y * stride, rows * row);
        y += rows;
    }
    if (r == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    lv_draw_buf_destroy(snap);
    return r;
}

static esp_err_t options_handler(httpd_req_t *req)
{
    cors(req);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* ---------- WebSocket ---------- */

typedef struct {
    int fd;          /* -1 = all clients */
    char *msg;
} ws_job_t;

static void ws_send_work(void *arg)
{
    ws_job_t *job = arg;
    httpd_ws_frame_t f = {
        .final = true,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)job->msg,
        .len = strlen(job->msg),
    };
    if (job->fd >= 0) {
        httpd_ws_send_frame_async(s_server, job->fd, &f);
    } else {
        size_t n = MAX_CLIENTS;
        int fds[MAX_CLIENTS];
        bool any = false;
        if (httpd_get_client_list(s_server, &n, fds) == ESP_OK) {
            for (size_t i = 0; i < n; i++) {
                if (httpd_ws_get_fd_info(s_server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                    any = true;
                    httpd_ws_send_frame_async(s_server, fds[i], &f);
                }
            }
        }
        s_have_ws = any;
    }
    free(job->msg);
    free(job);
}

static void ws_queue(int fd, const char *text)
{
    if (!s_server || !text) {
        return;
    }
    ws_job_t *job = malloc(sizeof(*job));
    char *copy = strdup(text);
    if (!job || !copy) {
        free(job);
        free(copy);
        return;
    }
    job->fd = fd;
    job->msg = copy;
    if (httpd_queue_work(s_server, ws_send_work, job) != ESP_OK) {
        free(copy);
        free(job);
    }
}

void web_ws_broadcast(const char *text)
{
    if (s_have_ws) {
        ws_queue(-1, text);
    }
}

static void ws_send_snapshot(int fd)
{
    cJSON *o = cJSON_CreateObject();
    sig_to_json(cJSON_AddObjectToObject(o, "signals"));
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    ws_queue(fd, s);
    cJSON_free(s);
}

static void ws_handle_text(int fd, char *text)
{
    cJSON *j = cJSON_Parse(text);
    if (!cJSON_IsObject(j)) {
        /* Plain text: "name=value" lines set signals; the raw text goes to ws.rx */
        cJSON_Delete(j);
        sig_set_text("ws.rx", text, true);
        char *save = NULL;
        for (char *line = strtok_r(text, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
            if (strchr(line, '=')) {
                io_set_from_text("kv", "", line);
            }
        }
        return;
    }
    bool known = false;
    const cJSON *set = cJSON_GetObjectItem(j, "set");
    if (cJSON_IsObject(set)) {
        set_signals_from_json(set);
        known = true;
    }
    if (cJSON_GetObjectItem(j, "get")) {
        ws_send_snapshot(fd);
        known = true;
    }
    const char *log = json_str(j, "log", NULL);
    const cJSON *tab = cJSON_GetObjectItem(j, "tab");
    if (log || tab) {
        char tabbuf[32] = "";
        if (cJSON_IsNumber(tab)) {
            snprintf(tabbuf, sizeof(tabbuf), "%d", tab->valueint);
        } else if (cJSON_IsString(tab)) {
            strlcpy(tabbuf, tab->valuestring, sizeof(tabbuf));
        }
        lvgl_port_lock(0);
        if (log) {
            ui_engine_log(log);
        }
        if (tabbuf[0]) {
            ui_engine_show_tab(tabbuf);
        }
        lvgl_port_unlock();
        known = true;
    }
    if (!known) {
        sig_set_text("ws.rx", text, true);
    }
    cJSON_Delete(j);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        s_have_ws = true;
        ws_send_snapshot(httpd_req_to_sockfd(req));
        return ESP_OK;
    }
    httpd_ws_frame_t f = { 0 };
    esp_err_t r = httpd_ws_recv_frame(req, &f, 0);
    if (r != ESP_OK || f.len > MAX_WS_FRAME) {
        return ESP_FAIL;
    }
    if (f.type != HTTPD_WS_TYPE_TEXT || f.len == 0) {
        return ESP_OK;
    }
    f.payload = malloc(f.len + 1);
    if (!f.payload) {
        return ESP_ERR_NO_MEM;
    }
    r = httpd_ws_recv_frame(req, &f, f.len);
    if (r == ESP_OK) {
        f.payload[f.len] = '\0';
        ws_handle_text(httpd_req_to_sockfd(req), (char *)f.payload);
    }
    free(f.payload);
    return r;
}

/* ---------- Start ---------- */

esp_err_t web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 24;
    cfg.max_open_sockets = MAX_CLIENTS;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.core_id = 0;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true },
        { .uri = "/api/layout", .method = HTTP_GET, .handler = layout_get },
        { .uri = "/api/layout", .method = HTTP_POST, .handler = layout_post },
        { .uri = "/api/layout/default", .method = HTTP_GET, .handler = layout_default_get },
        { .uri = "/api/layout/rollback", .method = HTTP_POST, .handler = rollback_post },
        { .uri = "/api/info", .method = HTTP_GET, .handler = info_get },
        { .uri = "/api/signals", .method = HTTP_GET, .handler = signals_get },
        { .uri = "/api/signals", .method = HTTP_POST, .handler = signals_post },
        { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_get },
        { .uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post },
        { .uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post },
        { .uri = "/api/ota", .method = HTTP_POST, .handler = ota_post },
        { .uri = "/api/screenshot.bmp", .method = HTTP_GET, .handler = screenshot_get },
        { .uri = "/api/*", .method = HTTP_OPTIONS, .handler = options_handler },
        { .uri = "/*", .method = HTTP_GET, .handler = static_handler },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }
    ESP_LOGI(TAG, "web server started");
    return ESP_OK;
}
