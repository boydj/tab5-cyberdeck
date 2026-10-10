/*
 * Screen-shape helpers. The UI was laid out for the Tab5's 1280x720
 * landscape panel; on a tall screen (HAL_PORTRAIT, e.g. the T-Display P4 at
 * 568x1232) the same widgets re-flow: side-by-side panes stack and fixed
 * widths clamp to the screen. Everything here is a compile-time constant,
 * so the landscape build compiles to exactly the layout it always had.
 */
#pragma once

#include "deck_hal.h"
#include "lvgl.h"

/* `land` on a wide screen, `port` on a tall one. */
#define DECK_LP(land, port) (HAL_PORTRAIT ? (port) : (land))

/* A fixed width that still leaves `margin` px on both sides of the screen. */
#define DECK_FIT_W(w, margin) LV_MIN((int32_t)(w), (int32_t)(HAL_SCREEN_W - 2 * (margin)))

/* Panes side by side on a wide screen, stacked on a tall one. */
#define DECK_FLOW_SPLIT DECK_LP(LV_FLEX_FLOW_ROW, LV_FLEX_FLOW_COLUMN)

/* On-screen keyboard height: a share of a wide screen, a fixed comfortable
 * height on a tall one (42% of 1232 px would be mostly key padding). */
#define DECK_OSK_H(land_pct) DECK_LP(LV_PCT(land_pct), 330)
