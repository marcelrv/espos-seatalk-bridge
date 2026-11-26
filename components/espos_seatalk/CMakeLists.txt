# SPDX-FileCopyrightText: 2026 Marcel Verpaalen
# SPDX-License-Identifier: Apache-2.0
#
# espos_seatalk: Raymarine SeaTalk 1 protocol stack (9-bit UART PHY, link
# framing, codec, polite TX scheduler, core task) and optional Signal K adapter.
idf_build_get_property(target IDF_TARGET)

set(srcs
    src/link/st_link.c
    src/codec/st_codec.c
    src/sched/st_sched.c
    src/core/st_core.c
    src/sk/st_sk_map.c
    src/sk/st_sk.c
    src/espos_seatalk.c)

set(priv_reqs esp_timer esp_hw_support freertos)
if(target STREQUAL "linux")
    list(APPEND srcs src/phy/st_phy_sim.c)
else()
    list(APPEND srcs src/phy/st_phy.c)
    list(APPEND priv_reqs esp_driver_uart esp_driver_gpio esp_hal_uart)
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS include
                       PRIV_INCLUDE_DIRS src/sk
                       REQUIRES esp_common
                       PRIV_REQUIRES ${priv_reqs})

# Signal K adapter and config descriptor are wired in only when espOS is part
# of the build. Namespace-tolerant checks work for both in-tree/submodule
# builds (espos_sk) and registry installs (signalk-espos__espos_sk).
include("${CMAKE_CURRENT_LIST_DIR}/cmake/espos_components.cmake")

espos_optional_requires(PRIVATE espos_sk espos_config espos_net)
espos_has_component(_have_sk espos_sk)
espos_has_component(_have_cfg espos_config)
espos_has_component(_have_net espos_net)

if(_have_sk AND _have_cfg AND _have_net)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ESPOS_SEATALK_HAVE_SK=1)
endif()

if(_have_cfg AND COMMAND espos_config_add_descriptor)
    espos_config_add_descriptor(config/seatalk.json)
endif()
