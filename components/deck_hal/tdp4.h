/*
 * LilyGo T-Display P4 v1.0 board internals shared by hal_tdisplay_p4.c and
 * its keyboard and touch helpers. Not part of the public HAL API.
 *
 * Pin map and bring-up order follow LilyGo's lilygo_device_driver
 * (device/t_display_p4/v1/config.h and keyboard_expansion_config.h) and the
 * board schematics in Xinyuan-LilyGO/T-Display-P4. Only v1.0 (ESP32-C6
 * radio, BQ27220 gauge, XL9535 expander) is supported; v2.0 boards use an
 * ESP32-C5, an AXP517 and different pins.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Buses ---------------------------------------------------------------- */

/* Main bus (LP I2C): XL9535, BQ27220, PCF8563, GT9895 touch. */
#define TDP4_MAIN_SDA 7
#define TDP4_MAIN_SCL 8
/* Audio/sensor bus (I2C1): ES8311 codec, AW86224 haptics, ICM20948. */
#define TDP4_AUDIO_SDA 20
#define TDP4_AUDIO_SCL 21
/* K270 keyboard bus (I2C0) on the 1x4 expansion header: XL9555, TCA8418. */
#define TDP4_KBD_SDA 46
#define TDP4_KBD_SCL 45
/* TCA8418 INT (pulled up on the keyboard). */
#define TDP4_KBD_INT 48
/* SY7200A key backlight driver EN/PWM. */
#define TDP4_KBD_BL 47

#define TDP4_ADDR_XL9535  0x20
#define TDP4_ADDR_BQ27220 0x55
#define TDP4_ADDR_PCF8563 0x51
#define TDP4_ADDR_GT9895  0x5D
#define TDP4_ADDR_ES8311  0x18
#define TDP4_ADDR_XL9555  0x20 /* keyboard bus */
#define TDP4_ADDR_TCA8418 0x34 /* keyboard bus */

/* ---- XL9535 IO expander (main bus) ---------------------------------------- */
/* Pins 0..7 are port 0, 10..17 port 1 (LilyGo's numbering). */
enum {
    TDP4_XL_PWR_3V3   = 0,  /* peripheral 3V3 enable, active LOW, must stay on */
    TDP4_XL_SKY13453  = 1,  /* RF switch control */
    TDP4_XL_LCD_RST   = 2,  /* active LOW */
    TDP4_XL_TP_RST    = 3,  /* active LOW */
    TDP4_XL_TP_INT    = 4,
    TDP4_XL_ETH_RST   = 5,  /* active LOW, Ethernet kept in reset */
    TDP4_XL_AUDIO_EN  = 6,  /* audio power, active HIGH */
    TDP4_XL_IMU_INT   = 7,
    TDP4_XL_USBPHY_EN = 10, /* keep on: off costs ~20 mA (LilyGo) */
    TDP4_XL_GPS_WAKE  = 11,
    TDP4_XL_RTC_INT   = 12,
    TDP4_XL_C6_WAKE   = 13,
    TDP4_XL_C6_EN     = 14, /* ESP32-C6 enable, HIGH = running */
    TDP4_XL_SD_PWR    = 15, /* SD card power, active LOW */
    TDP4_XL_RADIO_RST = 16,
    TDP4_XL_RADIO_IRQ = 17,
};

bool tdp4_xl_write(int pin, bool level);

/* ---- Display geometry and timing (RM69A10 AMOLED) ------------------------- */

#define TDP4_LCD_W 568
#define TDP4_LCD_H 1232

/* ---- Keyboard (tdp4_keyboard.c) ------------------------------------------- */

/* Start the attach/poll task. Key events are pushed to `q` as hal_key_t. */
void tdp4_kbd_start(i2c_master_bus_handle_t bus, QueueHandle_t q);
bool tdp4_kbd_present(void);
/* Key backlight 0..100 (0 = off); remembered across re-attach. */
void tdp4_kbd_backlight(uint8_t percent);

/* ---- Touch (tdp4_touch.c) ------------------------------------------------- */

/* Reset and probe the GT9895 on the main bus, then add an LVGL pointer
 * input for `disp`. Returns false if the controller does not answer. */
bool tdp4_touch_init(i2c_master_bus_handle_t bus, lv_display_t *disp);

#ifdef __cplusplus
}
#endif
