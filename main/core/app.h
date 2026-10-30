/* Layout lifecycle: load at boot, deploy from the web editor, roll back. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define APP_LAYOUT_PATH  "/data/layout.json"
#define APP_BACKUP_PATH  "/data/layout.bak.json"
#define APP_MAX_LAYOUT   (512 * 1024)

/*
 * Validate and apply a layout. If `persist`, it is saved first (the previous
 * layout becomes the backup). On error, `err` holds a message for the user.
 */
esp_err_t app_apply_json(const char *json, size_t len, bool persist, char *err, size_t errlen);

/* Swap the current and backup layouts and apply the result. */
esp_err_t app_rollback(char *err, size_t errlen);

/* The built-in example layout (NUL-terminated, static). */
const char *app_default_layout(void);
