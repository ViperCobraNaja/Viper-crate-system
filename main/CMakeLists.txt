idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES app_video motion_detector video_recorder storage_manager cloud_interface esp_image_effects)