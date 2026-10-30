/*
 * Minimal CH422G I/O expander driver.
 *
 * The CH422G has no register map: each command is sent to its own I2C address.
 *   0x24  system parameters (bit0 = IO0..7 output enable)
 *   0x38  output levels for IO0..7
 */
#include "ch422g.h"

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define CH422G_ADDR_SYS     0x24
#define CH422G_ADDR_IO_OUT  0x38
#define CH422G_SYS_IO_OE    0x01

static const char *TAG = "ch422g";

static i2c_master_dev_handle_t s_sys;
static i2c_master_dev_handle_t s_out;
static uint8_t s_shadow;
static SemaphoreHandle_t s_lock;

static esp_err_t write_byte(i2c_master_dev_handle_t dev, uint8_t v)
{
    return i2c_master_transmit(dev, &v, 1, 100);
}

esp_err_t ch422g_init(i2c_master_bus_handle_t bus, uint8_t initial_outputs)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .scl_speed_hz = 400000,
    };
    cfg.device_address = CH422G_ADDR_SYS;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_sys), TAG, "add sys");
    cfg.device_address = CH422G_ADDR_IO_OUT;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_out), TAG, "add out");
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");

    ESP_RETURN_ON_ERROR(write_byte(s_sys, CH422G_SYS_IO_OE), TAG, "set IO output mode");
    s_shadow = initial_outputs;
    return write_byte(s_out, s_shadow);
}

esp_err_t ch422g_set_outputs(uint8_t value)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_shadow = value;
    esp_err_t err = write_byte(s_out, s_shadow);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t ch422g_set_pin(int pin, bool level)
{
    ESP_RETURN_ON_FALSE(pin >= 0 && pin < 8, ESP_ERR_INVALID_ARG, TAG, "bad pin");
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t v = level ? (s_shadow | (1u << pin)) : (s_shadow & ~(1u << pin));
    esp_err_t err = ESP_OK;
    if (v != s_shadow) {
        err = write_byte(s_out, v);
        if (err == ESP_OK) {
            s_shadow = v;
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

bool ch422g_get_pin(int pin)
{
    return (pin >= 0 && pin < 8) ? (s_shadow >> pin) & 1 : false;
}
