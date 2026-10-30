/* MQTT client: "mqtt" inputs subscribe to topics, actions publish. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "io_internal.h"
#include "io_manager.h"
#include "mqtt_client.h"
#include "util.h"

static const char *TAG = "mqtt";

#define MAX_PAYLOAD 1024

static esp_mqtt_client_handle_t s_client;
static char *s_signature;          /* config + topics, to detect changes */
static char s_publish_prefix[64];
static bool s_connected;
static uint32_t s_rx, s_tx;

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

static void subscribe_all(void)
{
    io_cfg_lock();
    int n;
    io_input_t *in = io_inputs(&n);
    for (int i = 0; i < n; i++) {
        if (in[i].type == IN_MQTT) {
            esp_mqtt_client_subscribe(s_client, in[i].topic, 0);
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

    io_cfg_lock();
    int n;
    io_input_t *in = io_inputs(&n);
    for (int i = 0; i < n; i++) {
        if (in[i].type != IN_MQTT || !topic_match(in[i].topic, ev->topic, ev->topic_len)) {
            continue;
        }
        if (in[i].json_key[0]) {
            cJSON *j = cJSON_Parse(payload);
            const cJSON *it = cJSON_GetObjectItem(j, in[i].json_key);
            if (cJSON_IsNumber(it)) {
                io_store(&in[i], it->valuedouble);
            } else if (cJSON_IsBool(it)) {
                io_store(&in[i], cJSON_IsTrue(it));
            } else if (cJSON_IsString(it)) {
                sig_set_text(in[i].signal, it->valuestring, true);
            }
            cJSON_Delete(j);
        } else {
            double v;
            if (util_parse_number(payload, &v, false)) {
                io_store(&in[i], v);
            } else {
                sig_set_text(in[i].signal, payload, true);
            }
        }
    }
    io_cfg_unlock();
    free(payload);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        sig_set_num("mqtt.connected", 1, false);
        subscribe_all();
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

static void stop_client(void)
{
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        s_connected = false;
        sig_set_num("mqtt.connected", 0, false);
    }
}

/* Must be called without the io config lock held. */
void io_mqtt_apply(const cJSON *port)
{
    bool enabled = port && json_bool(port, "enabled", false) && *json_str(port, "uri", "");
    strlcpy(s_publish_prefix, enabled ? json_str(port, "publish_prefix", "") : "", sizeof(s_publish_prefix));

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
        if (!sig) {
            io_cfg_unlock();
            return;
        }
        int o = snprintf(sig, cap, "%s|%s|%s|%s", json_str(port, "uri", ""), json_str(port, "username", ""),
                         json_str(port, "password", ""), json_str(port, "client_id", ""));
        for (int i = 0; i < n && o < (int)cap; i++) {
            if (in[i].type == IN_MQTT) {
                o += snprintf(sig + o, cap - o, "|%s", in[i].topic);
            }
        }
        io_cfg_unlock();
    }
    if (sig && s_signature && !strcmp(sig, s_signature) && s_client) {
        free(sig);
        return;     /* nothing changed, keep the connection */
    }
    stop_client();
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
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "invalid MQTT configuration");
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "connecting to %s", cfg.broker.address.uri);
}

esp_err_t io_mqtt_publish(const char *topic, const char *payload, int qos, bool retain)
{
    if (!s_client || !topic || !*topic) {
        return ESP_ERR_INVALID_STATE;
    }
    int r = esp_mqtt_client_enqueue(s_client, topic, payload, 0, qos, retain, true);
    if (r < 0) {
        return ESP_FAIL;
    }
    s_tx++;
    return ESP_OK;
}

void io_mqtt_publish_signal(const char *name, const char *value)
{
    if (!s_publish_prefix[0] || !s_connected || !strncmp(name, "sys.", 4)) {
        return;
    }
    char topic[128];
    snprintf(topic, sizeof(topic), "%s/%s", s_publish_prefix, name);
    io_mqtt_publish(topic, value, 0, true);
}

void io_mqtt_status(cJSON *obj)
{
    cJSON *p = cJSON_AddObjectToObject(obj, "mqtt");
    cJSON_AddBoolToObject(p, "running", s_client != NULL);
    cJSON_AddBoolToObject(p, "connected", s_connected);
    cJSON_AddNumberToObject(p, "rx", s_rx);
    cJSON_AddNumberToObject(p, "tx", s_tx);
}
