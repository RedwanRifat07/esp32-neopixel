set(embed_files "index.html")
if(CONFIG_SIOT_IMS_MOCK_CA)
    # Bench builds only: the mock server's CA (generate with tools/mock_ims.py's openssl recipe).
    set(mock_ca "${CMAKE_CURRENT_SOURCE_DIR}/../certs/mock_ca.pem")
    if(NOT EXISTS "${mock_ca}")
        message(FATAL_ERROR "IMS_MOCK_CA=1 needs certs/mock_ca.pem (the mock server's certificate)")
    endif()
    list(APPEND embed_files "${mock_ca}")
endif()

idf_component_register(SRCS "app_main.c" "led_glow.c" "locate_apply.c" "ims_locate.c" "wifi_sta.c" "web_server.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_timer freertos esp_wifi esp_netif esp_event
                                     nvs_flash esp_http_server esp_http_client esp-tls mbedtls json
                       EMBED_TXTFILES ${embed_files})
