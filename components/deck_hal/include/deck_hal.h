/*
 * deck_hal: the only layer that knows about real hardware.
 *
 * Everything above this (UI, launcher, apps) talks to the board through these
 * functions, so the same UI code runs on the Tab5 and in the desktop
 * simulator (sim/), which provides its own implementation of this header.
 *
 * Threading: hal_init() starts the LVGL task. Any LVGL call made from another
 * task must be wrapped in hal_lvgl_lock()/hal_lvgl_unlock(). LVGL callbacks
 * (events, timers, indev reads) already run with the lock held.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

/* Board selection. On the device it comes from Kconfig (DECK_BOARD choice);
 * the simulator passes -DDECK_BOARD_TDISPLAY_P4 to render the same layout. */
#if defined(CONFIG_DECK_BOARD_TDISPLAY_P4) && !defined(DECK_BOARD_TDISPLAY_P4)
#define DECK_BOARD_TDISPLAY_P4 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Lifecycle ----------------------------------------------------------- */

/* Bring up board, display, touch, LVGL, keyboard, RTC, power monitor, SD.
 * Returns false only if the display could not be started. Optional parts that
 * fail (no keyboard, no SD) are reported through the getters below. */
bool hal_init(void);

bool hal_lvgl_lock(uint32_t timeout_ms);   /* 0 = wait forever */
void hal_lvgl_unlock(void);

/* ---- Display ------------------------------------------------------------- */

#ifdef DECK_BOARD_TDISPLAY_P4
/* LilyGo T-Display P4, 4.1" AMOLED, native portrait. With the K270 keyboard
 * docked below the screen it runs portrait; undocked it can turn to
 * landscape (1232 x 568), see Orientation below. */
#define HAL_PANEL_W 568
#define HAL_PANEL_H 1232
#else
#define HAL_PANEL_W 1280
#define HAL_PANEL_H 720
#endif

/* The screen as the UI sees it right now, after any rotation. Before the
 * display exists this is the panel's own size. Layout code must call these
 * (or HAL_SCREEN_W/H, HAL_PORTRAIT) when it builds a screen, not cache them:
 * after a rotation the shell rebuilds the UI at the new size. */
int hal_screen_w(void);
int hal_screen_h(void);
#define HAL_SCREEN_W hal_screen_w()
#define HAL_SCREEN_H hal_screen_h()

/* True when the screen is taller than wide (layouts stack instead of row). */
#define HAL_PORTRAIT (hal_screen_h() > hal_screen_w())

void hal_backlight_set(uint8_t percent);   /* 3..100 (never fully dark) */

/* ---- Orientation --------------------------------------------------------- */

/* Screen rotation, numerically equal to lv_display_rotation_t. Named by
 * how the device is held: HAL_ROT_90 = turned clockwise onto its side (the
 * left edge of the portrait screen on top), HAL_ROT_270 = turned
 * counterclockwise (right edge on top). */
typedef enum {
    HAL_ROT_0   = 0,
    HAL_ROT_90  = 1,
    HAL_ROT_180 = 2,
    HAL_ROT_270 = 3,
} hal_rot_t;

/* The ROTATION setting (persisted). AUTO follows the motion sensor, but
 * stays portrait while the keyboard is docked. */
typedef enum {
    HAL_ORIENT_AUTO,
    HAL_ORIENT_PORTRAIT,
    HAL_ORIENT_LANDSCAPE,        /* HAL_ROT_90 */
    HAL_ORIENT_LANDSCAPE_FLIP,   /* HAL_ROT_270 */
    HAL_ORIENT_MODE_COUNT,
} hal_orient_mode_t;

/* False on boards that never rotate (Tab5): everything below is then inert
 * (rotation stays HAL_ROT_0). */
bool              hal_orient_supported(void);
hal_orient_mode_t hal_orient_mode(void);
void              hal_orient_set_mode(hal_orient_mode_t mode);

/* The rotation the screen should have now, from the setting, the keyboard
 * and the (debounced) motion sensor. Cheap; the shell polls it. */
hal_rot_t hal_orient_wanted(void);

/* Current rotation, and changing it. hal_display_set_rotation() must run on
 * the LVGL task (or with the LVGL lock held); it changes hal_screen_w/h and
 * leaves rebuilding the UI to the caller. */
hal_rot_t hal_display_rotation(void);
void      hal_display_set_rotation(hal_rot_t rot);

/* Motion sensor name ("ICM20948", "QMI8658", "SIM"), or NULL if none. */
const char *hal_orient_sensor(void);

/* Teach the sensor which way is up, in two steps while the user holds the
 * deck still: step 0 upright in portrait, step 1 turned clockwise onto its
 * side (landscape) at about the same lean. Returns false if the reading was
 * unusable (moving, lying flat, lean changed, or not a quarter turn); step 1
 * saves the result. Step -1 cancels after step 0. Blocks ~150 ms. */
bool hal_orient_calibrate(int step);

/* Screen sleep: off turns the backlight fully off and the keyboard LEDs
 * out; on restores the saved brightness and keyboard light. */
void hal_display_power(bool on);

/* ---- Keyboard ------------------------------------------------------------ */

/* One key transition from the Tab5 Keyboard in HID mode.
 * `code` is a USB HID usage ID (0x04 = 'a', 0x28 = Enter, ...).
 * `mods` is the HID modifier byte (bit0 LCtrl, bit1 LShift, bit2 LAlt, ...).
 * The keyboard reports releases with code 0, so `code` is 0 when !pressed. */
typedef struct {
    uint8_t code;
    uint8_t mods;
    bool    pressed;
} hal_key_t;

/* Pop one pending key event. Non-blocking; returns false when empty. */
bool hal_key_poll(hal_key_t *out);

bool    hal_kbd_present(void);
uint8_t hal_kbd_fw_version(void);          /* 0 if unknown */

/* Audio board control. The speaker amplifier is off unless audio is
 * playing through the speaker; headphones are detected on the jack. */
void hal_speaker_amp(bool on);
bool hal_headphones(void);
/* Tab5 only: the system I2C bus (i2c_master_bus_handle_t) for the codec. */
void *hal_tab5_sys_i2c(void);
/* The I2C bus (i2c_master_bus_handle_t) the audio codec sits on, any board. */
void *hal_codec_i2c(void);

/* Keyboard LEDs. brightness 0..100 (0 = off). With use_color the LEDs show
 * `rgb` (0xRRGGBB); otherwise the keyboard's own status colors (Caps/Sym).
 * Remembered and re-applied whenever the keyboard is (re)attached. */
void hal_kbd_light(uint8_t brightness, bool use_color, uint32_t rgb);

/* ---- Radio --------------------------------------------------------------- */

/* Prepare the Wi-Fi co-processor link. deck_net calls this once, from its own
 * task, before the first esp_wifi_init(). Returns false if the radio cannot
 * be brought up (Wi-Fi then stays off). */
bool hal_radio_init(void);

/* Radio repair (T-Display P4): its ESP32-C6 can ship with Espressif's AT
 * firmware instead of the network adapter DECK//OS needs. While AT is still
 * on it, the C6 can download and install the adapter over its own Wi-Fi.
 * hal_radio_repair_request() saves what that needs and restarts the deck;
 * the next boot runs the repair instead of Wi-Fi, and restarts again when it
 * has worked. One attempt per request. Unsupported boards: no-ops. */
bool        hal_radio_repair_supported(void);
const char *hal_radio_repair_default_url(void);
void        hal_radio_repair_request(const char *ssid, const char *pass, const char *url);
/* Status line of the repair on this boot; false if none ran. */
bool        hal_radio_repair_status(char *out, size_t n);

/* ---- Power --------------------------------------------------------------- */

typedef struct {
    bool  valid;       /* false if the power monitor could not be read */
    float volts;       /* pack voltage */
    float amps;        /* + charging, - discharging */
    int   percent;     /* 0..100 estimate, -1 if no battery */
    bool  charging;
} hal_power_t;

void hal_power_read(hal_power_t *out);

/* ---- Clock --------------------------------------------------------------- */

/* System time is seeded from the RTC at boot; this writes it back to both.
 * The RTC holds local wall time for the selected timezone. */
bool hal_rtc_set(const struct tm *local);
bool hal_rtc_present(void);

/* Timezones: a fixed list of POSIX TZ rules. The choice is persisted and
 * applied at boot before the RTC is read. Changing it keeps the absolute
 * time and rewrites the RTC in the new local time. */
int         hal_tz_count(void);
const char *hal_tz_name(int index);
int         hal_tz_get(void);
void        hal_tz_set(int index);

/* ---- Storage ------------------------------------------------------------- */

#define HAL_SD_MOUNT "/sdcard"

/* Where user files live: the SD card if one is mounted, otherwise a FAT
 * partition in internal flash (about 3.8 MB). NULL if neither is usable.
 * Paths are POSIX; use stdio/dirent on them directly. */
const char *hal_storage_root(void);
bool        hal_storage_is_sd(void);

bool     hal_sd_mounted(void);
uint64_t hal_sd_total_bytes(void);
uint64_t hal_sd_free_bytes(void);

/* ---- Settings (persisted key/value, NVS on device) ---------------------- */

int32_t hal_cfg_get_i32(const char *key, int32_t def);
void    hal_cfg_set_i32(const char *key, int32_t value);
/* Copies into out (always NUL terminated); returns false if not set. */
bool    hal_cfg_get_str(const char *key, char *out, size_t n);
void    hal_cfg_set_str(const char *key, const char *value);   /* NULL erases */
/* Binary values (the SSH device key). With out == NULL, *len gets the size. */
bool    hal_cfg_get_blob(const char *key, void *out, size_t *len);
void    hal_cfg_set_blob(const char *key, const void *data, size_t len); /* NULL erases */
/* True when settings are stored encrypted (eFuse HMAC key present). */
bool    hal_cfg_encrypted(void);

/* ---- System info --------------------------------------------------------- */

typedef struct {
    const char *board;          /* e.g. "TAB5 / ST7123" */
    const char *chip;           /* e.g. "ESP32-P4 rev1.0" */
    uint32_t    cpu_mhz;
    size_t      heap_int_free;
    size_t      heap_int_total;
    size_t      psram_free;
    size_t      psram_total;
    uint32_t    uptime_s;
} hal_sysinfo_t;

void hal_sysinfo(hal_sysinfo_t *out);

#ifdef __cplusplus
}
#endif
