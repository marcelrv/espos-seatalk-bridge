# SPDX-FileCopyrightText: 2026 Marcel Verpaalen
# SPDX-License-Identifier: Apache-2.0
#
# Standalone SeaTalk 1 -> Signal K example consuming espOS from the ESP
# Component Registry (no submodule or prologue required).
cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(seatalk_to_sk)
espos_project_ui_partition()
