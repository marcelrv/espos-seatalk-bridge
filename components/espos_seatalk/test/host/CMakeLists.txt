# SPDX-FileCopyrightText: 2026 Marcel Verpaalen
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.16)
project(seatalk_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

enable_testing()

get_filename_component(ST_COMP "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

if(MSVC)
  add_compile_options(/W4 /WX)
else()
  add_compile_options(-Wall -Wextra -Werror)
endif()

# Code under test: pure C layers of espos_seatalk, no ESP-IDF.
add_library(st_link STATIC ${ST_COMP}/src/link/st_link.c)
target_include_directories(st_link PUBLIC ${ST_COMP}/include)

function(st_add_host_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE st_link)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

add_library(st_codec STATIC ${ST_COMP}/src/codec/st_codec.c)
target_include_directories(st_codec PUBLIC ${ST_COMP}/include)
if(NOT MSVC)
  target_link_libraries(st_codec PUBLIC m)
endif()

add_library(st_sched STATIC ${ST_COMP}/src/sched/st_sched.c)
target_include_directories(st_sched PUBLIC ${ST_COMP}/include)
target_link_libraries(st_sched PUBLIC st_codec)

st_add_host_test(test_st_link test_st_link.c)
st_add_host_test(test_st_codec test_st_codec.c)
target_link_libraries(test_st_codec PRIVATE st_codec)

st_add_host_test(test_st_sched test_st_sched.c)
target_link_libraries(test_st_sched PRIVATE st_sched)

add_library(st_sk_map STATIC ${ST_COMP}/src/sk/st_sk_map.c)
target_include_directories(st_sk_map PUBLIC ${ST_COMP}/src/sk ${ST_COMP}/include)
target_link_libraries(st_sk_map PUBLIC st_codec)

st_add_host_test(test_st_sk_map test_st_sk_map.c)
target_link_libraries(test_st_sk_map PRIVATE st_sk_map)
target_compile_definitions(test_st_sk_map PRIVATE
  ST_SK_DESCRIPTOR="${ST_COMP}/config/seatalk.json")
