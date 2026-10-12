#pragma once
#include "esp_err.h"
#include "sdkconfig.h"

/* Total drawers: SIOT_LED_COUNT pixels on SIOT_LED_GPIO, then SIOT_LED2_COUNT on SIOT_LED2_GPIO. */
#define LED_GLOW_DRAWERS (CONFIG_SIOT_LED_COUNT + CONFIG_SIOT_LED2_COUNT)

/* Initialise the NeoPixel strip (RMT) and the glow task. All pixels start off.
 * Drawer n (1..LED_GLOW_DRAWERS) maps to pixel n-1 of the first strip, continuing on the second. Colour, brightness and glow
 * duration come from CONFIG_SIOT_*. Only the glow task touches the strip. */
esp_err_t led_glow_init(void);

/* Glow one drawer for CONFIG_SIOT_GLOW_MS. Non-blocking, safe from any task. Several
 * drawers can glow at once, each with its own timer; re-triggering a glowing drawer
 * restarts its timer.
 * Returns ESP_ERR_INVALID_ARG for a drawer outside 1..LED_GLOW_DRAWERS, ESP_ERR_INVALID_STATE
 * before init, ESP_ERR_TIMEOUT if the command queue is full. */
esp_err_t led_glow_trigger(unsigned drawer);

/* Turn every drawer off now. Same return codes as led_glow_trigger (minus INVALID_ARG). */
esp_err_t led_glow_clear_all(void);
