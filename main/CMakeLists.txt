idf_component_register(
    SRCS 
        "ws_matrix.c" 
        "main.c" 
        "llm.c" 
        "captive_portal.c"
        "wifi_manager.c"
        "motion_sensor.c"
        "button_manager.c"
    INCLUDE_DIRS 
        ""
    REQUIRES
        esp_wifi
        esp_http_server
        lwip
        esp_netif
        nvs_flash
        spiffs
        driver
        esp_timer
        esp_hw_support    # Questo include esp_random
    PRIV_REQUIRES
        esp_psram
    LDFRAGMENTS 
        "../linker.lf"
)

# Performance optimization for LLM
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-if-conversion)

# Create SPIFFS image
spiffs_create_partition_image(data ../data FLASH_IN_PROJECT)