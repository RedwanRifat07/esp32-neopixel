#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_glow.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "web_server.h"
#include "wifi_sta.h"

static const char *TAG = "app";

void app_main(void)
{
    /* Secrets (Wi-Fi password, tokens, IMS URL) are deliberately never logged. */
    ESP_LOGI(TAG, "config: led_gpio=%d drawers=%d poll=%dms glow=%dms brightness=%d%%",
             CONFIG_SIOT_LED_GPIO, LED_GLOW_DRAWERS, CONFIG_SIOT_POLL_INTERVAL_MS,
             CONFIG_SIOT_GLOW_MS, CONFIG_SIOT_GLOW_BRIGHTNESS_PCT);

    /* The default NVS partition only holds Wi-Fi calibration data here. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(led_glow_init());

#if CONFIG_SIOT_LED_BOOT_TEST
    /* Wiring check: light drawers 1..N one after another. */
    ESP_LOGW(TAG, "boot test: chasing drawers 1..%d", LED_GLOW_DRAWERS);
    for (unsigned d = 1; d <= LED_GLOW_DRAWERS; d++) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(led_glow_trigger(d));
        vTaskDelay(pdMS_TO_TICKS(300));
    }
#endif

    ESP_ERROR_CHECK(wifi_sta_start());
    while (!wifi_sta_wait_connected(15000)) {
        ESP_LOGW(TAG, "still waiting for Wi-Fi...");
    }
    ESP_ERROR_CHECK(web_server_start());
}
