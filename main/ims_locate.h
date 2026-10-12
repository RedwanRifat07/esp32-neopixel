#pragma once
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    const char *state;      /* "disabled" | "syncing" | "ok" | "auth_failed" | "error" */
    int64_t last_seq;       /* last event seq applied or learned at sync; -1 if none yet */
    int32_t last_ok_age_s;  /* seconds since the last successful IMS response; -1 if never */
} ims_status_t;

/* Start the long-poll task if CONFIG_SIOT_IMS_URL and CONFIG_SIOT_IMS_TOKEN are set; otherwise
 * the poller stays "disabled". At boot it learns the current seq from /locate/latest and lights
 * nothing; it then long-polls /locate/next and applies each event to the LEDs. Call after
 * led_glow_init() and once Wi-Fi is up. Only seq, drawer count and HTTP status are logged. */
esp_err_t ims_locate_start(void);

/* Snapshot of the poller state for /api/status. Safe from any task. */
void ims_locate_get_status(ims_status_t *out);
