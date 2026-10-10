/* Simulator-only hooks. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Queue a keyboard event exactly as the Tab5 Keyboard would report it. */
void sim_push_key(uint8_t code, uint8_t mods, bool pressed);

/* Pretend the motion sensor sees the deck held at this rotation. */
void sim_set_tilt(int rot);
/* main.c: the display changed size (rotation); rebind its buffers. */
void sim_display_resized(void);
