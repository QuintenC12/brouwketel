idf_component_register(SRCS "main.c" "net.c"
                    INCLUDE_DIRS "."
                    REQUIRES max31865 scherm_uart json esp_driver_gpio esp_timer
                             esp_wifi esp_netif esp_event nvs_flash mqtt)
