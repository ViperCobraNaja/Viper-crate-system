idf_component_register(
    SRCS "wifi_manager.c" "http_server.c"
    INCLUDE_DIRS "."
    REQUIRES esp_netif esp_event esp_wifi nvs_flash esp_http_server espressif__esp_wifi_remote wpa_supplicant
)
