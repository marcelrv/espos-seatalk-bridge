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

# The hardware descriptor shows this build's pins as defaults and in its text.
idf_build_get_property(_st_target IDF_TARGET)
set(ST_HW_TARGET "${_st_target}")
set(ST_HW_RX "${CONFIG_ST_BRIDGE_RX_GPIO}")
set(ST_HW_TX "${CONFIG_ST_BRIDGE_TX_GPIO}")
set(ST_HW_LED "${CONFIG_ST_BRIDGE_LED_EN_GPIO}")
if(ST_HW_LED LESS 0)
    set(ST_HW_LED_TXT "none")
else()
    set(ST_HW_LED_TXT "GPIO ${ST_HW_LED}")
endif()
configure_file(config/seatalk_hw.json.in "${CMAKE_CURRENT_BINARY_DIR}/config/seatalk_hw.json" @ONLY)
espos_config_add_descriptor("${CMAKE_CURRENT_BINARY_DIR}/config/seatalk_hw.json")
if(CONFIG_ST_BRIDGE_TELNET)
    espos_config_add_descriptor(config/seatalk_console.json)
endif()
