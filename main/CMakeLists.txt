# Embed the server root certificate into the final binary
idf_build_get_property(project_dir PROJECT_DIR)
idf_component_register(SRCS "main_app.c" "ota_hal.c" "wifi.c"
                    INCLUDE_DIRS "."
                    REQUIRES 
                        esp_wifi
                        esp_event
                        esp_netif
                        nvs_flash
                        esp_https_ota
                        esp_http_client
                        app_update
                        esp_driver_gpio
                        mbedtls)
