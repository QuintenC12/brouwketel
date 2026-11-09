idf_component_register(
    SRCS "scherm_uart.c"
    INCLUDE_DIRS "."
    REQUIRES freertos
    PRIV_REQUIRES esp_driver_uart json
)
