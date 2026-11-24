# SPDX-FileCopyrightText: 2026 Marcel Verpaalen
# SPDX-License-Identifier: Apache-2.0
set(srcs main.c st_con.c st_leds.c)
if(CONFIG_ST_BRIDGE_BENCH)
    list(APPEND srcs st_bench.c)
endif()
if(CONFIG_ST_BRIDGE_SIM)
    list(APPEND srcs st_sim.c)
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS .
                       PRIV_REQUIRES espos_core espos_sk espos_ota espos_config esp_driver_gpio esp_driver_uart
                                     nvs_flash esp_timer esp_system esp_hw_support freertos lwip esp_app_format
                                     st_phy st_link st_codec st_sched st_core st_sk esp_driver_usb_serial_jtag)
espos_config_add_descriptor(config/seatalk_console.json)
