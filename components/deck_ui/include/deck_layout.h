/*
 * Screen-shape helpers. The UI was laid out for the Tab5's 1280x720
 * landscape panel; other shapes re-flow the same widgets:
 *
 *   WIDE   landscape at least DECK_SHORT_MAX_H tall: the Tab5 (1280x720),
 *          the original layout
 *   SHORT  landscape but shorter: the T-Display P4 turned on its side
 *          (1232x568); the wide layout with vertical space trimmed
 *   TALL   portrait (T-Display P4, 568x1232): side-by-side panes stack and
 *          fixed widths clamp to the screen
 *
 * The screen can turn at runtime, so these are evaluated whenever a screen
 * is built (the shell rebuilds, or calls on_relayout, after a rotation).
 * Never keep their results in statics or use them in initializers.
 */
#pragma once

#include "deck_hal.h"
#include "lvgl.h"

typedef enum {
    DECK_SHAPE_WIDE,
    DECK_SHAPE_SHORT,
    DECK_SHAPE_TALL,
} deck_shape_t;

/* Landscape screens shorter than this get the SHORT layout. */
#define DECK_SHORT_MAX_H 640

static inline deck_shape_t deck_shape(void)
{
    int w = hal_screen_w(), h = hal_screen_h();
    if (h > w) return DECK_SHAPE_TALL;
    return h < DECK_SHORT_MAX_H ? DECK_SHAPE_SHORT : DECK_SHAPE_WIDE;
}

/* A landscape screen with little height (SHORT). */
#define DECK_SHORT (deck_shape() == DECK_SHAPE_SHORT)

/* `land` on a wide or short landscape screen, `port` on a tall one. */
#define DECK_LP(land, port) (HAL_PORTRAIT ? (port) : (land))

/* One value per shape: `land` WIDE (Tab5), `short_` SHORT, `port` TALL. */
#define DECK_LSP(land, short_, port) \
    (deck_shape() == DECK_SHAPE_TALL ? (port) : deck_shape() == DECK_SHAPE_SHORT ? (short_) : (land))

/* A fixed width that still leaves `margin` px on both sides of the screen. */
#define DECK_FIT_W(w, margin) LV_MIN((int32_t)(w), (int32_t)(HAL_SCREEN_W - 2 * (margin)))

/* Panes side by side on a landscape screen, stacked on a tall one. */
#define DECK_FLOW_SPLIT DECK_LP(LV_FLEX_FLOW_ROW, LV_FLEX_FLOW_COLUMN)

/* On-screen keyboard height: a share of a wide screen, a fixed comfortable
 * height on a tall one (42% of 1232 px would be mostly key padding) and on
 * a short one (where every line of text counts). */
#define DECK_OSK_H(land_pct) DECK_LSP(LV_PCT(land_pct), 220, 330)
