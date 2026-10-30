/* CAN bus (TWAI) input/output using the TWAI node driver (ESP-IDF >= 5.5). */
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "io_internal.h"
#include "io_manager.h"
#include "util.h"

static const char *TAG = "can";

typedef struct {
    uint32_t id;
    bool ext;
    uint8_t len;
    uint8_t data[8];
} can_msg_t;

static twai_node_handle_t s_node;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_rxq;
static int s_bitrate;
static bool s_log_raw;
static uint32_t s_rx_count, s_tx_count, s_tx_err;

static bool on_rx(twai_node_handle_t node, const twai_rx_done_event_data_t *edata, void *ctx)
{
    can_msg_t m = {0};
    twai_frame_t f = { .buffer = m.data, .buffer_len = sizeof(m.data) };
    if (twai_node_receive_from_isr(node, &f) != ESP_OK) {
        return false;
    }
    m.id = f.header.id;
    m.ext = f.header.ide;
    m.len = f.header.dlc > 8 ? 8 : f.header.dlc;
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_rxq, &m, &woken);
    return woken == pdTRUE;
}

static void can_task(void *arg)
{
    can_msg_t m;
    for (;;) {
        if (xQueueReceive(s_rxq, &m, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        s_rx_count++;
        if (s_log_raw) {
            char txt[48];
            int o = snprintf(txt, sizeof(txt), m.ext ? "%08lX#" : "%03lX#", (unsigned long)m.id);
            for (int i = 0; i < m.len && o < (int)sizeof(txt) - 3; i++) {
                o += snprintf(txt + o, sizeof(txt) - o, "%02X", m.data[i]);
            }
            sig_set_text("can.rx", txt, true);
        }
        io_cfg_lock();
        int n;
        io_input_t *in = io_inputs(&n);
        for (int i = 0; i < n; i++) {
            if (in[i].type != IN_CAN || in[i].can_id != m.id || in[i].ext != m.ext) {
                continue;
            }
            if (!strcmp(in[i].format, "hex")) {
                char txt[24] = "";
                for (int b = 0; b < m.len; b++) {
                    snprintf(txt + b * 2, sizeof(txt) - b * 2, "%02X", m.data[b]);
                }
                sig_set_text(in[i].signal, txt, true);
                continue;
            }
            double v;
            int start = in[i].start;
            if (start >= 0 && start < m.len && io_decode(m.data + start, m.len - start, in[i].format, &v)) {
                io_store(&in[i], v);
            }
        }
        io_cfg_unlock();
    }
}

static void stop_node(void)
{
    if (s_node) {
        twai_node_disable(s_node);
        twai_node_delete(s_node);
        s_node = NULL;
        board_select_can(false);
    }
}

void io_can_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_rxq = xQueueCreate(32, sizeof(can_msg_t));
    xTaskCreatePinnedToCore(can_task, "can", 4096, NULL, 4, NULL, 0);
}

void io_can_apply(const cJSON *port)
{
    bool enabled = port && json_bool(port, "enabled", false);
    int bitrate = json_int(port, "bitrate", 500000);
    s_log_raw = json_bool(port, "log_raw", false);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_node && (!enabled || bitrate != s_bitrate)) {
        stop_node();
    }
    if (enabled && !s_node) {
        board_select_can(true);
        twai_onchip_node_config_t cfg = {
            .io_cfg = {
                .tx = BOARD_CAN_TX,
                .rx = BOARD_CAN_RX,
                .quanta_clk_out = GPIO_NUM_NC,
                .bus_off_indicator = GPIO_NUM_NC,
            },
            .bit_timing = { .bitrate = bitrate },
            .tx_queue_depth = 8,
        };
        twai_event_callbacks_t cbs = { .on_rx_done = on_rx };
        esp_err_t err = twai_new_node_onchip(&cfg, &s_node);
        if (err == ESP_OK) {
            err = twai_node_register_event_callbacks(s_node, &cbs, NULL);
        }
        if (err == ESP_OK) {
            err = twai_node_enable(s_node);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "CAN start failed: %s", esp_err_to_name(err));
            if (s_node) {
                twai_node_delete(s_node);
                s_node = NULL;
            }
            board_select_can(false);
        } else {
            s_bitrate = bitrate;
            ESP_LOGI(TAG, "CAN running at %d bit/s", bitrate);
        }
    }
    xSemaphoreGive(s_lock);
}

esp_err_t io_can_send(uint32_t id, bool ext, const uint8_t *data, size_t len)
{
    if (len > 8) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t buf[8];
    memcpy(buf, data, len);
    twai_frame_t f = {
        .header = { .id = id, .ide = ext, .dlc = len },
        .buffer = buf,
        .buffer_len = len,
    };
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_node) {
        err = twai_node_transmit(s_node, &f, 100);
        if (err == ESP_OK) {
            /* the frame lives on our stack, so wait until it has left */
            err = twai_node_transmit_wait_all_done(s_node, 200);
        }
    }
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        s_tx_count++;
    } else {
        s_tx_err++;
        ESP_LOGW(TAG, "CAN send 0x%lX failed: %s", (unsigned long)id, esp_err_to_name(err));
    }
    return err;
}

void io_can_status(cJSON *obj)
{
    cJSON *p = cJSON_AddObjectToObject(obj, "can");
    cJSON_AddBoolToObject(p, "running", s_node != NULL);
    cJSON_AddNumberToObject(p, "bitrate", s_bitrate);
    cJSON_AddNumberToObject(p, "rx", s_rx_count);
    cJSON_AddNumberToObject(p, "tx", s_tx_count);
    cJSON_AddNumberToObject(p, "tx_errors", s_tx_err);
}
