/*
 * Builds the LVGL screen from a layout JSON document and keeps widgets in sync
 * with signals. All functions must be called with the LVGL port lock held.
 */
#pragma once

#include "cJSON.h"
#include "lvgl.h"

void ui_engine_init(lv_display_t *disp);

/* Replace the current UI. Takes ownership of `layout` (freed on the next build). */
void ui_engine_build(cJSON *layout);

/* Select a tab by id, title or index. */
void ui_engine_show_tab(const char *tab);

/* Append a timestamped line to every log widget. */
void ui_engine_log(const char *text);
