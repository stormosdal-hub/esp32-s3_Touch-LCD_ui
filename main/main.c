/*
 * ESP32-S3-Touch-LCD-7 UI designer firmware.
 *
 * The screen is described by a JSON layout (tabs, widgets, inputs, rules)
 * stored in LittleFS. A web editor served by the device edits that layout and
 * deploys it without reflashing.
 */
#include <stdio.h>
#include <string.h>
#include "actions.h"
#include "app.h"
#include "board.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "io_manager.h"
#include "net.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "signals.h"
#include "storage.h"
#include "ui_engine.h"
#include "util.h"
#include "web_server.h"

static const char *TAG = "main";

extern const char default_layout_start[] asm("_binary_default_layout_json_start");

static SemaphoreHandle_t s_apply_lock;

/*
 * Crash guard: a counter in NVS is raised before a layout is applied and
 * cleared once it is running. If the device reboots in between, the next boot
 * falls back to the previous layout, then to the built-in one.
 */
#define NVS_APP      "app"
#define NVS_ATTEMPTS "apply_try"

static uint8_t apply_attempts(void)
{
    uint8_t v = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_APP, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_ATTEMPTS, &v);
        nvs_close(h);
    }
    return v;
}

static void set_apply_attempts(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_APP, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_ATTEMPTS, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

const char *app_default_layout(void)
{
    return default_layout_start;
}

/* Takes ownership of root. */
static void apply_root(cJSON *root)
{
    const cJSON *set = cJSON_GetObjectItem(root, "settings");
    net_set_timezone(json_str(set, "timezone", "UTC0"));
    io_manager_apply(root);
    lvgl_port_lock(0);
    ui_engine_build(root);
    lvgl_port_unlock();
}

static cJSON *parse_layout(const char *json, size_t len, char *err, size_t errlen)
{
    cJSON *root = util_json_parse(json, len);
    if (!cJSON_IsObject(root)) {
        const char *at = cJSON_GetErrorPtr();
        snprintf(err, errlen, "invalid JSON%s%.40s", at ? " near: " : "", at ? at : "");
        cJSON_Delete(root);
        return NULL;
    }
    const cJSON *tabs = cJSON_GetObjectItem(root, "tabs");
    if (!cJSON_IsArray(tabs)) {
        snprintf(err, errlen, "layout needs a \"tabs\" array");
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

esp_err_t app_apply_json(const char *json, size_t len, bool persist, char *err, size_t errlen)
{
    cJSON *root = parse_layout(json, len, err, errlen);
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_apply_lock, portMAX_DELAY);
    if (persist) {
        size_t old_len;
        char *old = storage_read(APP_LAYOUT_PATH, &old_len);
        if (old) {
            storage_write(APP_BACKUP_PATH, old, old_len);
            free(old);
        }
        if (storage_write(APP_LAYOUT_PATH, json, len) != ESP_OK) {
            xSemaphoreGive(s_apply_lock);
            cJSON_Delete(root);
            snprintf(err, errlen, "could not save layout to flash");
            return ESP_FAIL;
        }
    }
    if (persist) {
        set_apply_attempts(1);    /* if this crashes, the next boot uses the backup */
    }
    apply_root(root);
    if (persist) {
        set_apply_attempts(0);
    }
    xSemaphoreGive(s_apply_lock);
    ESP_LOGI(TAG, "layout applied (%u bytes%s)", (unsigned)len, persist ? ", saved" : "");
    return ESP_OK;
}

esp_err_t app_rollback(char *err, size_t errlen)
{
    size_t blen = 0, clen = 0;
    char *backup = storage_read(APP_BACKUP_PATH, &blen);
    if (!backup) {
        snprintf(err, errlen, "no previous layout saved");
        return ESP_ERR_NOT_FOUND;
    }
    char *current = storage_read(APP_LAYOUT_PATH, &clen);
    cJSON *root = parse_layout(backup, blen, err, errlen);
    esp_err_t r = ESP_ERR_INVALID_ARG;
    if (root) {
        xSemaphoreTake(s_apply_lock, portMAX_DELAY);
        r = storage_write(APP_LAYOUT_PATH, backup, blen);
        if (r == ESP_OK && current) {
            storage_write(APP_BACKUP_PATH, current, clen);
        }
        if (r == ESP_OK) {
            apply_root(root);
        } else {
            cJSON_Delete(root);
            snprintf(err, errlen, "could not save layout to flash");
        }
        xSemaphoreGive(s_apply_lock);
    }
    free(backup);
    free(current);
    return r;
}

static void system_task(void *arg)
{
    for (;;) {
        sig_set_num("sys.uptime", (double)(esp_timer_get_time() / 1000000), false);
        sig_set_num("sys.heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024, false);
        sig_set_num("sys.psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024, false);
        net_tick();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void *json_malloc(size_t size)
{
    return util_malloc_psram(size);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Layout documents can be large; keep their JSON trees in PSRAM. */
    cJSON_Hooks hooks = { .malloc_fn = json_malloc, .free_fn = free };
    cJSON_InitHooks(&hooks);

    s_apply_lock = xSemaphoreCreateMutex();
    sig_init();
    storage_init();
    ESP_ERROR_CHECK(board_init());
    lvgl_port_lock(0);
    ui_engine_init(board_display());
    lvgl_port_unlock();
    actions_init();
    io_manager_init();
    net_init();     /* before the layout: MQTT and HTTP actions need the network stack */

    /* Load the stored layout, unless applying it crashed last time. */
    uint8_t attempts = apply_attempts();
    size_t len = 0;
    char *json = NULL;
    if (attempts == 0) {
        json = storage_read(APP_LAYOUT_PATH, &len);
    } else if (attempts == 1) {
        ESP_LOGE(TAG, "the last layout crashed the device; restoring the previous one");
        json = storage_read(APP_BACKUP_PATH, &len);
        if (json) {
            rename(APP_LAYOUT_PATH, "/data/layout.crashed.json");
            storage_write(APP_LAYOUT_PATH, json, len);
        }
    } else {
        ESP_LOGE(TAG, "stored layouts keep crashing; starting with the built-in layout");
        rename(APP_LAYOUT_PATH, "/data/layout.crashed.json");
    }
    set_apply_attempts(attempts < 255 ? attempts + 1 : 255);
    char msg[128] = "";
    if (!json || app_apply_json(json, len, false, msg, sizeof(msg)) != ESP_OK) {
        if (json) {
            ESP_LOGE(TAG, "stored layout rejected (%s), using the default layout", msg);
        }
        app_apply_json(app_default_layout(), strlen(app_default_layout()), false, msg, sizeof(msg));
    }
    free(json);
    set_apply_attempts(0);

    vTaskDelay(pdMS_TO_TICKS(100));    /* let LVGL draw the first frame before lighting up */
    board_backlight_set(true);

    web_server_start();
    xTaskCreatePinnedToCore(system_task, "system", 4096, NULL, 2, NULL, 0);

    /* We got this far, so a freshly flashed OTA image is good. */
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "ready");
}
