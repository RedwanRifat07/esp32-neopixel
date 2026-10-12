#include "led_glow.h"

#include <stdint.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "led_color.h"
#include "led_strip.h"
#include "sdkconfig.h"

#define RMT_RESOLUTION_HZ (10 * 1000 * 1000)
#define CMD_QUEUE_LEN 16

typedef enum { CMD_GLOW, CMD_CLEAR_ALL } cmd_type_t;
typedef struct {
    cmd_type_t type;
    uint8_t pixel;
} cmd_t;

static const char *TAG = "led_glow";

/* Drawers 1..LED_COUNT live on strip 0 (SIOT_LED_GPIO); the rest on strip 1 (SIOT_LED2_GPIO). */
#define N_STRIPS 2
static const int s_strip_gpio[N_STRIPS] = {CONFIG_SIOT_LED_GPIO, CONFIG_SIOT_LED2_GPIO};
static const int s_strip_len[N_STRIPS] = {CONFIG_SIOT_LED_COUNT, CONFIG_SIOT_LED2_COUNT};
static led_strip_handle_t s_strip[N_STRIPS];
static QueueHandle_t s_cmd_q;

/* Set one drawer (0-based) on whichever strip it belongs to. */
static esp_err_t set_drawer(int idx, uint8_t r, uint8_t g, uint8_t b)
{
    const int s = idx < s_strip_len[0] ? 0 : 1;
    return led_strip_set_pixel(s_strip[s], s == 0 ? idx : idx - s_strip_len[0], r, g, b);
}

static bool refresh_all(void)
{
    bool ok = true;
    for (int s = 0; s < N_STRIPS; s++) {
        if (s_strip[s] && led_strip_refresh(s_strip[s]) != ESP_OK) {
            ESP_LOGE(TAG, "refresh failed on gpio %d", s_strip_gpio[s]);
            ok = false;
        }
    }
    return ok;
}

static void led_task(void *arg)
{
    const led_rgb_t on = led_color_scaled(CONFIG_SIOT_GLOW_RGB, CONFIG_SIOT_GLOW_BRIGHTNESS_PCT);
    const int64_t glow_us = (int64_t)CONFIG_SIOT_GLOW_MS * 1000;
    int64_t deadline_us[LED_GLOW_DRAWERS] = {0}; /* 0 = off */

    for (;;) {
        /* Sleep until the nearest deadline, or indefinitely if everything is off. */
        int64_t now = esp_timer_get_time();
        int64_t next = INT64_MAX;
        for (int i = 0; i < LED_GLOW_DRAWERS; i++) {
            if (deadline_us[i] && deadline_us[i] < next) {
                next = deadline_us[i];
            }
        }
        TickType_t wait = portMAX_DELAY;
        if (next != INT64_MAX) {
            int64_t left_ms = (next - now + 999) / 1000;
            wait = left_ms <= 0 ? 0 : pdMS_TO_TICKS(left_ms);
            if (wait == 0 && left_ms > 0) {
                wait = 1;
            }
        }

        bool dirty = false;
        cmd_t cmd;
        if (xQueueReceive(s_cmd_q, &cmd, wait) == pdTRUE) {
            if (cmd.type == CMD_GLOW) {
                if (set_drawer(cmd.pixel, on.r, on.g, on.b) == ESP_OK) {
                    deadline_us[cmd.pixel] = esp_timer_get_time() + glow_us;
                    dirty = true;
                    ESP_LOGI(TAG, "drawer %u on", cmd.pixel + 1u);
                } else {
                    ESP_LOGE(TAG, "set_pixel failed for drawer %u", cmd.pixel + 1u);
                }
            } else { /* CMD_CLEAR_ALL */
                for (int i = 0; i < LED_GLOW_DRAWERS; i++) {
                    if (deadline_us[i]) {
                        set_drawer(i, 0, 0, 0);
                        deadline_us[i] = 0;
                        dirty = true;
                    }
                }
                ESP_LOGI(TAG, "all off");
            }
        }

        now = esp_timer_get_time();
        for (int i = 0; i < LED_GLOW_DRAWERS; i++) {
            if (deadline_us[i] && deadline_us[i] <= now) {
                set_drawer(i, 0, 0, 0);
                deadline_us[i] = 0;
                dirty = true;
                ESP_LOGI(TAG, "drawer %d off", i + 1);
            }
        }
        if (dirty) {
            refresh_all();
        }
    }
}

esp_err_t led_glow_init(void)
{
    ESP_RETURN_ON_FALSE(s_strip[0] == NULL, ESP_ERR_INVALID_STATE, TAG, "already initialised");

    const led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
    };
    for (int i = 0; i < N_STRIPS; i++) {
        if (s_strip_len[i] == 0) {
            continue;
        }
        const led_strip_config_t strip_cfg = {
            .strip_gpio_num = s_strip_gpio[i],
            .max_leds = s_strip_len[i],
            .led_model = LED_MODEL_WS2812,
            .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        };
        ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip[i]), TAG, "strip init");
        ESP_RETURN_ON_ERROR(led_strip_clear(s_strip[i]), TAG, "initial clear");
    }

    s_cmd_q = xQueueCreate(CMD_QUEUE_LEN, sizeof(cmd_t));
    ESP_RETURN_ON_FALSE(s_cmd_q, ESP_ERR_NO_MEM, TAG, "queue alloc");
    ESP_RETURN_ON_FALSE(xTaskCreate(led_task, "led_glow", 3072, NULL, 5, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task alloc");

    ESP_LOGI(TAG, "ready: drawers=%d (1..%d on gpio %d, %d..%d on gpio %d)", LED_GLOW_DRAWERS,
             CONFIG_SIOT_LED_COUNT, CONFIG_SIOT_LED_GPIO, CONFIG_SIOT_LED_COUNT + 1,
             LED_GLOW_DRAWERS, CONFIG_SIOT_LED2_GPIO);
    return ESP_OK;
}

static esp_err_t send_cmd(cmd_t cmd)
{
    ESP_RETURN_ON_FALSE(s_cmd_q, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    return xQueueSend(s_cmd_q, &cmd, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t led_glow_trigger(unsigned drawer)
{
    if (drawer < 1 || drawer > LED_GLOW_DRAWERS) {
        return ESP_ERR_INVALID_ARG;
    }
    return send_cmd((cmd_t){.type = CMD_GLOW, .pixel = (uint8_t)(drawer - 1)});
}

esp_err_t led_glow_clear_all(void)
{
    return send_cmd((cmd_t){.type = CMD_CLEAR_ALL});
}
