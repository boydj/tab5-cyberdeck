idf_build_get_property(deck_board DECK_BOARD)
if(deck_board STREQUAL "tdisplay_p4")
    set(aout_src "aout_tdisplay_p4.c")
else()
    set(aout_src "aout_tab5.c")
endif()

idf_component_register(
    SRCS "player.c" "id3.c" "${aout_src}"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES deck_hal
    PRIV_REQUIRES minimp3 esp_driver_i2s esp_driver_i2c esp_timer espressif__esp_codec_dev
)
