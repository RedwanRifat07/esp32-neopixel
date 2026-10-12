#pragma once
#include "esp_err.h"

/* Start the drawer-test HTTP server on port 80 (call after Wi-Fi has an IP).
 *   GET  /              test page
 *   GET  /api/status    {"drawers":N,"glow_ms":M,"auth":bool}      (no auth)
 *   POST /api/glow      {"drawer":n}   -> glow drawer n (1..N)
 *   POST /api/off       (empty body)   -> all drawers off
 * POSTs require Content-Type: application/json (blocks cross-site form posts) and, if
 * CONFIG_SIOT_WEB_TOKEN is non-empty, a matching X-Auth-Token header. */
esp_err_t web_server_start(void);
