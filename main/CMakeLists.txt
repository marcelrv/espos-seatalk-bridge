idf_component_register(SRCS main.c st_bench.c st_sim.c st_con.c
                       INCLUDE_DIRS .
                       PRIV_REQUIRES espos_core espos_sk espos_ota esp_driver_gpio esp_driver_uart
                                     nvs_flash esp_timer esp_system esp_hw_support freertos lwip esp_app_format
                                     st_phy st_link st_codec st_sched st_core st_sk)
