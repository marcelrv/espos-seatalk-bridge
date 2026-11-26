# SPDX-FileCopyrightText: 2026 Marcel Verpaalen
# SPDX-License-Identifier: Apache-2.0
set(srcs main.c st_api.c st_leds.c)
if(CONFIG_ST_BRIDGE_BENCH)
    list(APPEND srcs st_con.c st_bench.c)
endif()
if(CONFIG_ST_BRIDGE_SIM)
    list(APPEND srcs st_sim.c)
endif()

set(priv_reqs espos_core espos_sk espos_ota espos_config espos_httpd espos_seatalk
              esp_driver_gpio esp_driver_uart esp_timer esp_system esp_hw_support
              freertos esp_app_format)
if(CONFIG_ST_BRIDGE_BENCH)
    list(APPEND priv_reqs nvs_flash lwip esp_driver_usb_serial_jtag)
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS .
                       PRIV_REQUIRES ${priv_reqs})

espos_config_add_descriptor(config/seatalk_hw.json)
if(CONFIG_ST_BRIDGE_TELNET)
    espos_config_add_descriptor(config/seatalk_console.json)
endif()
