cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(ble_temp_node)

target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c ${CMAKE_CURRENT_SOURCE_DIR}/src/temp_sensor.c ${CMAKE_CURRENT_SOURCE_DIR}/src/app_timer.c ${CMAKE_CURRENT_SOURCE_DIR}/src/ble_service.c)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)