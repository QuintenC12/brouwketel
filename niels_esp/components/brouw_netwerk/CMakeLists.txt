idf_component_register(
    SRCS "brouw_netwerk.c" "instellingen.c" "portaal.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_event esp_netif esp_wifi esp_http_server nvs_flash
    EMBED_TXTFILES "formulier.html"
)
