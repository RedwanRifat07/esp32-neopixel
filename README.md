# esp32-neopixel

ESP32 firmware (ESP-IDF) that lights WS2812 NeoPixels as per-drawer indicators and exposes a
small HTTP API plus a browser test page. Each "drawer" is one pixel; triggering a drawer makes
its pixel glow for a fixed time, then switch off on its own timer.

## Status

| Area | State | Evidence |
|---|---|---|
| Build (ESP-IDF v5.5.4, target `esp32`) | PROVEN | `idf.py build` succeeded; app 0xe3eb0 bytes, 70% of the 3 MB app partition free (mock/dev build) |
| Flash + boot on a real ESP32 | PROVEN | write verified, boot log shows config, Wi-Fi join and `web: listening on port 80` |
| HTTP API (`/api/status`, `/api/glow`) | PROVEN | exercised with `curl` against the running board |
| Pixels physically lighting on both strips | UNKNOWN | firmware logs success, but the LEDs were not observed by the author of this README |
| IMS poller | PROVEN against the mock IMS on Board A (rows 1-8 below) | serial log + mock log, 2026-10-12. Not yet against the real IMS, not yet Board B, soak not run |
| Behaviour on ESP-IDF 6.0 | UNKNOWN | only v5.5.4 was built |

## Hardware

- Classic ESP32 dev board (built for `esp32`; chip rev v3.0, 4 MB flash detected, image header configured for 2 MB).
- WS2812-type pixels, GRB order, 800 kHz timing (assumed in code: `LED_MODEL_WS2812`, `LED_STRIP_COLOR_COMPONENT_FMT_GRB`; not verified against your specific strip).
- One strip per board on `LED_GPIO` (default 18), one pixel per drawer. Cabinet A and Cabinet B each get their
  own ESP32 running the same firmware; `.env` (`CABINET`, `DRAWER_MAP`) selects which.
- Pixel → drawer is set by `DRAWER_MAP` (pixel order). Which physical drawer each pixel really lights
  is UNKNOWN until checked on the bench (see "Bench results").

Wire a common ground between the ESP32 and the strips' supply. A 3.3 V data line into a 5 V strip
can be marginal; a level shifter is the usual fix (not tested here).

## Configuration

Settings live in a gitignored `.env` and are turned into `sdkconfig.local` at CMake configure time by
[tools/env_to_sdkconfig.py](tools/env_to_sdkconfig.py). The build fails on a missing, invalid or
placeholder value. Secrets are compiled into the flash image and are never logged.

```sh
cp .env.example .env     # then edit .env
```

| Key | Required | Default | Notes |
|---|---|---|---|
| `WIFI_SSID` | yes | – | 2.4 GHz network, 1–32 bytes |
| `WIFI_PASSWORD` | yes | – | WPA2, 8–63 bytes |
| `LED_COUNT` | yes | – | pixels = drawers on this board, 1–30; must equal the `DRAWER_MAP` entry count |
| `CABINET` | yes | – | `A` or `B` |
| `DRAWER_MAP` | yes | – | physical drawer per pixel, in pixel order, e.g. `A1,A2,A3,A4,A5`; codes start with the cabinet letter, no duplicates |
| `LED_GPIO` | no | 18 | strip data pin |
| `WEB_TOKEN` | no | empty | 8–64 printable chars, no spaces. Empty = **no auth** |
| `GLOW_MS` | no | 5000 | glow duration |
| `GLOW_BRIGHTNESS_PCT` | no | 25 | 1–100 |
| `GLOW_RGB` | no | `FFB46E` | `RRGGBB` |
| `IMS_URL` | no | empty | IMS API base, `https://` only, no trailing slash, e.g. `https://ims.siot.solutions/api/v1`. Empty = poller disabled |
| `IMS_TOKEN` | with `IMS_URL` | – | device key (scope `locate:read`) |
| `IMS_MOCK_CA` | no | 0 | `1` = bench build that trusts **only** `certs/mock_ca.pem` (see below). Requires `DEV_BUILD=1` |
| `DEV_BUILD` | no | 0 | `1` marks a bench build. `IMS_MOCK_CA=1` without it fails at configure time (and `ims_locate.c` has a matching `#error`) |
| `WIFI2_SSID`, `WIFI2_PASSWORD` | no | – | fallback network; set both or neither |
| `GREEN_RGB`, `RED_RGB` | no | `00FF00`, `FF0000` | in-stock / out-of-stock colours, scaled by `GLOW_BRIGHTNESS_PCT` |
| `LED_BOOT_TEST` | no | 0 | `1` = chase drawers 1..N at boot |
| `POLL_INTERVAL_MS`, `DEVICE_ID` | no | – | accepted but unused |

Constraints enforced by the tool: GPIOs 0, 1, 2, 3, 5, 6–12 and 15
(strapping, flash, UART0) are rejected for LED data.

## Build and flash

Requires ESP-IDF v5.5.x.

```sh
source ~/esp/esp-idf/export.sh        # your ESP-IDF 5.5 install
idf.py build
idf.py -p /dev/cu.usbserial-0001 flash monitor   # adjust the port
```

The monitor prints the board's DHCP address (`wifi_sta: got ip ...`). On boot, with
`LED_BOOT_TEST=1` (off by default), drawers 1..N light in sequence as a wiring test.

## HTTP API

Served on port 80. If `WEB_TOKEN` is set, POSTs must send `X-Auth-Token: <token>`.

| Method | Path | Body | Result |
|---|---|---|---|
| GET | `/` | – | test page (drawer buttons) |
| GET | `/api/status` | – | `{"cabinet":"A","drawers":5,"glow_ms":5000,"auth":false,"ssid":"<network>","ims":{"state":"ok","lastSeq":412,"lastOkAgeS":3}}`; `ims.state` is `disabled`, `syncing`, `ok`, `auth_failed` or `error`; never the Wi-Fi password |
| POST | `/api/glow` | `{"drawer": <1..N>}` | glow that drawer; `400` if out of range |
| POST | `/api/off` | – | turn all drawers off |

POSTs need `Content-Type: application/json`.

```sh
curl -X POST -H 'Content-Type: application/json' -d '{"drawer":3}' http://<board-ip>/api/glow
```

Several drawers can glow at once, each with its own timer; re-triggering a glowing drawer restarts
its timer.

## IMS poller

`ims_locate.c` long-polls `GET <IMS_URL>/locate/next?after=<seq>&wait=25` with `Authorization: Bearer <IMS_TOKEN>` (30 s HTTP timeout).

- **Boot / resync:** one `GET /locate/latest` to learn `seq` (a `204` means no event yet: start at seq 0); nothing lights. Any failure re-learns `seq` this way, so a restarted server (seq reset) is recovered without a reboot. Events emitted during an outage are not replayed.
- **200 + JSON:** parsed by `locate_apply()` and applied: only zones that start with this board's `CABINET` letter and appear in `DRAWER_MAP`; each event *replaces* the previous light set (nothing for this cabinet = all off); lit pixels switch off after `ttlMs` (default 15000 if absent).
- **204:** poll again. **401/403:** log once, retry after 5 min. **Other errors** (including a non-JSON 200 from a proxy, redirects, oversized or malformed bodies): back off 2 s doubling to 60 s.
- **Logged:** only `seq`, drawer count and HTTP status. The key and URL are never logged. Redirects are not followed so the key cannot be forwarded elsewhere.
- **Wi-Fi:** hostname `siot-drawers-a` or `-b`; after 3 consecutive failures the board alternates to `WIFI2_*` if set.

## Mock IMS (bench)

[tools/mock_ims.py](tools/mock_ims.py) is a stdlib-only stand-in for the IMS locate service. The board is
meant to long-poll it exactly as it will poll the real IMS (the poller itself is Step 3, not built yet).

```sh
openssl req -x509 -newkey rsa:2048 -nodes -days 30 -subj /CN=ims.siot.solutions \
    -addext subjectAltName=DNS:ims.siot.solutions,IP:<this-host-ip> -keyout mock.key -out mock.crt
export MOCK_TOKEN=<16+ chars>           # never commit; the server never prints it
python3 -I tools/mock_ims.py --cert mock.crt --key mock.key        # https on :8443
curl --cacert mock.crt -H "Authorization: Bearer $MOCK_TOKEN" -X POST \
     -d '{"drawers":[{"zone":"B2","room":"Cabinet B","state":"green","cells":["2A-2D"]}],"ttlMs":15000}' \
     https://<host>:8443/mock/emit
```

| Endpoint (also under `/api/v1`) | Behaviour |
|---|---|
| `GET /locate/latest` | newest event; `204` before the first emit (observed behaviour of the real IMS on 2026-10-12) |
| `GET /locate/next?after=S&wait=W` | first event with `seq > S`; `204` after `W` s (capped at 60) |
| `POST /mock/emit` | inject an event; rejects unknown fields so no product names/ids can be added |

Any wrong or missing bearer token gets `401`.

`IMS_MOCK_CA=1` embeds `certs/mock_ca.pem` (the mock's certificate; `certs/` is gitignored) and trusts **only** it.
This differs from "an extra CA": in ESP-IDF 5.5.4 `esp-tls` uses the certificate bundle *or* a supplied CA, not both
(`esp_tls_mbedtls.c`: the bundle branch is tested first, the custom CA is an `else if`). A mock build therefore
cannot reach the real IMS. The certificate must have a SAN for the host or IP in `IMS_URL`. A production build
(`IMS_MOCK_CA=0`) uses only the certificate bundle, with no insecure fallback. The mode is logged at boot: `TLS trust: MOCK CA ONLY (dev build, ...)` (warning) or `TLS trust: certificate bundle`.

## Layout

| Path | Purpose |
|---|---|
| [main/app_main.c](main/app_main.c) | startup, NVS, Wi-Fi, LED driver, web server, boot chase test |
| [main/led_glow.c](main/led_glow.c) | single-strip RMT driver; one task owns the strip, commands arrive over a queue |
| [main/ims_locate.c](main/ims_locate.c) | IMS long-poll task and status |
| [main/locate_apply.c](main/locate_apply.c) | pure event → per-pixel state function (host-tested) |
| [main/web_server.c](main/web_server.c), [main/index.html](main/index.html) | HTTP server, API and test page |
| [main/wifi_sta.c](main/wifi_sta.c) | station mode with timed reconnect backoff |
| [main/led_color.h](main/led_color.h) | pure colour-scaling helper |
| [main/Kconfig.projbuild](main/Kconfig.projbuild) | `SIOT_*` config symbols |
| [tools/](tools/) | `.env` converter and host-side tests |

## Tests

```sh
python3 -I tools/test_env_to_sdkconfig.py          # .env parser/validator
python3 -I tools/test_mock_ims.py                  # mock IMS server (needs openssl for the TLS test)
J=$IDF_PATH/components/json/cJSON                 # locate_apply host test
cc -I main -I $J tools/test_locate_apply.c main/locate_apply.c $J/cJSON.c -o /tmp/tla && /tmp/tla
cc -o /tmp/test_led_color tools/test_led_color.c && /tmp/test_led_color   # colour scaling (host)
```

All suites were run and pass (Python: 16 env-tool + 13 mock-IMS tests; C: `led_color OK`, `locate_apply OK (28 state checks)`).

## Security notes

- With no `WEB_TOKEN` the API is open to anyone on the network. Set one for anything beyond a bench.
- The token travels over plain HTTP; it is not encrypted in transit.
- `.env`, `sdkconfig*` and `build/` are gitignored. Never commit real credentials; rotate any that leak.

## Bench results (Cabinet A board, MAC 94:b5:55:8d:76:dc)

| Item | Result |
|---|---|
| Boot chase | PROVEN in the serial log: pixels 1–5 = A1–A5 on, then off |
| Free heap after Wi-Fi + web server | PROVEN: 212788 B (min ever 212788 B) |
| Physical drawer lit by each pixel | PROVEN (observed by the operator, 2026-10-12): pixel 1..5 = A1..A5, matches `DRAWER_MAP` |
| Supply | Operator-reported: separate 5 V adapter/charger. Voltage and current rating not given (UNKNOWN); nothing measured |
| Level shifter | Operator-reported: none, GPIO 18 wired straight to the strip data-in. 3.3 V data into a 5 V strip is marginal: a risk if flicker or wrong colours appear |
| Common ground | Operator-reported: adapter, strip and ESP32 grounds are joined |
| Brightness cap | 25% of `GLOW_RGB` by default (ASSUMED safe for 5 pixels; supply not measured) |

## Step 4 bench: Board A vs mock IMS (2026-10-12)

Mock server on the Mac (`tools/mock_ims.py`, HTTPS, self-signed cert), board on the same Wi-Fi. "Latency" is
host time of the POST to host time the serial line `state set` was received, so it includes USB-serial delay.
The table below is the firmware's log. LED output was confirmed separately by the operator (see below).

| # | Event | Board A result (serial log) | Latency |
|---|---|---|---|
| 1 | B2 green | `state set: 0 lit` (ignored, other cabinet) | 0.26 s |
| 2 | A4 red | `1 lit`; `drawer 4 (A4) off` 15.01 s later (board uptime stamps) | 0.33 s |
| 3 | A2 green + B4 red | `1 lit` (A2 only); A2 off after 15 s | 0.23 s |
| 3b | A2 green, then A5 red 3 s later | second event replaced the first; only A5 expired, 15 s after its own set | 0.29 s / 0.29 s |
| 4 | empty event while A1 lit (ttl 60 s) | `0 lit` | 0.16 s |
| 5 | unknown zone R3 | `0 lit` | 0.14 s |
| 6 | mock stopped 2 min, restarted (seq reset to 0) | backoff 2, 4, 8, 16, 32, 60 s; `synced at seq 0` 3.2 s after the restart; next event applied, no reboot | – |
| 7 | reset while A1 lit (ttl 120 s) | boot shows no `state set`; `synced at seq 2` | – |
| 8 | wrong key | `http 401` + one `key rejected; retrying in 5 min` (E level); next request 300.8 s later | – |

**LED output (operator-reported, not measured):** after a visible test run (A1 green; A2 red; A3 green + A5 red;
empty event; B2 green) the operator reported the LEDs "working fine". The reply was a single sentence, so per-step
detail (which pixel, colour accuracy, off times) was not itemised. Board status afterwards: `ims.state` ok, `lastSeq` 5.

Notes on row 8: the `http 401` warning is printed on every attempt (once per 5 min); the *error* line is printed once per episode.
Row 7 used a serial-RTS reset (`POWERON_RESET`), not an unplug.

| Gate item | State |
|---|---|
| 8/8 on Board A | PASS by log. LED output: operator-reported working (2026-10-12, 15:36 +06), see below |
| 8/8 on Board B | NOT RUN (no second board) |
| Real IMS | NOT RUN (no device key yet) |
| Overnight soak, heap every 10 min | NOT RUN |
| 30-minute serial capture, no secret | PARTIAL: 682 s captured; 0 occurrences of the IMS key, the Wi-Fi password or "Bearer" in the serial and mock logs |
| Free heap after Wi-Fi + web | 191412 B and 191436 B (two boots); the 10-minute heap line not yet observed |

## Known risks

- **Flash layout (PROVEN):** the chip reports 4 MB (`esptool flash_id`: "Detected flash size: 4MB"). [partitions.csv](partitions.csv) gives one 3 MB app partition at 0x10000 (0x310000..0x3FFFFF unused). Free app space measured from build output: production 68% (0x20ad10 B), mock/dev 70% (0x21c150 B), no-IMS 74% (0x2355e0 B); it was ~4% / 11% with the old 1 MB partition. No OTA partition yet.
- **Level shifter absent** (operator-reported): see Bench results. The operator will fit one later (74AHCT125 suggested); until then the data line is 3.3 V direct.

## Not done yet

- On-board test of the poller against the mock and the real IMS (Step 4), plus the overnight soak.

## Recovering a board

1. Find the port: `ls /dev/cu.usbserial-*`. Read the MAC first and confirm it is the board you mean: `python -m esptool --port <port> read_mac`.
2. Rebuild and re-flash (write only): `idf.py -B build -p <port> flash`. Do **not** `erase_flash` without the lab lead's yes.
3. If it will not boot, hold BOOT, tap EN, then flash (the usual ESP32 dev-board procedure; not needed so far). Watch `idf.py -p <port> monitor` for `wifi_sta: got ip` and `ims: synced at seq ...`.
4. Wi-Fi never joins: check `WIFI_SSID` is 2.4 GHz, add `WIFI2_*` as a fallback, and confirm the board is within range. (One boot on 2026-10-12 needed ~45 s to join, reason 201 "AP not found", then joined; cause unknown.)
5. `ims.state` stuck at `error`: the board cannot reach `IMS_URL` or the TLS check fails (wrong CA / SAN). `auth_failed`: the device key is wrong or revoked; it retries every 5 min.
