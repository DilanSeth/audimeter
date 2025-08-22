# main/CMakeLists.txt
idf_component_register(
    SRCS "tv_audience_main.c"
    INCLUDE_DIRS "."
    REQUIRES 
        nvs_flash
        esp_wifi
        esp_event
        esp_http_client
        esp_netif
        driver
        esp_system
        freertos
        lwip
        esp-dsp
        cjson
    PRIV_REQUIRES
        spi_flash
)