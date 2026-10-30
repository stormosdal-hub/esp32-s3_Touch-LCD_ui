/* Shared between the io_* modules. Not part of the public API. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_oneshot.h"
#include "io_codec.h"
#include "signals.h"

#define IO_MAX_INPUTS 64

typedef enum {
    IN_GPIO,
    IN_ADC,
    IN_I2C,
    IN_SERIAL,
    IN_MODBUS,
    IN_CAN,
    IN_MQTT,
} in_type_t;

typedef struct {
    in_type_t type;
    char id[24];
    char signal[SIG_NAME_MAX];
    double scale, offset;
    int period_ms;
    int64_t next_due_us;
    char format[8];

    /* gpio */
    int pin;
    bool invert;
    int debounce_ms;
    int stable;            /* -1 until first read */
    int last_raw;
    int64_t raw_since_us;

    /* adc */
    adc_oneshot_unit_handle_t adc;
    adc_channel_t adc_chan;
    adc_cali_handle_t cali;
    bool millivolts;

    /* i2c / modbus / can */
    int addr;              /* i2c address or modbus slave */
    int reg;               /* register (-1 = none for i2c) */
    int len;
    int fn;                /* modbus function */
    i2c_master_dev_handle_t i2c_dev;
    uint32_t can_id;
    bool ext;
    int start;             /* first data byte (can) */

    /* serial / mqtt */
    char mode[8];          /* serial: line | kv | json */
    char topic[128];
    char json_key[32];
} io_input_t;

void io_cfg_lock(void);
void io_cfg_unlock(void);
/* Current inputs; only valid while io_cfg_lock() is held. */
io_input_t *io_inputs(int *count);

/* Apply scale/offset and store in the input's signal. */
void io_store(const io_input_t *in, double raw);
/* Set signals from a received text message according to mode line|kv|json. */
void io_store_text(const char *mode, const char *signal, const char *text);

void io_serial_init(void);
void io_serial_apply(const cJSON *port);
void io_serial_status(cJSON *obj);
bool io_serial_running(void);

void io_can_init(void);
void io_can_apply(const cJSON *port);
void io_can_status(cJSON *obj);
bool io_can_running(void);

void io_mqtt_init(void);
void io_mqtt_apply(const cJSON *port);
void io_mqtt_status(cJSON *obj);
