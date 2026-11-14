# SeaTalk 1 <-> Signal K bridge (espOS, ESP-IDF 6.0).
# Design: ../SeaTalkCore_SPEC.md   Hardware: ../SeaTalkRemote_FIRMWARE.md
#
# espOS is the `espos/` git submodule. Its prologue sets the IDF version pin,
# sdkconfig defaults, partition table, component dirs and the signing key.
# The SeaTalk core (st_phy, st_link, st_sched, st_codec) lives in components/
# and does not depend on espOS.
cmake_minimum_required(VERSION 3.22)

# Local fixes to the espOS submodule (patches/espos-*.patch), applied at
# configure time and skipped when already applied. Candidates for upstream.
find_package(Git QUIET)
file(GLOB _espos_patches "${CMAKE_CURRENT_LIST_DIR}/patches/espos-*.patch")
list(SORT _espos_patches)
foreach(_p IN LISTS _espos_patches)
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --reverse --check --ignore-whitespace "${_p}"
                    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/espos" RESULT_VARIABLE _applied
                    OUTPUT_QUIET ERROR_QUIET)
    if(NOT _applied EQUAL 0)
        execute_process(COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace "${_p}"
                        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/espos" RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "espOS patch ${_p} does not apply (submodule updated?)")
        endif()
        message(STATUS "applied ${_p}")
    endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/espos/cmake/espos_project.cmake")
espos_project_prologue(NAME "seatalk-bridge")
project(seatalk_bridge)
espos_project_ui_partition()
