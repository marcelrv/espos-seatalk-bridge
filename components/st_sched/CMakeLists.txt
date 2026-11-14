# SPDX-FileCopyrightText: 2026 Marcel Verpaalen
# SPDX-License-Identifier: Apache-2.0
idf_component_register(SRCS st_sched.c
                       INCLUDE_DIRS include
                       REQUIRES st_codec st_phy)
