idf_component_register(SRCS st_core.c
                       INCLUDE_DIRS include
                       REQUIRES st_phy st_link st_codec st_sched
                       PRIV_REQUIRES esp_timer esp_hw_support freertos)
