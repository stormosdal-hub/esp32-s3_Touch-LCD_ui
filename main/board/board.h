#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

/* Bring up I2C, CH422G, RGB panel, GT911 touch and the LVGL port. */
esp_err_t board_init(void);

i2c_master_bus_handle_t board_i2c_bus(void);
lv_display_t *board_display(void);

esp_err_t board_backlight_set(bool on);
bool board_backlight_get(void);

/* Route GPIO19/20 to the CAN transceiver (true) or to native USB (false). */
esp_err_t board_select_can(bool can);

/* True if `gpio` is free for user inputs/outputs (see BOARD_USER_GPIOS). */
bool board_gpio_is_user(int gpio);
