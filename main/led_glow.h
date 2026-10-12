#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "locate_apply.h"
#include "sdkconfig.h"


/* Initialise the NeoPixel strip (RMT) and the glow task. All pixels start off.
 * Drawer n (1..CONFIG_SIOT_LED_COUNT) maps to pixel n-1; CONFIG_SIOT_DRAWER_MAP names the physical drawer of each pixel. Colour, brightness and glow
 * duration come from CONFIG_SIOT_*. Only the glow task touches the strip. */
esp_err_t led_glow_init(void);

/* Glow one drawer for CONFIG_SIOT_GLOW_MS. Non-blocking, safe from any task. Several
 * drawers can glow at once, each with its own timer; re-triggering a glowing drawer
 * restarts its timer.
 * Returns ESP_ERR_INVALID_ARG for a drawer outside 1..CONFIG_SIOT_LED_COUNT, ESP_ERR_INVALID_STATE
 * before init, ESP_ERR_TIMEOUT if the command queue is full. */
esp_err_t led_glow_trigger(unsigned drawer);

/* Turn every drawer off now. Same return codes as led_glow_trigger (minus INVALID_ARG). */
esp_err_t led_glow_clear_all(void);

/* Replace the whole light set: pixel i takes states[i] (off/green/red; n must equal
 * CONFIG_SIOT_LED_COUNT). Lit pixels switch off after ttl_ms. Colours are CONFIG_SIOT_GREEN_RGB /
 * CONFIG_SIOT_RED_RGB at CONFIG_SIOT_GLOW_BRIGHTNESS_PCT. Non-blocking, safe from any task.
 * Returns ESP_ERR_INVALID_ARG for a bad n or ttl_ms == 0, ESP_ERR_INVALID_STATE before init,
 * ESP_ERR_TIMEOUT if the command queue is full. */
esp_err_t led_glow_set_states(const led_state_t *states, size_t n, uint32_t ttl_ms);
