#pragma once

#include "esp_err.h"

esp_err_t web_server_start(void);

/* Send a text frame to every connected WebSocket client (non-blocking, copies `text`). */
void web_ws_broadcast(const char *text);
