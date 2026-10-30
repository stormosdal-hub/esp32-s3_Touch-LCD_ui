/*
 * Pin map and panel timing for the Waveshare ESP32-S3-Touch-LCD-7 (800x480, ST7262 RGB panel,
 * GT911 touch, CH422G I/O expander).
 *
 * Values follow Waveshare's ESP-IDF demo for this board. If you have a different revision
 * (for example the 1024x600 "7B"), check the schematic and adjust this file.
 */
#pragma once

#include "driver/gpio.h"
#include "sdkconfig.h"

/* ---------- Display ---------- */
#define BOARD_LCD_H_RES             800
#define BOARD_LCD_V_RES             480
#define BOARD_LCD_PCLK_HZ           (CONFIG_APP_LCD_PCLK_MHZ * 1000 * 1000)
#define BOARD_LCD_HSYNC_PULSE       4
#define BOARD_LCD_HSYNC_BACK_PORCH  8
#define BOARD_LCD_HSYNC_FRONT_PORCH 8
#define BOARD_LCD_VSYNC_PULSE       4
#define BOARD_LCD_VSYNC_BACK_PORCH  8
#define BOARD_LCD_VSYNC_FRONT_PORCH 8
#define BOARD_LCD_PCLK_ACTIVE_NEG   1

#define BOARD_LCD_GPIO_VSYNC        GPIO_NUM_3
#define BOARD_LCD_GPIO_HSYNC        GPIO_NUM_46
#define BOARD_LCD_GPIO_DE           GPIO_NUM_5
#define BOARD_LCD_GPIO_PCLK         GPIO_NUM_7
/* RGB565 data lines D0..D15 = B3..B7, G2..G7, R3..R7 */
#define BOARD_LCD_GPIO_DATA { \
    GPIO_NUM_14, GPIO_NUM_38, GPIO_NUM_18, GPIO_NUM_17, GPIO_NUM_10,              \
    GPIO_NUM_39, GPIO_NUM_0,  GPIO_NUM_45, GPIO_NUM_48, GPIO_NUM_47, GPIO_NUM_21, \
    GPIO_NUM_1,  GPIO_NUM_2,  GPIO_NUM_42, GPIO_NUM_41, GPIO_NUM_40 }

/* ---------- I2C bus (touch, CH422G and the external I2C header) ---------- */
#define BOARD_I2C_PORT              0
#define BOARD_I2C_SDA               GPIO_NUM_8
#define BOARD_I2C_SCL               GPIO_NUM_9
#define BOARD_I2C_FREQ_HZ           400000

/* GT911 interrupt line. It is also used during reset to select the I2C address (low = 0x5D). */
#define BOARD_TOUCH_GPIO_INT        GPIO_NUM_4

/* ---------- CH422G expander outputs (EXIO0..EXIO7) ---------- */
#define BOARD_EXIO_TP_RST           1   /* touch reset (active low) */
#define BOARD_EXIO_BL_EN            2   /* backlight enable */
#define BOARD_EXIO_LCD_RST          3   /* LCD reset (active low) */
#define BOARD_EXIO_SD_CS            4   /* TF card chip select */
#define BOARD_EXIO_USB_SEL          5   /* routes GPIO19/20 to native USB or to the CAN transceiver */
/* Level of USB_SEL that connects GPIO19/20 to the CAN transceiver. Check your board's schematic. */
#define BOARD_USB_SEL_CAN_LEVEL     1

/* ---------- RS-485 (UART1, transceiver has automatic direction control) ---------- */
#define BOARD_RS485_UART            1
#define BOARD_RS485_TX              GPIO_NUM_16
#define BOARD_RS485_RX              GPIO_NUM_15

/* ---------- CAN (TWAI). Shares GPIO19/20 with native USB, see BOARD_EXIO_USB_SEL ---------- */
#define BOARD_CAN_TX                GPIO_NUM_20
#define BOARD_CAN_RX                GPIO_NUM_19

/* ---------- TF card (SPI) ---------- */
#define BOARD_SD_MOSI               GPIO_NUM_11
#define BOARD_SD_SCK                GPIO_NUM_12
#define BOARD_SD_MISO               GPIO_NUM_13

/*
 * GPIOs that user layouts may use for gpio/adc inputs and gpio actions.
 * GPIO6 is the "Sensor" ADC header. GPIO15/16 (RS-485), GPIO19/20 (USB/CAN) and
 * GPIO43/44 (UART0, console) are listed too, but only use them when the
 * peripheral that normally owns them is disabled.
 */
#define BOARD_USER_GPIOS { 6, 15, 16, 19, 20, 43, 44 }
