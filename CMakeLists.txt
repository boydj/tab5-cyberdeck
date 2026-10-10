# DECK//OS: cyberdeck firmware for the M5Stack Tab5 + Tab5 Keyboard and the
# LilyGo T-Display P4 (AMOLED) + K270 keyboard.
#
#   idf.py build                                   # Tab5 (default)
#   idf.py -B build-tdisplay_p4 -DDECK_BOARD=tdisplay_p4 build
cmake_minimum_required(VERSION 3.16)

set(DECK_BOARD "tab5" CACHE STRING "Target board: tab5 or tdisplay_p4")

# Apps live in their own folder so each one is an isolated component.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/apps")

if(DECK_BOARD STREQUAL "tdisplay_p4")
    # Board defaults on top of the shared ones, and a separate sdkconfig so
    # switching boards never mixes settings.
    set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.tdisplay_p4")
    if(NOT DEFINED SDKCONFIG)
        set(SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.tdisplay_p4")
    endif()
    set(EXCLUDE_COMPONENTS m5_tab5_component m5_tab5_keyboard_component)
elseif(NOT DECK_BOARD STREQUAL "tab5")
    message(FATAL_ERROR "DECK_BOARD must be tab5 or tdisplay_p4 (got '${DECK_BOARD}')")
endif()

# Offline builds: a folder of pre-fetched managed components (lvgl__lvgl,
# espressif__esp_lvgl_port, ...) used with IDF_COMPONENT_MANAGER=0.
if(DECK_LOCAL_DEPS)
    list(APPEND EXTRA_COMPONENT_DIRS ${DECK_LOCAL_DEPS})
endif()

# Stored in the image header; OTA compares against GitHub release tags.
# Keep in step with DECK_VERSION in components/deck_core/include/deck_shell.h.
set(PROJECT_VER "1.1.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Components pick their board sources from this build property: component
# requirements are resolved before Kconfig runs, so they cannot use CONFIG_*.
idf_build_set_property(DECK_BOARD "${DECK_BOARD}")
# The project name is stored in the image and OTA refuses images with another
# name, so each board gets its own: a Tab5 never installs a T-Display image.
if(DECK_BOARD STREQUAL "tdisplay_p4")
    project(deck_tdisplay_p4)
else()
    project(tab5_cyberdeck)
endif()
