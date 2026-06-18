idf_component_register(
    SRCS "z_pub.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event nvs_flash freertos zenoh-pico
)
