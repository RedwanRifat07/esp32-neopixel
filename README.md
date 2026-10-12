# esp32-neopixel

ESP32 firmware (ESP-IDF) that lights WS2812 NeoPixels as per-drawer indicators and exposes a
small HTTP API plus a browser test page. Each "drawer" is one pixel; triggering a drawer makes
its pixel glow for a fixed time, then switch off on its own timer.

## Status

| Area | State | Evidence |
|---|---|---|
| Build (ESP-IDF v5.5.4, target `esp32`) | PROVEN | `idf.py build` succeeded, app 0xca580 bytes (21% of the 1 MB app partition free) |
| Flash + boot on a real ESP32 | PROVEN | write verified, boot log shows config, Wi-Fi join and `web: listening on port 80` |
| HTTP API (`/api/status`, `/api/glow`) | PROVEN | exercised with `curl` against the running board |
| Pixels physically lighting on both strips | UNKNOWN | firmware logs success, but the LEDs were not observed by the author of this README |
| IMS poller | NOT IMPLEMENTED | `IMS_*` settings are parsed and compiled in, but no code polls an IMS endpoint |
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
| `POLL_INTERVAL_MS`, `IMS_URL`, `IMS_TOKEN`, `DEVICE_ID` | no | – | reserved for the IMS poller (not implemented) |

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
`CONFIG_SIOT_LED_BOOT_TEST=y` (default), drawers 1..N light in sequence as a wiring test.

## HTTP API

Served on port 80. If `WEB_TOKEN` is set, POSTs must send `X-Auth-Token: <token>`.

| Method | Path | Body | Result |
|---|---|---|---|
| GET | `/` | – | test page (drawer buttons) |
| GET | `/api/status` | – | `{"drawers":5,"glow_ms":5000,"auth":false}` |
| POST | `/api/glow` | `{"drawer": <1..N>}` | glow that drawer; `400` if out of range |
| POST | `/api/off` | – | turn all drawers off |

POSTs need `Content-Type: application/json`.

```sh
curl -X POST -H 'Content-Type: application/json' -d '{"drawer":3}' http://<board-ip>/api/glow
```

Several drawers can glow at once, each with its own timer; re-triggering a glowing drawer restarts
its timer.

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
| `GET /locate/latest` | newest event; `seq` 0 with no drawers before the first emit |
| `GET /locate/next?after=S&wait=W` | first event with `seq > S`; `204` after `W` s (capped at 60) |
| `POST /mock/emit` | inject an event; rejects unknown fields so no product names/ids can be added |

Any wrong or missing bearer token gets `401`. `IMS_MOCK_CA=1` in `.env` is the switch that will let the
firmware trust the mock's CA; it defaults to off, and a production build trusts only the certificate
bundle. Nothing in the firmware reads it yet.

## Layout

| Path | Purpose |
|---|---|
| [main/app_main.c](main/app_main.c) | startup, NVS, Wi-Fi, LED driver, web server, boot chase test |
| [main/led_glow.c](main/led_glow.c) | single-strip RMT driver; one task owns the strip, commands arrive over a queue |
| [main/web_server.c](main/web_server.c), [main/index.html](main/index.html) | HTTP server, API and test page |
| [main/wifi_sta.c](main/wifi_sta.c) | station mode with timed reconnect backoff |
| [main/led_color.h](main/led_color.h) | pure colour-scaling helper |
| [main/Kconfig.projbuild](main/Kconfig.projbuild) | `SIOT_*` config symbols |
| [tools/](tools/) | `.env` converter and host-side tests |

## Tests

```sh
python3 -I tools/test_env_to_sdkconfig.py          # .env parser/validator
python3 -I tools/test_mock_ims.py                  # mock IMS server (needs openssl for the TLS test)
cc -o /tmp/test_led_color tools/test_led_color.c && /tmp/test_led_color   # colour scaling (host)
```

Both suites were run and pass (Python: 14 env-tool tests + 13 mock-IMS tests; C: `led_color OK`).

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

## Not done yet

- IMS polling (`IMS_URL`/`IMS_TOKEN`/`DEVICE_ID`) is configuration only.
- Disable `CONFIG_SIOT_LED_BOOT_TEST` once real triggers drive the LEDs.
- Flash size: the board reports 4 MB but the image header is 2 MB; the extra space is unused.
