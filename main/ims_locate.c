#include "ims_locate.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_glow.h"
#include "locate_apply.h"
#include "sdkconfig.h"
#include "wifi_sta.h"

#define WAIT_S 25
#define HTTP_TIMEOUT_MS 30000
#define BODY_MAX 4096
#define BACKOFF_MIN_MS 2000
#define BACKOFF_MAX_MS 60000
#define AUTH_BACKOFF_MS (5 * 60 * 1000)

#if CONFIG_SIOT_IMS_MOCK_CA && !CONFIG_SIOT_DEV_BUILD
#error "IMS_MOCK_CA trusts only the mock CA; it must be an explicit dev build (DEV_BUILD=1)"
#endif

#if CONFIG_SIOT_IMS_MOCK_CA
extern const char mock_ca_pem_start[] asm("_binary_mock_ca_pem_start");
#endif

static const char *TAG = "ims";

typedef enum { R_EVENT, R_NO_EVENT, R_AUTH, R_ERROR } result_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static const char *s_state = "disabled";
static int64_t s_last_seq = -1;
static int64_t s_last_ok_us = -1;

static void set_state(const char *state)
{
    taskENTER_CRITICAL(&s_lock);
    s_state = state;
    taskEXIT_CRITICAL(&s_lock);
}

static void note_ok(int64_t seq)
{
    taskENTER_CRITICAL(&s_lock);
    s_last_ok_us = esp_timer_get_time();
    if (seq >= 0) {
        s_last_seq = seq;
    }
    taskEXIT_CRITICAL(&s_lock);
}

void ims_locate_get_status(ims_status_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    out->state = s_state;
    out->last_seq = s_last_seq;
    out->last_ok_age_s = s_last_ok_us < 0 ? -1 : (int32_t)((esp_timer_get_time() - s_last_ok_us) / 1000000);
    taskEXIT_CRITICAL(&s_lock);
}

typedef struct {
    bool json;
} hdr_ctx_t;

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(evt->header_key, "Content-Type") == 0) {
        ((hdr_ctx_t *)evt->user_data)->json = strcasestr(evt->header_value, "application/json") != NULL;
    }
    return ESP_OK;
}

/* One GET. Fills body (NUL-terminated) and *len on a 200. Never logs the URL, the key or the body. */
static result_t http_get(const char *path_and_query, char *body, size_t cap, size_t *len)
{
    char url[256];
    if (snprintf(url, sizeof(url), "%s%s", CONFIG_SIOT_IMS_URL, path_and_query) >= (int)sizeof(url)) {
        ESP_LOGE(TAG, "request URL too long");
        return R_ERROR;
    }
    hdr_ctx_t ctx = {0};
    const esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .event_handler = on_http_event,
        .user_data = &ctx,
        .disable_auto_redirect = true, /* never forward the key to another host */
#if CONFIG_SIOT_IMS_MOCK_CA
        .cert_pem = mock_ca_pem_start, /* bench build: trusts only the mock CA */
#else
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return R_ERROR;
    }
    char auth[sizeof("Bearer ") + sizeof(CONFIG_SIOT_IMS_TOKEN)];
    snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_SIOT_IMS_TOKEN);
    esp_http_client_set_header(c, "Authorization", auth);
    memset(auth, 0, sizeof(auth));

    result_t res = R_ERROR;
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(err));
        goto out;
    }
    int64_t clen = esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (clen < 0 && status == 0) {
        ESP_LOGW(TAG, "no response: %s", esp_err_to_name((esp_err_t)clen));
        goto out;
    }
    if (status == 204) {
        res = R_NO_EVENT;
    } else if (status == 401 || status == 403) {
        ESP_LOGW(TAG, "http %d", status);
        res = R_AUTH;
    } else if (status != 200) {
        ESP_LOGW(TAG, "http %d", status);
    } else if (!ctx.json) {
        /* A proxy or Cloudflare page, not IMS. */
        ESP_LOGW(TAG, "http 200 but not application/json");
    } else if (clen > (int64_t)(cap - 1)) {
        ESP_LOGW(TAG, "http 200, body too large");
    } else {
        size_t n = 0;
        int r = 0;
        while (n < cap - 1 && (r = esp_http_client_read(c, body + n, cap - 1 - n)) > 0) {
            n += (size_t)r;
        }
        if (r < 0 || !esp_http_client_is_complete_data_received(c)) {
            ESP_LOGW(TAG, "http 200, incomplete body");
        } else {
            body[n] = '\0';
            *len = n;
            res = R_EVENT;
        }
    }
out:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return res;
}

static void sleep_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void poller_task(void *arg)
{
    static char body[BODY_MAX];
    static led_state_t states[CONFIG_SIOT_LED_COUNT];
    const char cabinet = CONFIG_SIOT_CABINET[0];
    uint32_t backoff_ms = BACKOFF_MIN_MS;
    bool synced = false, auth_logged = false;
    int64_t seq = 0;

    set_state("syncing");
    for (;;) {
        if (!wifi_sta_wait_connected(5000)) {
            continue;
        }
        char path[64];
        size_t len = 0;
        result_t r;
        if (!synced) {
            r = http_get("/locate/latest", body, sizeof(body), &len);
        } else {
            snprintf(path, sizeof(path), "/locate/next?after=%lld&wait=%d", (long long)seq, WAIT_S);
            r = http_get(path, body, sizeof(body), &len);
        }

        if (r == R_NO_EVENT && synced) {
            note_ok(-1);
            continue; /* long-poll timed out: poll again */
        }
        if (r == R_EVENT) {
            locate_info_t info;
            locate_rc_t rc = locate_apply(body, len, cabinet, CONFIG_SIOT_DRAWER_MAP, states,
                                          CONFIG_SIOT_LED_COUNT, &info);
            if (rc == LOCATE_OK) {
                if (!synced) {
                    /* Boot/resync: learn the seq only; light nothing. */
                    seq = info.seq;
                    synced = true;
                    ESP_LOGI(TAG, "synced at seq %lld", (long long)seq);
                } else {
                    seq = info.seq;
                    ESP_LOGI(TAG, "event seq %lld, %u drawers, http 200", (long long)seq, info.drawer_count);
                    esp_err_t le = led_glow_set_states(states, CONFIG_SIOT_LED_COUNT, info.ttl_ms);
                    if (le != ESP_OK) {
                        ESP_LOGW(TAG, "LED update dropped: %s", esp_err_to_name(le));
                    }
                }
                note_ok(seq);
                set_state("ok");
                backoff_ms = BACKOFF_MIN_MS;
                auth_logged = false;
                continue;
            }
            ESP_LOGW(TAG, "bad event payload (rc %d), no change", (int)rc);
            r = R_ERROR;
        }

        synced = false; /* any failure: re-learn seq from /latest after the backoff */
        if (r == R_AUTH) {
            if (!auth_logged) {
                ESP_LOGE(TAG, "key rejected; retrying in 5 min");
                auth_logged = true;
            }
            set_state("auth_failed");
            sleep_ms(AUTH_BACKOFF_MS);
        } else {
            set_state("error");
            ESP_LOGW(TAG, "retry in %u ms", (unsigned)backoff_ms);
            sleep_ms(backoff_ms);
            backoff_ms = backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : backoff_ms * 2;
        }
    }
}

esp_err_t ims_locate_start(void)
{
    if (CONFIG_SIOT_IMS_URL[0] == '\0' || CONFIG_SIOT_IMS_TOKEN[0] == '\0') {
        ESP_LOGW(TAG, "IMS_URL/IMS_TOKEN not set: poller disabled");
        return ESP_OK;
    }
#if CONFIG_SIOT_IMS_MOCK_CA
    ESP_LOGW(TAG, "TLS trust: MOCK CA ONLY (dev build, cannot reach the real IMS)");
#else
#if CONFIG_SIOT_DEV_BUILD
    ESP_LOGI(TAG, "TLS trust: certificate bundle (dev build)");
#else
    ESP_LOGI(TAG, "TLS trust: certificate bundle");
#endif
#endif
    return xTaskCreate(poller_task, "ims_locate", 10240, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
