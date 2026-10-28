idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_wifi esp_netif esp_event nvs_flash mqtt json esp_timer)
