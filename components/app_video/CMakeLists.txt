idf_component_register(SRCS "app_video.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_video espressif__esp_cam_sensor esp_driver_ppa lvgl espressif__esp32_p4_function_ev_board)
