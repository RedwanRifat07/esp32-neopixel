/* Host test: cc -I main -I $IDF_PATH/components/json/cJSON tools/test_locate_apply.c \
 *               main/locate_apply.c $IDF_PATH/components/json/cJSON/cJSON.c -o t && ./t */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "locate_apply.h"

static const char *MAP_A = "A1,A2,A3,A4,A5";
static const char *MAP_B = "B1,B2,B3,B4,B5";
static int checks;

#define APPLY(js, cab, map, st, info) locate_apply(js, strlen(js), cab, map, st, 5, info)
#define EXPECT(st, a, b, c, d, e)                                                                  \
    do {                                                                                           \
        led_state_t want[5] = {a, b, c, d, e};                                                     \
        assert(memcmp(st, want, sizeof(want)) == 0);                                               \
        checks++;                                                                                  \
    } while (0)
static void fill(led_state_t *st, led_state_t v)
{
    for (int i = 0; i < 5; i++) {
        st[i] = v;
    }
}

#define G LED_STATE_GREEN
#define R LED_STATE_RED
#define O LED_STATE_OFF

int main(void)
{
    led_state_t st[5];
    locate_info_t info = {0};

    /* cabinet filter: the doc's example event */
    const char *ev = "{\"seq\":412,\"ts\":\"2026-10-12T08:00:00Z\",\"ttlMs\":15000,\"drawers\":["
                     "{\"zone\":\"B2\",\"room\":\"Cabinet B\",\"state\":\"green\",\"cells\":[\"2A-2D\"]},"
                     "{\"zone\":\"A4\",\"room\":\"Cabinet A\",\"state\":\"red\",\"cells\":[\"1A\"]}]}";
    assert(APPLY(ev, 'A', MAP_A, st, &info) == LOCATE_OK);
    EXPECT(st, O, O, O, R, O);
    assert(info.seq == 412 && info.ttl_ms == 15000 && info.drawer_count == 2);
    assert(APPLY(ev, 'B', MAP_B, st, &info) == LOCATE_OK);
    EXPECT(st, O, G, O, O, O);

    /* green + red on the two cabinets, A2 green / B4 red */
    const char *ev3 = "{\"seq\":3,\"drawers\":[{\"zone\":\"A2\",\"state\":\"green\"},{\"zone\":\"B4\",\"state\":\"red\"}]}";
    assert(APPLY(ev3, 'A', MAP_A, st, &info) == LOCATE_OK);
    EXPECT(st, O, G, O, O, O);
    assert(info.ttl_ms == LOCATE_DEFAULT_TTL_MS); /* ttlMs absent -> default */
    assert(APPLY(ev3, 'B', MAP_B, st, &info) == LOCATE_OK);
    EXPECT(st, O, O, O, R, O);

    /* empty set for this cabinet replaces everything -> all off */
    fill(st, G);
    assert(APPLY("{\"seq\":4,\"drawers\":[]}", 'A', MAP_A, st, &info) == LOCATE_OK);
    EXPECT(st, O, O, O, O, O);
    fill(st, G);
    assert(APPLY("{\"seq\":5,\"drawers\":[{\"zone\":\"B1\",\"state\":\"green\"}]}", 'A', MAP_A, st, &info) == LOCATE_OK);
    EXPECT(st, O, O, O, O, O);

    /* unknown zones ignored: R3, A9 (cabinet letter but not in map), a2 (case), empty, non-string */
    fill(st, G);
    assert(APPLY("{\"seq\":6,\"drawers\":[{\"zone\":\"R3\",\"state\":\"red\"},{\"zone\":\"A9\",\"state\":\"red\"},"
                 "{\"zone\":\"a2\",\"state\":\"red\"},{\"zone\":\"\",\"state\":\"red\"},{\"zone\":7,\"state\":\"red\"},"
                 "{\"zone\":\"A1\",\"state\":\"blue\"},{\"zone\":\"A3\"},5,null]}",
                 'A', MAP_A, st, &info) == LOCATE_OK);
    EXPECT(st, O, O, O, O, O);
    assert(info.drawer_count == 9);

    /* duplicate zone: last wins */
    assert(APPLY("{\"seq\":7,\"drawers\":[{\"zone\":\"A1\",\"state\":\"red\"},{\"zone\":\"A1\",\"state\":\"green\"}]}",
                 'A', MAP_A, st, &info) == LOCATE_OK);
    EXPECT(st, G, O, O, O, O);

    /* non-default map order is honoured */
    assert(APPLY("{\"seq\":8,\"drawers\":[{\"zone\":\"A5\",\"state\":\"green\"}]}", 'A', "A3,A1,A5,A2,A4", st, &info) == LOCATE_OK);
    EXPECT(st, O, O, G, O, O);

    /* malformed / invalid -> error and states untouched */
    const char *bad[] = {"", "not json", "{", "[]", "null", "{\"seq\":1}", "{\"drawers\":[]}",
                         "{\"seq\":\"1\",\"drawers\":[]}", "{\"seq\":-1,\"drawers\":[]}",
                         "{\"seq\":1.5,\"drawers\":[]}", "{\"seq\":1,\"drawers\":{}}",
                         "{\"seq\":1,\"ttlMs\":\"x\",\"drawers\":[]}", "{\"seq\":1,\"ttlMs\":5,\"drawers\":[]}",
                         "{\"seq\":1,\"ttlMs\":99999999,\"drawers\":[]}", "<html>Cloudflare</html>"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        fill(st, R);
        locate_rc_t rc = APPLY(bad[i], 'A', MAP_A, st, &info);
        assert(rc == LOCATE_BAD_JSON || rc == LOCATE_BAD_EVENT);
        EXPECT(st, R, R, R, R, R);
    }
    /* bad args */
    assert(locate_apply(NULL, 0, 'A', MAP_A, st, 5, &info) == LOCATE_BAD_ARGS);
    assert(APPLY("{\"seq\":1,\"drawers\":[]}", 'C', MAP_A, st, &info) == LOCATE_BAD_ARGS);
    assert(locate_apply("{}", 2, 'A', MAP_A, st, 0, &info) == LOCATE_BAD_ARGS);
    /* length is honoured: trailing garbage beyond len is not parsed */
    const char *trail = "{\"seq\":9,\"drawers\":[]}garbage";
    assert(locate_apply(trail, strlen(trail) - 7, 'A', MAP_A, st, 5, &info) == LOCATE_OK && info.seq == 9);

    printf("locate_apply OK (%d state checks)\n", checks);
    return 0;
}
