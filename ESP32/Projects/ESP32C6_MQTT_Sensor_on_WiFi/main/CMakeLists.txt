idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_adc
        esp_driver_gpio
        led_strip
        esp_event
        esp_netif
        esp_system
        esp_wifi
        freertos
        log
        mqtt
        nvs_flash
)
