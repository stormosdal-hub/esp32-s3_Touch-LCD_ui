#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs.h"
#include "signals.h"
#include "util.h"

static const char *TAG = "net";

#define NVS_NS              "net"
#define AP_FALLBACK_S       20      /* open the setup AP after this long without a connection */
#define AP_CLOSE_S          30      /* close it this long after the station got an IP */
#define STA_RETRY_AP_S      30      /* station retry interval while the AP is up */

static esp_netif_t *s_sta, *s_ap;
static char s_ssid[33], s_pass[65], s_host[33], s_ap_ssid[33];
static bool s_connected, s_ap_on;
static int64_t s_state_since;       /* time of last connect / disconnect (us) */
static int s_retry_countdown;
static char s_ip[16] = "0.0.0.0";

static void nvs_get(nvs_handle_t h, const char *key, char *out, size_t len, const char *def)
{
    size_t l = len;
    if (nvs_get_str(h, key, out, &l) != ESP_OK) {
        strlcpy(out, def, len);
    }
}

static void load_config(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get(h, "ssid", s_ssid, sizeof(s_ssid), CONFIG_APP_WIFI_SSID);
        nvs_get(h, "pass", s_pass, sizeof(s_pass), CONFIG_APP_WIFI_PASSWORD);
        nvs_get(h, "host", s_host, sizeof(s_host), CONFIG_APP_HOSTNAME);
        nvs_close(h);
    } else {
        strlcpy(s_ssid, CONFIG_APP_WIFI_SSID, sizeof(s_ssid));
        strlcpy(s_pass, CONFIG_APP_WIFI_PASSWORD, sizeof(s_pass));
        strlcpy(s_host, CONFIG_APP_HOSTNAME, sizeof(s_host));
    }
}

static void set_ap(bool on)
{
    if (on == s_ap_on) {
        return;
    }
    s_ap_on = on;
    esp_wifi_set_mode(on ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    ESP_LOGI(TAG, "setup access point %s", on ? "on" : "off");
}

static void apply_sta_config(void)
{
    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, s_ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, s_pass, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = s_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;
    esp_wifi_set_config(WIFI_IF_STA, &sta);
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_ssid[0]) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_connected) {
            s_connected = false;
            s_state_since = esp_timer_get_time();
            strlcpy(s_ip, "0.0.0.0", sizeof(s_ip));
        }
        /* Reconnecting makes the radio scan, which disturbs clients on the setup AP. */
        if (s_ap_on) {
            s_retry_countdown = STA_RETRY_AP_S;
        } else if (s_ssid[0]) {
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        s_state_since = esp_timer_get_time();
        ESP_LOGI(TAG, "connected to '%s', IP %s, http://%s.local", s_ssid, s_ip, s_host);
    }
}

esp_err_t net_init(void)
{
    load_config();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta, s_host);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "LCD-UI-%02X%02X", mac[4], mac[5]);
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_ap_ssid);
    strlcpy((char *)ap.ap.password, CONFIG_APP_AP_PASSWORD, sizeof(ap.ap.password));
    ap.ap.authmode = strlen(CONFIG_APP_AP_PASSWORD) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ap.ap.max_connection = 4;
    ap.ap.channel = 1;

    s_ap_on = !s_ssid[0];
    ESP_ERROR_CHECK(esp_wifi_set_mode(s_ap_on ? WIFI_MODE_APSTA : WIFI_MODE_STA));
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    apply_sta_config();
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);     /* lower latency for the web editor / WebSocket */
    s_state_since = esp_timer_get_time();

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(s_host);
        mdns_instance_name_set("ESP32-S3 LCD UI");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);
    ESP_LOGI(TAG, "WiFi started (%s)", s_ssid[0] ? s_ssid : "no network configured, setup AP only");
    return ESP_OK;
}

static void reconnect_cb(void *arg)
{
    esp_wifi_disconnect();
    apply_sta_config();
    esp_netif_set_hostname(s_sta, s_host);
    mdns_hostname_set(s_host);
    if (s_ssid[0]) {
        esp_wifi_connect();
    }
}

esp_err_t net_set_sta(const char *ssid, const char *password, const char *hostname)
{
    if (!ssid || strlen(ssid) > 32 || (password && strlen(password) > 64)) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, password ? password : "", sizeof(s_pass));
    if (hostname && *hostname) {
        strlcpy(s_host, hostname, sizeof(s_host));
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", s_ssid);
        nvs_set_str(h, "pass", s_pass);
        nvs_set_str(h, "host", s_host);
        nvs_commit(h);
        nvs_close(h);
    }
    /* Reconnect shortly, after the HTTP response has been sent. */
    static esp_timer_handle_t t;
    if (!t) {
        const esp_timer_create_args_t args = { .callback = reconnect_cb, .name = "wifi" };
        esp_timer_create(&args, &t);
    }
    if (t) {
        esp_timer_stop(t);
        esp_timer_start_once(t, 500 * 1000);
    }
    return ESP_OK;
}

void net_set_timezone(const char *tz)
{
    setenv("TZ", (tz && *tz) ? tz : "UTC0", 1);
    tzset();
}

void net_tick(void)
{
    int64_t since_s = (esp_timer_get_time() - s_state_since) / 1000000;
    if (!s_connected && !s_ap_on && since_s >= AP_FALLBACK_S) {
        set_ap(true);
    }
    if (s_connected && s_ap_on && since_s >= AP_CLOSE_S) {
        wifi_sta_list_t list;
        if (esp_wifi_ap_get_sta_list(&list) != ESP_OK || list.num == 0) {
            set_ap(false);
        }
    }
    if (!s_connected && s_ap_on && s_ssid[0] && s_retry_countdown > 0 && --s_retry_countdown == 0) {
        esp_wifi_connect();
    }

    int rssi = 0;
    if (s_connected) {
        esp_wifi_sta_get_rssi(&rssi);
    }
    sig_set_num("sys.rssi", rssi, false);
    sig_set_text("sys.ip", s_ip, false);
    char wifi[160];
    if (s_connected) {
        snprintf(wifi, sizeof(wifi), "%s  http://%s.local  (%s)", s_ssid, s_host, s_ip);
    } else if (s_ap_on) {
        snprintf(wifi, sizeof(wifi), "Setup WiFi: %s  pass: %s  http://192.168.4.1", s_ap_ssid,
                 strlen(CONFIG_APP_AP_PASSWORD) >= 8 ? CONFIG_APP_AP_PASSWORD : "(open)");
    } else {
        snprintf(wifi, sizeof(wifi), "Connecting to %s ...", s_ssid);
    }
    sig_set_text("sys.wifi", wifi, false);

    time_t now = time(NULL);
    if (now > 1600000000) {
        struct tm tm;
        localtime_r(&now, &tm);
        char buf[24];
        strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
        sig_set_text("sys.time", buf, false);
        strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
        sig_set_text("sys.date", buf, false);
    }
}

void net_info_json(cJSON *obj)
{
    cJSON *n = cJSON_AddObjectToObject(obj, "wifi");
    cJSON_AddStringToObject(n, "ssid", s_ssid);
    cJSON_AddBoolToObject(n, "connected", s_connected);
    cJSON_AddStringToObject(n, "ip", s_ip);
    cJSON_AddStringToObject(n, "hostname", s_host);
    cJSON_AddBoolToObject(n, "ap_active", s_ap_on);
    cJSON_AddStringToObject(n, "ap_ssid", s_ap_ssid);
}

esp_err_t net_scan_json(cJSON *arr)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        return err;
    }
    uint16_t n = 20;
    wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
    if (!recs) {
        return ESP_ERR_NO_MEM;
    }
    if (esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
        for (int i = 0; i < n; i++) {
            if (!recs[i].ssid[0]) {
                continue;
            }
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "ssid", (const char *)recs[i].ssid);
            cJSON_AddNumberToObject(o, "rssi", recs[i].rssi);
            cJSON_AddBoolToObject(o, "secure", recs[i].authmode != WIFI_AUTH_OPEN);
            cJSON_AddItemToArray(arr, o);
        }
    }
    free(recs);
    return ESP_OK;
}
