#include "web_server.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "led_glow.h"
#include "sdkconfig.h"

#define MAX_BODY 127
#define MAX_TOKEN 64

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static bool token_required(void)
{
    return CONFIG_SIOT_WEB_TOKEN[0] != '\0';
}

/* Constant-time token compare; true if no token is configured. */
static bool authorized(httpd_req_t *req)
{
    if (!token_required()) {
        return true;
    }
    size_t len = httpd_req_get_hdr_value_len(req, "X-Auth-Token");
    if (len == 0 || len > MAX_TOKEN) {
        return false;
    }
    char got[MAX_TOKEN + 1];
    if (httpd_req_get_hdr_value_str(req, "X-Auth-Token", got, sizeof(got)) != ESP_OK) {
        return false;
    }
    const char *want = CONFIG_SIOT_WEB_TOKEN;
    size_t want_len = strlen(want);
    unsigned diff = (unsigned)(len ^ want_len);
    for (size_t i = 0; i < len; i++) {
        diff |= (unsigned)(got[i] ^ want[i < want_len ? i : 0]);
    }
    return diff == 0;
}

static bool is_json_request(httpd_req_t *req)
{
    char ct[40];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct)) != ESP_OK) {
        return false;
    }
    return strncasecmp(ct, "application/json", 16) == 0;
}

/* Common POST gate. Returns ESP_OK to continue, or after having sent the error reply. */
static esp_err_t check_post(httpd_req_t *req)
{
    if (!is_json_request(req)) {
        send_json(req, "415 Unsupported Media Type", "{\"error\":\"content-type must be application/json\"}");
        return ESP_FAIL;
    }
    if (!authorized(req)) {
        send_json(req, "401 Unauthorized", "{\"error\":\"missing or wrong X-Auth-Token\"}");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
                       "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'");
    /* EMBED_TXTFILES appends a NUL; don't send it. */
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t status_get(httpd_req_t *req)
{
    char body[80];
    snprintf(body, sizeof(body), "{\"drawers\":%d,\"glow_ms\":%d,\"auth\":%s}",
             CONFIG_SIOT_LED_COUNT, CONFIG_SIOT_GLOW_MS, token_required() ? "true" : "false");
    return send_json(req, "200 OK", body);
}

static esp_err_t glow_post(httpd_req_t *req)
{
    if (check_post(req) != ESP_OK) {
        return ESP_OK;
    }
    if (req->content_len == 0 || req->content_len > MAX_BODY) {
        return send_json(req, "400 Bad Request", "{\"error\":\"body must be 1..127 bytes of JSON\"}");
    }
    char buf[MAX_BODY + 1];
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += (size_t)r;
    }
    buf[got] = '\0';

    cJSON *root = cJSON_Parse(buf);
    const cJSON *d = root ? cJSON_GetObjectItemCaseSensitive(root, "drawer") : NULL;
    if (!cJSON_IsNumber(d) || d->valuedouble != (double)(int)d->valuedouble) {
        cJSON_Delete(root);
        return send_json(req, "400 Bad Request", "{\"error\":\"expected {\\\"drawer\\\": <integer>}\"}");
    }
    int drawer = (int)d->valuedouble;
    cJSON_Delete(root);

    esp_err_t err = (drawer >= 1) ? led_glow_trigger((unsigned)drawer) : ESP_ERR_INVALID_ARG;
    if (err == ESP_ERR_INVALID_ARG) {
        char msg[64];
        snprintf(msg, sizeof(msg), "{\"error\":\"drawer must be 1..%d\"}", CONFIG_SIOT_LED_COUNT);
        return send_json(req, "400 Bad Request", msg);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "trigger failed: %s", esp_err_to_name(err));
        return send_json(req, "503 Service Unavailable", "{\"error\":\"led busy, retry\"}");
    }
    char ok[48];
    snprintf(ok, sizeof(ok), "{\"ok\":true,\"drawer\":%d}", drawer);
    return send_json(req, "200 OK", ok);
}

static esp_err_t off_post(httpd_req_t *req)
{
    if (check_post(req) != ESP_OK) {
        return ESP_OK;
    }
    if (led_glow_clear_all() != ESP_OK) {
        return send_json(req, "503 Service Unavailable", "{\"error\":\"led busy, retry\"}");
    }
    return send_json(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t favicon_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }
    static const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/glow", .method = HTTP_POST, .handler = glow_post},
        {.uri = "/api/off", .method = HTTP_POST, .handler = off_post},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s: %s", routes[i].uri, esp_err_to_name(err));
            httpd_stop(server);
            return err;
        }
    }
    ESP_LOGI(TAG, "listening on port %d (auth %s)", cfg.server_port, token_required() ? "on" : "OFF");
    return ESP_OK;
}
