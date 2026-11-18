idf_component_register(SRCS "main.c" "net.c" "recept.c" "pid.c"
                    INCLUDE_DIRS "."
                    REQUIRES max31865 scherm_uart brouw_netwerk json mqtt
                             esp_driver_gpio esp_timer)
