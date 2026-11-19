# idf_component_register(
#     SRCS "main.cpp"
#     INCLUDE_DIRS "."
#     PRIV_REQUIRES
#         esp_adc
#         esp_driver_gpio
#         led_strip
#         esp_event
#         esp_netif
#         esp_system
#         esp_wifi
#         freertos
#         log
#         nvs_flash
# )
idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES 
                    esp_wifi 
                    esp_event 
                    nvs_flash 
                    esp_http_server 
                    esp_netif 
                    esp_adc
                    EMBED_FILES "index.html")