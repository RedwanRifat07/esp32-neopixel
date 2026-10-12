#include "wifi_sta.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"

#define CONNECTED_BIT BIT0
#define BACKOFF_MIN_MS 1000
#define BACKOFF_MAX_MS 30000
#define FAILS_BEFORE_SWITCH 3

static const char *const s_ssid[2] = {CONFIG_SIOT_WIFI_SSID, CONFIG_SIOT_WIFI2_SSID};
static const char *const s_pass[2] = {CONFIG_SIOT_WIFI_PASSWORD, CONFIG_SIOT_WIFI2_PASSWORD};
static int s_active;      /* index into s_ssid/s_pass */
static unsigned s_fails;  /* consecutive failures on the active network */

static const char *TAG = "wifi_sta";

static EventGroupHandle_t s_events;
static esp_timer_handle_t s_retry_timer;
static uint32_t s_backoff_ms = BACKOFF_MIN_MS;

static esp_err_t apply_config(int which)
{
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, s_ssid[which], sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, s_pass[which], sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    memset(&cfg, 0, sizeof(cfg)); /* don't leave the password on the stack */
    return err;
}

static void retry_cb(void *arg)
{
    esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        xEventGroupClearBits(s_events, CONNECTED_BIT);
        ESP_LOGW(TAG, "disconnected (reason %d), retry in %u ms", d->reason, (unsigned)s_backoff_ms);
        if (++s_fails >= FAILS_BEFORE_SWITCH && s_ssid[1][0] != '\0') {
            s_active ^= 1;
            s_fails = 0;
            ESP_LOGW(TAG, "switching to %s Wi-Fi network", s_active ? "fallback" : "primary");
            if (apply_config(s_active) != ESP_OK) {
                ESP_LOGE(TAG, "set_config failed");
            }
        }
        esp_timer_start_once(s_retry_timer, (uint64_t)s_backoff_ms * 1000);
        if (s_backoff_ms < BACKOFF_MAX_MS) {
            s_backoff_ms = s_backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : s_backoff_ms * 2;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        s_backoff_ms = BACKOFF_MIN_MS;
        s_fails = 0;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_events, CONNECTED_BIT);
    }
}

esp_err_t wifi_sta_start(void)
{
    s_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_events, ESP_ERR_NO_MEM, TAG, "event group alloc");
    const esp_timer_create_args_t targs = {.callback = retry_cb, .name = "wifi_retry"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s_retry_timer), TAG, "retry timer");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(netif, ESP_FAIL, TAG, "create sta netif");
    char host[20];
    snprintf(host, sizeof(host), "siot-drawers-%c", tolower((unsigned char)CONFIG_SIOT_CABINET[0]));
    esp_netif_set_hostname(netif, host);

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL), TAG, "reg wifi");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL), TAG, "reg ip");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode");
    ESP_RETURN_ON_ERROR(apply_config(s_active), TAG, "set config");
    return esp_wifi_start();
}

bool wifi_sta_wait_connected(uint32_t timeout_ms)
{
    EventBits_t bits = xEventGroupWaitBits(s_events, CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return bits & CONNECTED_BIT;
}

const char *wifi_sta_ssid(void)
{
    return (s_events && (xEventGroupGetBits(s_events) & CONNECTED_BIT)) ? s_ssid[s_active] : "";
}
