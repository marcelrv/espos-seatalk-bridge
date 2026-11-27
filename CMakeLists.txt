# SPDX-FileCopyrightText: 2026 Marcel Verpaalen
# SPDX-License-Identifier: Apache-2.0
# SeaTalk 1 <-> Signal K bridge (espOS, ESP-IDF 6).
# Design: docs/protocol.md   Hardware: docs/hardware.md
#
# espOS is the `espos/` git submodule. Its prologue sets the IDF version pin,
# sdkconfig defaults, partition table, component dirs and the signing key.
# The reusable SeaTalk 1 stack lives in components/espos_seatalk/.
cmake_minimum_required(VERSION 3.22)

include("${CMAKE_CURRENT_LIST_DIR}/espos/cmake/espos_project.cmake")
espos_project_prologue(NAME "seatalk-bridge")
project(seatalk_bridge)
espos_project_ui_partition()
