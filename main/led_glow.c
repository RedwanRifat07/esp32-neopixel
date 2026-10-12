#include "led_glow.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

static led_strip_handle_t s_strip;
static QueueHandle_t s_cmd_q;
/* Drawer codes in pixel order, parsed once from CONFIG_SIOT_DRAWER_MAP ("A1,A2,..."). */
static char s_code[CONFIG_SIOT_LED_COUNT][8];

static void parse_drawer_map(void)
{
    const char *p = CONFIG_SIOT_DRAWER_MAP;
    for (int i = 0; i < CONFIG_SIOT_LED_COUNT; i++) {
        size_t n = strcspn(p, ",");
        snprintf(s_code[i], sizeof(s_code[i]), "%.*s", (int)n, p);
        p += n + (p[n] == ',' ? 1 : 0);
    }
}

static void led_task(void *arg)
{
    const led_rgb_t on = led_color_scaled(CONFIG_SIOT_GLOW_RGB, CONFIG_SIOT_GLOW_BRIGHTNESS_PCT);
    const int64_t glow_us = (int64_t)CONFIG_SIOT_GLOW_MS * 1000;
    int64_t deadline_us[CONFIG_SIOT_LED_COUNT] = {0}; /* 0 = off */

    for (;;) {
        /* Sleep until the nearest deadline, or indefinitely if everything is off. */
        int64_t now = esp_timer_get_time();
        int64_t next = INT64_MAX;
        for (int i = 0; i < CONFIG_SIOT_LED_COUNT; i++) {
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
                if (led_strip_set_pixel(s_strip, cmd.pixel, on.r, on.g, on.b) == ESP_OK) {
                    deadline_us[cmd.pixel] = esp_timer_get_time() + glow_us;
                    dirty = true;
                    ESP_LOGI(TAG, "drawer %u (%s) on", cmd.pixel + 1u, s_code[cmd.pixel]);
                } else {
                    ESP_LOGE(TAG, "set_pixel failed for drawer %u", cmd.pixel + 1u);
                }
            } else { /* CMD_CLEAR_ALL */
                for (int i = 0; i < CONFIG_SIOT_LED_COUNT; i++) {
                    if (deadline_us[i]) {
                        led_strip_set_pixel(s_strip, i, 0, 0, 0);
                        deadline_us[i] = 0;
                        dirty = true;
                    }
                }
                ESP_LOGI(TAG, "all off");
            }
        }

        now = esp_timer_get_time();
        for (int i = 0; i < CONFIG_SIOT_LED_COUNT; i++) {
            if (deadline_us[i] && deadline_us[i] <= now) {
                led_strip_set_pixel(s_strip, i, 0, 0, 0);
                deadline_us[i] = 0;
                dirty = true;
                ESP_LOGI(TAG, "drawer %d (%s) off", i + 1, s_code[i]);
            }
        }
        if (dirty && led_strip_refresh(s_strip) != ESP_OK) {
            ESP_LOGE(TAG, "refresh failed");
        }
    }
}

esp_err_t led_glow_init(void)
{
    ESP_RETURN_ON_FALSE(s_strip == NULL, ESP_ERR_INVALID_STATE, TAG, "already initialised");

    const led_strip_config_t strip_cfg = {
        .strip_gpio_num = CONFIG_SIOT_LED_GPIO,
        .max_leds = CONFIG_SIOT_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
    };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip), TAG, "strip init");
    ESP_RETURN_ON_ERROR(led_strip_clear(s_strip), TAG, "initial clear");
    parse_drawer_map();

    s_cmd_q = xQueueCreate(CMD_QUEUE_LEN, sizeof(cmd_t));
    ESP_RETURN_ON_FALSE(s_cmd_q, ESP_ERR_NO_MEM, TAG, "queue alloc");
    ESP_RETURN_ON_FALSE(xTaskCreate(led_task, "led_glow", 3072, NULL, 5, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task alloc");

    ESP_LOGI(TAG, "ready: cabinet=%s gpio=%d pixels=%d map=%s", CONFIG_SIOT_CABINET,
             CONFIG_SIOT_LED_GPIO, CONFIG_SIOT_LED_COUNT, CONFIG_SIOT_DRAWER_MAP);
    return ESP_OK;
}

static esp_err_t send_cmd(cmd_t cmd)
{
    ESP_RETURN_ON_FALSE(s_cmd_q, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    return xQueueSend(s_cmd_q, &cmd, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t led_glow_trigger(unsigned drawer)
{
    if (drawer < 1 || drawer > CONFIG_SIOT_LED_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    return send_cmd((cmd_t){.type = CMD_GLOW, .pixel = (uint8_t)(drawer - 1)});
}

esp_err_t led_glow_clear_all(void)
{
    return send_cmd((cmd_t){.type = CMD_CLEAR_ALL});
}
