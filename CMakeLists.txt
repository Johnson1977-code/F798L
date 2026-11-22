cmake_minimum_required(VERSION 3.20.0)

list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(F798L)

target_sources(app PRIVATE src/main.c)

zephyr_library_sources_ifdef(CONFIG_DISPLAY_ST7801N
    drivers/display/panel_st7801n.c)