/*
 * Screen size as the UI sees it: the LVGL display's resolution, which
 * follows rotation. Shared by every deck_hal build, the simulator included.
 */
#include "deck_hal.h"

#include "lvgl.h"

int hal_screen_w(void)
{
    lv_display_t *d = lv_display_get_default();
    return d ? (int)lv_display_get_horizontal_resolution(d) : HAL_PANEL_W;
}

int hal_screen_h(void)
{
    lv_display_t *d = lv_display_get_default();
    return d ? (int)lv_display_get_vertical_resolution(d) : HAL_PANEL_H;
}
