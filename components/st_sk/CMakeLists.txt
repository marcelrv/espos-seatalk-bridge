idf_component_register(SRCS st_sk.c st_sk_map.c
                       INCLUDE_DIRS include
                       PRIV_INCLUDE_DIRS .
                       REQUIRES st_codec
                       PRIV_REQUIRES st_core st_link st_sched st_phy espos_sk espos_config espos_net
                                     esp_timer freertos)
espos_config_add_descriptor(config/seatalk.json)
