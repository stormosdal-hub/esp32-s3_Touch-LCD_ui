#include "board.h"

#include "board_config.h"
#include "ch422g.h"
#include "esp_check.h"
#include "esp_idf_version.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

static i2c_master_bus_handle_t s_i2c;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static lv_display_t *s_disp;
static const int s_user_gpios[] = BOARD_USER_GPIOS;

static esp_err_t i2c_init(void)
{
    const i2c_master_bus_config_t cfg = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_i2c);
}

/*
 * Reset the GT911 through the expander while holding INT low, which makes it
 * answer on address 0x5D. Backlight stays off until LVGL has drawn a frame.
 */
static esp_err_t expander_and_touch_reset(void)
{
    uint8_t out = (1u << BOARD_EXIO_LCD_RST) | (1u << BOARD_EXIO_SD_CS) | (1u << BOARD_EXIO_USB_SEL);
    ESP_RETURN_ON_ERROR(ch422g_init(s_i2c, out), TAG, "CH422G init failed");

    const gpio_config_t int_out = {
        .pin_bit_mask = 1ULL << BOARD_TOUCH_GPIO_INT,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&int_out), TAG, "touch INT");
    gpio_set_level(BOARD_TOUCH_GPIO_INT, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(ch422g_set_pin(BOARD_EXIO_TP_RST, true), TAG, "touch reset release");
    vTaskDelay(pdMS_TO_TICKS(100));

    const gpio_config_t int_in = {
        .pin_bit_mask = 1ULL << BOARD_TOUCH_GPIO_INT,
        .mode = GPIO_MODE_INPUT,
    };
    return gpio_config(&int_in);
}

static esp_err_t lcd_init(void)
{
    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = BOARD_LCD_PCLK_HZ,
            .h_res = BOARD_LCD_H_RES,
            .v_res = BOARD_LCD_V_RES,
            .hsync_pulse_width = BOARD_LCD_HSYNC_PULSE,
            .hsync_back_porch = BOARD_LCD_HSYNC_BACK_PORCH,
            .hsync_front_porch = BOARD_LCD_HSYNC_FRONT_PORCH,
            .vsync_pulse_width = BOARD_LCD_VSYNC_PULSE,
            .vsync_back_porch = BOARD_LCD_VSYNC_BACK_PORCH,
            .vsync_front_porch = BOARD_LCD_VSYNC_FRONT_PORCH,
            .flags.pclk_active_neg = BOARD_LCD_PCLK_ACTIVE_NEG,
        },
        .data_width = 16,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
        .in_color_format = LCD_COLOR_FMT_RGB565,
#else
        .bits_per_pixel = 16,
#endif
        .num_fbs = 2,
        .bounce_buffer_size_px = BOARD_LCD_H_RES * CONFIG_APP_LCD_BOUNCE_LINES,
        .dma_burst_size = 64,
        .hsync_gpio_num = BOARD_LCD_GPIO_HSYNC,
        .vsync_gpio_num = BOARD_LCD_GPIO_VSYNC,
        .de_gpio_num = BOARD_LCD_GPIO_DE,
        .pclk_gpio_num = BOARD_LCD_GPIO_PCLK,
        .disp_gpio_num = GPIO_NUM_NC,
        .data_gpio_nums = BOARD_LCD_GPIO_DATA,
        .flags.fb_in_psram = 1,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&cfg, &s_panel), TAG, "RGB panel");
    return esp_lcd_panel_init(s_panel);
}

static esp_err_t touch_init(void)
{
    uint8_t addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    if (i2c_master_probe(s_i2c, addr, 50) != ESP_OK) {
        addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
        ESP_RETURN_ON_ERROR(i2c_master_probe(s_i2c, addr, 50), TAG, "GT911 not found on 0x5D or 0x14");
    }
    ESP_LOGI(TAG, "GT911 at 0x%02X", addr);

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.dev_addr = addr;
    io_cfg.scl_speed_hz = BOARD_I2C_FREQ_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c, &io_cfg, &io), TAG, "touch IO");

    esp_lcd_touch_io_gt911_config_t gt911_cfg = { .dev_addr = addr };
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,   /* reset is on the expander, done above */
        .int_gpio_num = GPIO_NUM_NC,   /* polled */
        .driver_data = &gt911_cfg,
    };
    return esp_lcd_touch_new_i2c_gt911(io, &tp_cfg, &s_touch);
}

static esp_err_t lvgl_init(void)
{
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 4;
    port_cfg.task_stack = 10 * 1024;
    port_cfg.task_affinity = 1;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "LVGL port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = s_panel,
        .buffer_size = BOARD_LCD_H_RES * BOARD_LCD_V_RES,
        .double_buffer = true,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .direct_mode = true,
        },
    };
    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = CONFIG_APP_LCD_BOUNCE_LINES > 0,
            .avoid_tearing = true,
        },
    };
    s_disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    ESP_RETURN_ON_FALSE(s_disp, ESP_FAIL, TAG, "LVGL display");

    if (s_touch) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = s_disp,
            .handle = s_touch,
        };
        ESP_RETURN_ON_FALSE(lvgl_port_add_touch(&touch_cfg), ESP_FAIL, TAG, "LVGL touch");
    }
    return ESP_OK;
}

esp_err_t board_init(void)
{
    ESP_RETURN_ON_ERROR(i2c_init(), TAG, "I2C");
    ESP_RETURN_ON_ERROR(expander_and_touch_reset(), TAG, "expander");
    ESP_RETURN_ON_ERROR(lcd_init(), TAG, "LCD");
    if (touch_init() != ESP_OK) {
        ESP_LOGE(TAG, "Touch init failed, continuing without touch");
        s_touch = NULL;
    }
    ESP_RETURN_ON_ERROR(lvgl_init(), TAG, "LVGL");
    return ESP_OK;
}

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return s_i2c;
}

lv_display_t *board_display(void)
{
    return s_disp;
}

esp_err_t board_backlight_set(bool on)
{
    return ch422g_set_pin(BOARD_EXIO_BL_EN, on);
}

bool board_backlight_get(void)
{
    return ch422g_get_pin(BOARD_EXIO_BL_EN);
}

esp_err_t board_select_can(bool can)
{
    bool level = can ? BOARD_USB_SEL_CAN_LEVEL : !BOARD_USB_SEL_CAN_LEVEL;
    return ch422g_set_pin(BOARD_EXIO_USB_SEL, level);
}

bool board_gpio_is_user(int gpio)
{
    for (size_t i = 0; i < sizeof(s_user_gpios) / sizeof(s_user_gpios[0]); i++) {
        if (s_user_gpios[i] == gpio) {
            return true;
        }
    }
    return false;
}
