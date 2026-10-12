#pragma once
#include <stddef.h>
#include <stdint.h>

/* Pure event-to-pixels logic (no ESP-IDF deps) so it can be unit-tested on the host. */

typedef enum { LED_STATE_OFF = 0, LED_STATE_GREEN = 1, LED_STATE_RED = 2 } led_state_t;

#define LOCATE_MAX_PIXELS 30
#define LOCATE_DEFAULT_TTL_MS 15000
#define LOCATE_MIN_TTL_MS 100
#define LOCATE_MAX_TTL_MS 3600000

typedef enum { LOCATE_OK = 0, LOCATE_BAD_JSON, LOCATE_BAD_EVENT, LOCATE_BAD_ARGS } locate_rc_t;

typedef struct {
    int64_t seq;
    uint32_t ttl_ms;
    unsigned drawer_count; /* entries in the event's "drawers" array, for logging */
} locate_info_t;

/* Apply one IMS locate event for this board.
 *   json/len  the event body (not required to be NUL-terminated)
 *   cabinet   this board's cabinet letter ('A' or 'B')
 *   map_csv   physical drawer of each pixel in pixel order, e.g. "A1,A2,A3,A4,A5"
 *   states    out: n_pixels entries, written only when LOCATE_OK
 *   info      out: seq, ttl and drawer count, written when the event parses (LOCATE_OK)
 *
 * Rules: only drawers whose zone starts with `cabinet` and appears in `map_csv` are applied;
 * other zones are ignored. The result replaces the whole previous light set, so an event with
 * nothing for this cabinet yields all pixels OFF (LOCATE_OK). If a zone repeats, the last entry
 * wins. Malformed JSON, a missing/invalid seq or drawers array, or a bad ttlMs returns an error
 * and leaves `states` untouched (no change to the lights). */
locate_rc_t locate_apply(const char *json, size_t len, char cabinet, const char *map_csv,
                         led_state_t *states, size_t n_pixels, locate_info_t *info);
