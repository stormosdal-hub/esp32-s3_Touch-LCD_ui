/*
 * WiFi: joins the configured network; if none is configured or it cannot be
 * reached, opens a setup access point "LCD-UI-XXXX". Also mDNS and SNTP.
 */
#pragma once

#include "cJSON.h"
#include "esp_err.h"

esp_err_t net_init(void);

/* Change the station network (saved in NVS) and reconnect. hostname may be NULL. */
esp_err_t net_set_sta(const char *ssid, const char *password, const char *hostname);

/* POSIX TZ string, e.g. "CET-1CEST,M3.5.0,M10.5.0/3". */
void net_set_timezone(const char *tz);

/* Called once per second: AP fallback handling and sys.ip / sys.wifi / sys.rssi signals. */
void net_tick(void);

void net_info_json(cJSON *obj);
esp_err_t net_scan_json(cJSON *arr);
