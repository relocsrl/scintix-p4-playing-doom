idf_component_register(SRCS "doom_p4_main.c" "usb_gamepad.c" "panel_display.c" "doom_port.c"
                       PRIV_REQUIRES esp_lcd esp_driver_gpio esp_hw_support nvs_flash esp_event esp_netif esp_wifi esp_http_server dns_server usb spiffs esp_timer doomgeneric
                       INCLUDE_DIRS "")
