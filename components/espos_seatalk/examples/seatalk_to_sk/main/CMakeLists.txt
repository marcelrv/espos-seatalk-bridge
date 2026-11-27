# SPDX-FileCopyrightText: 2026 Marcel Verpaalen
# SPDX-License-Identifier: Apache-2.0
idf_component_register(SRCS main.c
                       INCLUDE_DIRS .
                       PRIV_REQUIRES espos_core espos_sk espos_ota espos_seatalk)
