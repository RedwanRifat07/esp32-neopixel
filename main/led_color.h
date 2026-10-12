#pragma once
#include <stdint.h>

/* Pure helpers (no ESP-IDF deps) so they can be unit-tested on the host. */

typedef struct {
    uint8_t r, g, b;
} led_rgb_t;

/* Split 0xRRGGBB and scale each channel by pct/100 (pct clamped to 0..100).
 * Rounds to nearest so a non-zero channel at a low percentage is not silently zeroed
 * unless it truly rounds to 0. */
static inline led_rgb_t led_color_scaled(uint32_t rgb, unsigned pct)
{
    if (pct > 100) {
        pct = 100;
    }
    led_rgb_t c = {
        .r = (uint8_t)((((rgb >> 16) & 0xFF) * pct + 50) / 100),
        .g = (uint8_t)((((rgb >> 8) & 0xFF) * pct + 50) / 100),
        .b = (uint8_t)(((rgb & 0xFF) * pct + 50) / 100),
    };
    return c;
}
