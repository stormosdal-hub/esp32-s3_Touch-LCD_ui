#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

/* Put the expander in output mode and drive IO0..7 to initial_outputs. */
esp_err_t ch422g_init(i2c_master_bus_handle_t bus, uint8_t initial_outputs);
esp_err_t ch422g_set_outputs(uint8_t value);
esp_err_t ch422g_set_pin(int pin, bool level);
/* Last written output level (the CH422G cannot read back outputs). */
bool ch422g_get_pin(int pin);
