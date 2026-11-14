# SPDX-FileCopyrightText: 2026 Marcel Verpaalen
# SPDX-License-Identifier: Apache-2.0
idf_component_register(SRCS st_phy.c
                       INCLUDE_DIRS include
                       PRIV_REQUIRES esp_driver_uart esp_driver_gpio esp_timer esp_hal_uart)
