#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "ims_locate.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_glow.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "web_server.h"
#include "wifi_sta.h"

static const char *TAG = "app";

/* Soak evidence: free/min-ever heap every 10 minutes. */
static void heap_log_cb(void *arg)
{
    ESP_LOGI(TAG, "heap: free %u B, min ever %u B", (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());
}

void app_main(void)
{
    /* Secrets (Wi-Fi password, tokens, IMS URL) are deliberately never logged. */
    ESP_LOGI(TAG, "config: cabinet=%s led_gpio=%d drawers=%d poll=%dms glow=%dms brightness=%d%%",
             CONFIG_SIOT_CABINET, CONFIG_SIOT_LED_GPIO, CONFIG_SIOT_LED_COUNT, CONFIG_SIOT_POLL_INTERVAL_MS,
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
    ESP_LOGW(TAG, "boot test: chasing drawers 1..%d", CONFIG_SIOT_LED_COUNT);
    for (unsigned d = 1; d <= CONFIG_SIOT_LED_COUNT; d++) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(led_glow_trigger(d));
        vTaskDelay(pdMS_TO_TICKS(300));
    }
#endif

    ESP_ERROR_CHECK(wifi_sta_start());
    while (!wifi_sta_wait_connected(15000)) {
        ESP_LOGW(TAG, "still waiting for Wi-Fi...");
    }
    ESP_ERROR_CHECK(web_server_start());
    ESP_ERROR_CHECK(ims_locate_start());

    static esp_timer_handle_t heap_timer;
    const esp_timer_create_args_t hargs = {.callback = heap_log_cb, .name = "heap_log"};
    ESP_ERROR_CHECK(esp_timer_create(&hargs, &heap_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(heap_timer, 10ULL * 60 * 1000000));
    ESP_LOGI(TAG, "free heap after Wi-Fi+web: %u B (min ever %u B)", (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());
}
