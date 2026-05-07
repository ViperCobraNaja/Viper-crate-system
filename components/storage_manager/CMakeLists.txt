idf_component_register(SRCS "storage_manager.c"
                    INCLUDE_DIRS "."
                    REQUIRES video_recorder esp_muxer fatfs sdmmc spi_flash vfs esp_timer espressif__esp32_p4_function_ev_board)
