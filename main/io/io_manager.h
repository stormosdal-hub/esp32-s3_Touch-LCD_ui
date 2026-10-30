/*
 * Hardware inputs and outputs configured from the layout's "ports" and
 * "inputs" sections: GPIO, ADC, I2C polling, RS-485 (raw lines or Modbus RTU
 * master), CAN (TWAI) and MQTT.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"

esp_err_t io_manager_init(void);

/* Reconfigure ports and inputs from a layout. Safe to call repeatedly. */
void io_manager_apply(const cJSON *layout);

/* level: 0, 1, or -1 to toggle. Only pins in BOARD_USER_GPIOS are accepted. */
esp_err_t io_gpio_write(int pin, int level);
esp_err_t io_serial_send(const uint8_t *data, size_t len);
/* fn 5 (coil), 6 (register) or 16 (multiple registers, size from format). */
esp_err_t io_modbus_write(int slave, int fn, int reg, const char *format, double value);
esp_err_t io_i2c_write(int addr, const uint8_t *data, size_t len);
esp_err_t io_can_send(uint32_t id, bool ext, const uint8_t *data, size_t len);
esp_err_t io_mqtt_publish(const char *topic, const char *payload, int qos, bool retain);

/*
 * Set signals from a text message. mode "line": the whole text goes to `signal`;
 * "kv": "name=value" sets signal <signal><name>; "json": each key of an object.
 */
void io_set_from_text(const char *mode, const char *signal, const char *text);

/* Publish a changed signal to "<prefix>/<name>" if MQTT signal publishing is on. */
void io_mqtt_publish_signal(const char *name, const char *value);

/* Port status for /api/info. */
void io_status_json(cJSON *obj);
