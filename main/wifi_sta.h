#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Bring up Wi-Fi station mode and connect using CONFIG_SIOT_WIFI_*. Returns immediately;
 * reconnects forever with exponential backoff (1 s .. 30 s). Call once, after
 * nvs_flash_init(). */
esp_err_t wifi_sta_start(void);

/* Block until an IP address is obtained or timeout_ms elapses. */
bool wifi_sta_wait_connected(uint32_t timeout_ms);
