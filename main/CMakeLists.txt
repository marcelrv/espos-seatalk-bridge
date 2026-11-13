idf_component_register(SRCS main.c st_bench.c
                       INCLUDE_DIRS .
                       PRIV_REQUIRES espos_core espos_sk espos_ota esp_driver_gpio esp_driver_uart
                                     nvs_flash esp_timer esp_system esp_hw_support freertos
                                     st_phy st_link)
