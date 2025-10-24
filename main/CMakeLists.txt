idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES 
        bt
        esp_wifi
        fatfs
        driver
        esp_event
        nvs_flash
        sdmmc
)
