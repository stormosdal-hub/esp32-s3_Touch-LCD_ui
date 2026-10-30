#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_littlefs.h"
#include "esp_log.h"
#include "util.h"

static const char *TAG = "storage";
static bool s_mounted;

esp_err_t storage_init(void)
{
    const esp_vfs_littlefs_conf_t conf = {
        .base_path = "/data",
        .partition_label = "storage",
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        return err;
    }
    s_mounted = true;
    size_t total = 0, used = 0;
    esp_littlefs_info("storage", &total, &used);
    ESP_LOGI(TAG, "LittleFS mounted: %u of %u bytes used", (unsigned)used, (unsigned)total);
    return ESP_OK;
}

char *storage_read(const char *path, size_t *len)
{
    FILE *f = s_mounted ? fopen(path, "rb") : NULL;
    if (!f) {
        return NULL;
    }
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || st.st_size <= 0) {
        fclose(f);
        return NULL;
    }
    char *buf = util_malloc_psram(st.st_size + 1);
    if (buf && fread(buf, 1, st.st_size, f) != (size_t)st.st_size) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        buf[st.st_size] = '\0';
        if (len) {
            *len = st.st_size;
        }
    }
    return buf;
}

esp_err_t storage_write(const char *path, const char *data, size_t len)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return ESP_FAIL;
    }
    size_t w = fwrite(data, 1, len, f);
    if (fclose(f) != 0 || w != len) {
        remove(tmp);
        return ESP_FAIL;
    }
    /* LittleFS renames over an existing file atomically; fall back to remove+rename. */
    if (rename(tmp, path) == 0) {
        return ESP_OK;
    }
    remove(path);
    return rename(tmp, path) == 0 ? ESP_OK : ESP_FAIL;
}

void storage_info_json(cJSON *obj)
{
    size_t total = 0, used = 0;
    if (s_mounted) {
        esp_littlefs_info("storage", &total, &used);
    }
    cJSON *s = cJSON_AddObjectToObject(obj, "storage");
    cJSON_AddNumberToObject(s, "total", total);
    cJSON_AddNumberToObject(s, "used", used);
}
