idf_component_register(SRCS "esp_lcd_gc9d01.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_gpio esp_driver_spi freertos
                       REQUIRES "esp_lcd")
