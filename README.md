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
- Two strips, each driven through its own RMT channel:

| Drawers | GPIO | Pixels |
|---|---|---|
| 1 – 5 | 18 (`LED_GPIO`) | `LED_COUNT` = 5 |
| 6 – 9 | 19 (`LED2_GPIO`) | `LED2_COUNT` = 4 |

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
| `LED_COUNT` | yes | – | pixels on strip 1, 1–30 |
| `LED_GPIO` | no | 18 | strip 1 data pin |
| `LED2_COUNT` | no | 0 | pixels on strip 2; 0 disables it |
| `LED2_GPIO` | no | 19 | strip 2 data pin; must differ from `LED_GPIO` |
| `WEB_TOKEN` | no | empty | 8–64 printable chars, no spaces. Empty = **no auth** |
| `GLOW_MS` | no | 5000 | glow duration |
| `GLOW_BRIGHTNESS_PCT` | no | 25 | 1–100 |
| `GLOW_RGB` | no | `FFB46E` | `RRGGBB` |
| `POLL_INTERVAL_MS`, `IMS_URL`, `IMS_TOKEN`, `DEVICE_ID` | no | – | reserved for the IMS poller (not implemented) |

Constraints enforced by the tool: `LED_COUNT + LED2_COUNT <= 30`; GPIOs 0, 1, 2, 3, 5, 6–12 and 15
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
| GET | `/api/status` | – | `{"drawers":9,"glow_ms":5000,"auth":false}` |
| POST | `/api/glow` | `{"drawer": <1..N>}` | glow that drawer; `400` if out of range |
| POST | `/api/off` | – | turn all drawers off |

POSTs need `Content-Type: application/json`.

```sh
curl -X POST -H 'Content-Type: application/json' -d '{"drawer":6}' http://<board-ip>/api/glow
```

Several drawers can glow at once, each with its own timer; re-triggering a glowing drawer restarts
its timer.

## Layout

| Path | Purpose |
|---|---|
| [main/app_main.c](main/app_main.c) | startup, NVS, Wi-Fi, LED driver, web server, boot chase test |
| [main/led_glow.c](main/led_glow.c) | two-strip RMT driver; one task owns the strips, commands arrive over a queue |
| [main/web_server.c](main/web_server.c), [main/index.html](main/index.html) | HTTP server, API and test page |
| [main/wifi_sta.c](main/wifi_sta.c) | station mode with timed reconnect backoff |
| [main/led_color.h](main/led_color.h) | pure colour-scaling helper |
| [main/Kconfig.projbuild](main/Kconfig.projbuild) | `SIOT_*` config symbols |
| [tools/](tools/) | `.env` converter and host-side tests |

## Tests

```sh
python3 -I tools/test_env_to_sdkconfig.py          # .env parser/validator
cc -o /tmp/test_led_color tools/test_led_color.c && /tmp/test_led_color   # colour scaling (host)
```

Both suites were run and pass (Python: 14 tests; C: `led_color OK`).

## Security notes

- With no `WEB_TOKEN` the API is open to anyone on the network. Set one for anything beyond a bench.
- The token travels over plain HTTP; it is not encrypted in transit.
- `.env`, `sdkconfig*` and `build/` are gitignored. Never commit real credentials; rotate any that leak.

## Not done yet

- IMS polling (`IMS_URL`/`IMS_TOKEN`/`DEVICE_ID`) is configuration only.
- Disable `CONFIG_SIOT_LED_BOOT_TEST` once real triggers drive the LEDs.
- Flash size: the board reports 4 MB but the image header is 2 MB; the extra space is unused.
