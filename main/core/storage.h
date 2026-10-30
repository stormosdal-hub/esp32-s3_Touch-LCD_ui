/* LittleFS on the "storage" partition, mounted at /data. */
#pragma once

#include <stddef.h>
#include "cJSON.h"
#include "esp_err.h"

esp_err_t storage_init(void);

/* Read a whole file into a NUL-terminated buffer (free() it). NULL if missing. */
char *storage_read(const char *path, size_t *len);

/* Write via a temporary file and rename, so a power cut never leaves half a file. */
esp_err_t storage_write(const char *path, const char *data, size_t len);

void storage_info_json(cJSON *obj);
