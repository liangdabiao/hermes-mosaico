idf_component_register(SRCS "hermes_link.c" "hermes_wifi.c" "hermes_ws.c" "hermes_wire.c" "hermes_crypto.c"
    INCLUDE_DIRS "include"
    REQUIRES companion_credentials nvs_flash esp_wifi esp_netif esp_event esp_timer heap
    PRIV_REQUIRES esp_websocket_client)
