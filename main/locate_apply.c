#include "locate_apply.h"

#include <stdbool.h>
#include <string.h>

#include "cJSON.h"

#define MAX_EVENT_BYTES 8192
#define MAX_SEQ 9007199254740992.0 /* 2^53: largest integer a JSON double holds exactly */

/* True if v is a whole number in [lo, hi]. The range test comes first so the integer cast is
 * never applied to an out-of-range double (undefined behaviour). */
static bool whole_in_range(double v, double lo, double hi)
{
    return v >= lo && v <= hi && v == (double)(int64_t)v;
}

/* Pixel index of `zone` in the comma-separated map, or -1. Exact match, no allocation. */
static int pixel_of(const char *map_csv, const char *zone, size_t n_pixels)
{
    size_t zlen = strlen(zone);
    const char *p = map_csv;
    for (size_t i = 0; i < n_pixels; i++) {
        size_t n = strcspn(p, ",");
        if (n == zlen && strncmp(p, zone, n) == 0) {
            return (int)i;
        }
        if (p[n] == '\0') {
            break;
        }
        p += n + 1;
    }
    return -1;
}

locate_rc_t locate_apply(const char *json, size_t len, char cabinet, const char *map_csv,
                         led_state_t *states, size_t n_pixels, locate_info_t *info)
{
    if (!json || !map_csv || !states || !info || n_pixels == 0 || n_pixels > LOCATE_MAX_PIXELS ||
        (cabinet != 'A' && cabinet != 'B')) {
        return LOCATE_BAD_ARGS;
    }
    if (len == 0 || len > MAX_EVENT_BYTES) {
        return LOCATE_BAD_JSON;
    }
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        return LOCATE_BAD_JSON;
    }

    locate_rc_t rc = LOCATE_BAD_EVENT;
    const cJSON *seq = cJSON_GetObjectItemCaseSensitive(root, "seq");
    const cJSON *drawers = cJSON_GetObjectItemCaseSensitive(root, "drawers");
    const cJSON *ttl = cJSON_GetObjectItemCaseSensitive(root, "ttlMs");
    if (!cJSON_IsObject(root) || !cJSON_IsNumber(seq) || !whole_in_range(seq->valuedouble, 0, MAX_SEQ) ||
        !cJSON_IsArray(drawers)) {
        goto done;
    }
    uint32_t ttl_ms = LOCATE_DEFAULT_TTL_MS;
    if (ttl) {
        if (!cJSON_IsNumber(ttl) ||
            !whole_in_range(ttl->valuedouble, LOCATE_MIN_TTL_MS, LOCATE_MAX_TTL_MS)) {
            goto done;
        }
        ttl_ms = (uint32_t)ttl->valuedouble;
    }

    led_state_t out[LOCATE_MAX_PIXELS] = {LED_STATE_OFF};
    unsigned count = 0;
    const cJSON *d;
    cJSON_ArrayForEach(d, drawers)
    {
        count++;
        const cJSON *zone = cJSON_GetObjectItemCaseSensitive(d, "zone");
        const cJSON *state = cJSON_GetObjectItemCaseSensitive(d, "state");
        if (!cJSON_IsString(zone) || !zone->valuestring || zone->valuestring[0] != cabinet ||
            !cJSON_IsString(state) || !state->valuestring) {
            continue; /* other cabinet, malformed entry, or no state: ignore */
        }
        int px = pixel_of(map_csv, zone->valuestring, n_pixels);
        if (px < 0) {
            continue; /* unknown zone */
        }
        if (strcmp(state->valuestring, "green") == 0) {
            out[px] = LED_STATE_GREEN;
        } else if (strcmp(state->valuestring, "red") == 0) {
            out[px] = LED_STATE_RED;
        }
    }
    memcpy(states, out, n_pixels * sizeof(out[0]));
    info->seq = (int64_t)seq->valuedouble;
    info->ttl_ms = ttl_ms;
    info->drawer_count = count;
    rc = LOCATE_OK;
done:
    cJSON_Delete(root);
    return rc;
}
