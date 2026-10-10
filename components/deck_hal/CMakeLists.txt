# Board selection: the DECK_BOARD build property (from -DDECK_BOARD=<name>,
# see the top-level CMakeLists.txt) picks sources and requirements, because
# requirements are resolved before Kconfig. The Kconfig choice DECK_BOARD,
# set by the board's sdkconfig defaults, is what the C code sees.
idf_build_get_property(deck_board DECK_BOARD)
if(NOT deck_board)
    set(deck_board "tab5")
endif()

if(deck_board STREQUAL "tdisplay_p4")
    if(DEFINED CONFIG_DECK_BOARD_TAB5 AND CONFIG_DECK_BOARD_TAB5)
        message(FATAL_ERROR "DECK_BOARD=tdisplay_p4 but sdkconfig selects the Tab5; delete the sdkconfig or use a separate build dir")
    endif()
    idf_component_register(
        SRCS "hal_common.c" "hal_geometry.c" "hal_tdisplay_p4.c" "tdp4_keyboard.c" "tdp4_touch.c" "tdp4_orient.c"
        INCLUDE_DIRS "include"
        REQUIRES lvgl__lvgl
        PRIV_REQUIRES
            espressif__esp_lvgl_port
            espressif__esp_hosted
            esp_lcd
            esp_driver_i2c
            esp_driver_gpio
            esp_driver_ledc
            esp_driver_sdmmc
            esp_hw_support
            esp_driver_ppa
            esp_mm
            sdmmc
            fatfs
            wear_levelling
            nvs_flash
            nvs_sec_provider
            efuse
            esp_timer
    )
else()
    if(DEFINED CONFIG_DECK_BOARD_TDISPLAY_P4 AND CONFIG_DECK_BOARD_TDISPLAY_P4)
        message(FATAL_ERROR "sdkconfig selects the T-Display P4 but DECK_BOARD is tab5; pass -DDECK_BOARD=tdisplay_p4")
    endif()
    idf_component_register(
        SRCS "hal_tab5.cpp" "hal_common.c" "hal_geometry.c"
        INCLUDE_DIRS "include"
        REQUIRES lvgl__lvgl
        PRIV_REQUIRES
            m5_tab5_component
            m5_tab5_keyboard_component
            esp_driver_ledc
            esp_driver_sdmmc
            sdmmc
            fatfs
            wear_levelling
            nvs_flash
            nvs_sec_provider
            efuse
            esp_timer
    )
endif()
