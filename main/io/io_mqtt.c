/* MQTT client: "mqtt" inputs subscribe to topics, actions publish. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "io_internal.h"
#include "io_manager.h"
#include "mqtt_client.h"
#include "util.h"

static const char *TAG = "mqtt";

#define MAX_PAYLOAD 1024
#define PUBQ_LEN    32

typedef struct {
    char *topic;        /* full topic, or the signal name when is_signal */
    char *payload;
    uint8_t qos;
    bool retain;
    bool is_signal;     /* prefix with publish_prefix when sending */
} pub_job_t;

/*
 * s_lock guards s_client and s_publish_prefix. It is held while calling into
 * the client (which may block on the network), so only the publisher task and
 * io_mqtt_apply() take it, never the UI task or the MQTT event handler.
 */
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_pubq;
static esp_mqtt_client_handle_t s_client;
static char *s_signature;          /* config + topics, to detect changes */
static char s_publish_prefix[64];
static volatile bool s_publish_signals;
static volatile bool s_connected;
static volatile uint32_t s_rx, s_tx, s_dropped;

/* MQTT topic match with + and # wildcards. */
static bool topic_match(const char *filter, const char *topic, int tlen)
{
    const char *t = topic, *tend = topic + tlen;
    while (*filter) {
        if (*filter == '#') {
            return true;
        }
        if (*filter == '+') {
            while (t < tend && *t != '/') {
                t++;
            }
            filter++;
            continue;
        }
        if (t >= tend || *filter != *t) {
            return false;
        }
        filter++;
        t++;
    }
    return t == tend;
}

static void subscribe_all(esp_mqtt_client_handle_t client)
{
    io_cfg_lock();
    int n;
    io_input_t *in = io_inputs(&n);
    for (int i = 0; i < n; i++) {
        if (in[i].type == IN_MQTT) {
            esp_mqtt_client_subscribe(client, in[i].topic, 0);
        }
    }
    io_cfg_unlock();
}

static void handle_data(const esp_mqtt_event_t *ev)
{
    if (ev->current_data_offset != 0 || ev->data_len > MAX_PAYLOAD) {
        return;     /* only single-chunk messages are supported */
    }
    s_rx++;
    char *payload = malloc(ev->data_len + 1);
    if (!payload) {
        return;
    }
    memcpy(payload, ev->data, ev->data_len);
    payload[ev->data_len] = '\0';

    /* MQTT carries state, so unchanged values do not count as changes (avoids echo loops). */
    io_cfg_lock();
    int n;
    io_input_t *in = io_inputs(&n);
    for (int i = 0; i < n; i++) {
        if (in[i].type != IN_MQTT || !topic_match(in[i].topic, ev->topic, ev->topic_len)) {
            continue;
        }
        if (in[i].json_key[0]) {
            cJSON *j = util_json_parse(payload, ev->data_len);
            const cJSON *it = cJSON_GetObjectItem(j, in[i].json_key);
            if (cJSON_IsNumber(it)) {
                io_store(&in[i], it->valuedouble);
            } else if (cJSON_IsBool(it)) {
                io_store(&in[i], cJSON_IsTrue(it));
            } else if (cJSON_IsString(it)) {
                sig_set_text(in[i].signal, it->valuestring, false);
            }
            cJSON_Delete(j);
        } else {
            double v;
            if (util_parse_number(payload, &v, false)) {
                io_store(&in[i], v);
            } else {
                sig_set_text(in[i].signal, payload, false);
            }
        }
    }
    io_cfg_unlock();
    free(payload);
}

/* Runs in the MQTT task with the client's API lock held: must not take s_lock. */
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        sig_set_num("mqtt.connected", 1, false);
        subscribe_all(ev->client);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        sig_set_num("mqtt.connected", 0, false);
        break;
    case MQTT_EVENT_DATA:
        handle_data(ev);
        break;
    default:
        break;
    }
}

static void free_job(pub_job_t *job)
{
    free(job->topic);
    free(job->payload);
}

static void publisher_task(void *arg)
{
    pub_job_t job;
    for (;;) {
        if (xQueueReceive(s_pubq, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_client && s_connected) {
            char topic[192];
            const char *t = job.topic;
            if (job.is_signal) {
                snprintf(topic, sizeof(topic), "%s/%s", s_publish_prefix, job.topic);
                t = topic;
            }
            if (!job.is_signal || s_publish_prefix[0]) {
                if (esp_mqtt_client_enqueue(s_client, t, job.payload, 0, job.qos, job.retain, true) >= 0) {
                    s_tx++;
                } else {
                    s_dropped++;
                }
            }
        } else {
            s_dropped++;
        }
        xSemaphoreGive(s_lock);
        free_job(&job);
    }
}

static void queue_job(const char *topic, const char *payload, int qos, bool retain, bool is_signal)
{
    if (!s_pubq || !topic || !*topic) {
        return;
    }
    pub_job_t job = {
        .topic = strdup(topic),
        .payload = strdup(payload ? payload : ""),
        .qos = qos ? 1 : 0,
        .retain = retain,
        .is_signal = is_signal,
    };
    if (!job.topic || !job.payload || xQueueSend(s_pubq, &job, 0) != pdTRUE) {
        s_dropped++;
        free_job(&job);
    }
}

void io_mqtt_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_pubq = xQueueCreate(PUBQ_LEN, sizeof(pub_job_t));
    xTaskCreatePinnedToCore(publisher_task, "mqtt_pub", 4096, NULL, 3, NULL, 0);
}

/* Must be called without the io config lock held. */
void io_mqtt_apply(const cJSON *port)
{
    bool enabled = port && json_bool(port, "enabled", false) && *json_str(port, "uri", "");

    /* Build a signature of everything that requires a reconnect. */
    char *sig = NULL;
    if (enabled) {
        size_t cap = 512;
        io_cfg_lock();
        int n;
        io_input_t *in = io_inputs(&n);
        for (int i = 0; i < n; i++) {
            cap += strlen(in[i].topic) + 1;
        }
        sig = malloc(cap);
        if (sig) {
            int o = snprintf(sig, cap, "%s|%s|%s|%s", json_str(port, "uri", ""), json_str(port, "username", ""),
                             json_str(port, "password", ""), json_str(port, "client_id", ""));
            for (int i = 0; i < n && o < (int)cap; i++) {
                if (in[i].type == IN_MQTT) {
                    o += snprintf(sig + o, cap - o, "|%s", in[i].topic);
                }
            }
        }
        io_cfg_unlock();
        if (!sig) {
            return;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_publish_prefix, enabled ? json_str(port, "publish_prefix", "") : "", sizeof(s_publish_prefix));
    s_publish_signals = s_publish_prefix[0] != '\0';
    bool unchanged = sig && s_signature && !strcmp(sig, s_signature) && s_client;
    esp_mqtt_client_handle_t old = unchanged ? NULL : s_client;
    if (!unchanged) {
        s_client = NULL;
        s_connected = false;
    }
    xSemaphoreGive(s_lock);
    if (unchanged) {
        free(sig);
        return;     /* nothing changed, keep the connection */
    }

    /* Nobody can reach the old client any more; stop it outside the lock. */
    if (old) {
        esp_mqtt_client_stop(old);
        esp_mqtt_client_destroy(old);
        sig_set_num("mqtt.connected", 0, false);
    }
    free(s_signature);
    s_signature = sig;
    if (!enabled) {
        return;
    }

    const char *cid = json_str(port, "client_id", "");
    const char *user = json_str(port, "username", "");
    const char *pass = json_str(port, "password", "");
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = json_str(port, "uri", ""),
        .credentials.client_id = *cid ? cid : NULL,
        .credentials.username = *user ? user : NULL,
        .credentials.authentication.password = *pass ? pass : NULL,
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "invalid MQTT configuration");
        return;
    }
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, on_event, NULL);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_client = client;
    xSemaphoreGive(s_lock);
    esp_mqtt_client_start(client);
    ESP_LOGI(TAG, "connecting to %s", cfg.broker.address.uri);
}

esp_err_t io_mqtt_publish(const char *topic, const char *payload, int qos, bool retain)
{
    if (!s_client) {
        return ESP_ERR_INVALID_STATE;
    }
    queue_job(topic, payload, qos, retain, false);
    return ESP_OK;
}

void io_mqtt_publish_signal(const char *name, const char *value)
{
    if (!s_publish_signals || !s_connected || !strncmp(name, "sys.", 4) || !strncmp(name, "mqtt.", 5)) {
        return;
    }
    queue_job(name, value, 0, true, true);
}

void io_mqtt_status(cJSON *obj)
{
    cJSON *p = cJSON_AddObjectToObject(obj, "mqtt");
    cJSON_AddBoolToObject(p, "running", s_client != NULL);
    cJSON_AddBoolToObject(p, "connected", s_connected);
    cJSON_AddNumberToObject(p, "rx", s_rx);
    cJSON_AddNumberToObject(p, "tx", s_tx);
    cJSON_AddNumberToObject(p, "dropped", s_dropped);
}
