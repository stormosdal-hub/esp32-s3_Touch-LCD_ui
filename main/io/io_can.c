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

#define TX_QUEUE_DEPTH 8
#define TX_SLOTS       (TX_QUEUE_DEPTH + 2)   /* queued + in flight + spare */

typedef struct {
    uint32_t id;
    bool ext;
    uint8_t len;
    uint8_t data[8];
} can_msg_t;

/*
 * The driver keeps a pointer to each queued frame until it has been sent, so
 * frames live in this pool and are released from the TX-done callback.
 */
typedef struct {
    twai_frame_t frame;
    uint8_t data[8];
    volatile bool busy;
} tx_slot_t;

static twai_node_handle_t s_node;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_rxq;
static tx_slot_t s_tx[TX_SLOTS];
static int s_bitrate;
static bool s_log_raw;
static volatile bool s_bus_off;
static volatile uint32_t s_rx_count, s_tx_count, s_tx_err;

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

static bool on_tx_done(twai_node_handle_t node, const twai_tx_done_event_data_t *edata, void *ctx)
{
    for (int i = 0; i < TX_SLOTS; i++) {
        if (&s_tx[i].frame == edata->done_tx_frame) {
            s_tx[i].busy = false;
            break;
        }
    }
    if (edata->is_tx_success) {
        s_tx_count++;
    } else {
        s_tx_err++;
    }
    return false;
}

static bool on_state_change(twai_node_handle_t node, const twai_state_change_event_data_t *edata, void *ctx)
{
    if (edata->new_sta == TWAI_ERROR_BUS_OFF) {
        s_bus_off = true;
    }
    return false;
}

static void dispatch(const can_msg_t *m)
{
    s_rx_count++;
    if (s_log_raw) {
        char txt[48];
        int o = snprintf(txt, sizeof(txt), m->ext ? "%08lX#" : "%03lX#", (unsigned long)m->id);
        for (int i = 0; i < m->len && o < (int)sizeof(txt) - 3; i++) {
            o += snprintf(txt + o, sizeof(txt) - o, "%02X", m->data[i]);
        }
        sig_set_text("can.rx", txt, true);
    }
    io_cfg_lock();
    int n;
    io_input_t *in = io_inputs(&n);
    for (int i = 0; i < n; i++) {
        if (in[i].type != IN_CAN || in[i].can_id != m->id || in[i].ext != m->ext) {
            continue;
        }
        if (!strcmp(in[i].format, "hex")) {
            char txt[24] = "";
            for (int b = 0; b < m->len; b++) {
                snprintf(txt + b * 2, sizeof(txt) - b * 2, "%02X", m->data[b]);
            }
            sig_set_text(in[i].signal, txt, true);
            continue;
        }
        double v;
        int start = in[i].start;
        if (start >= 0 && start < m->len && io_decode(m->data + start, m->len - start, in[i].format, &v)) {
            io_store(&in[i], v);
        }
    }
    io_cfg_unlock();
}

static void can_task(void *arg)
{
    can_msg_t m;
    for (;;) {
        if (xQueueReceive(s_rxq, &m, pdMS_TO_TICKS(1000)) == pdTRUE) {
            dispatch(&m);
        }
        if (s_bus_off) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_node && s_bus_off) {
                ESP_LOGW(TAG, "bus-off, recovering");
                s_bus_off = false;
                twai_node_recover(s_node);
            }
            xSemaphoreGive(s_lock);
        }
    }
}

/* Caller holds s_lock. */
static void stop_node(void)
{
    if (s_node) {
        twai_node_disable(s_node);
        twai_node_delete(s_node);
        s_node = NULL;
        board_select_can(false);
    }
    for (int i = 0; i < TX_SLOTS; i++) {
        s_tx[i].busy = false;    /* the deleted node no longer references them */
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
            .fail_retry_cnt = 3,
            .tx_queue_depth = TX_QUEUE_DEPTH,
        };
        twai_event_callbacks_t cbs = {
            .on_rx_done = on_rx,
            .on_tx_done = on_tx_done,
            .on_state_change = on_state_change,
        };
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
            s_bus_off = false;
            ESP_LOGI(TAG, "CAN running at %d bit/s", bitrate);
        }
    }
    xSemaphoreGive(s_lock);
}

bool io_can_running(void)
{
    return s_node != NULL;
}

esp_err_t io_can_send(uint32_t id, bool ext, const uint8_t *data, size_t len)
{
    if (len > 8) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_node) {
        tx_slot_t *slot = NULL;
        for (int i = 0; i < TX_SLOTS && !slot; i++) {
            if (!s_tx[i].busy) {
                slot = &s_tx[i];
            }
        }
        if (!slot) {
            err = ESP_ERR_NO_MEM;     /* everything still queued: bus stuck or nobody acknowledges */
        } else {
            memcpy(slot->data, data, len);
            memset(&slot->frame, 0, sizeof(slot->frame));
            slot->frame.header.id = id;
            slot->frame.header.ide = ext;
            slot->frame.header.dlc = len;
            slot->frame.buffer = slot->data;
            slot->frame.buffer_len = len;
            slot->busy = true;
            err = twai_node_transmit(s_node, &slot->frame, 50);
            if (err != ESP_OK) {
                slot->busy = false;
            }
        }
    }
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
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
