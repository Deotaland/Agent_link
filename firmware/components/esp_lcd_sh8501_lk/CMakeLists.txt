idf_component_register(SRCS "esp_lcd_sh8501_lk.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_gpio esp_driver_spi freertos
                       REQUIRES "esp_lcd")
