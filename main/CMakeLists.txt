idf_component_register(SRCS "app_main.c" "led_glow.c" "wifi_sta.c" "web_server.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_timer freertos esp_wifi esp_netif esp_event
                                     nvs_flash esp_http_server json
                       EMBED_TXTFILES "index.html")
