/*
 * SYSTEM module, three columns (stacked in one scrolling column on a
 * portrait screen; on a short landscape screen the columns scroll):
 *   DISPLAY   brightness, scanlines, boot POST, accent; ROTATION on boards
 *             that turn (in the middle column on a short screen)
 *   NETWORK   Wi-Fi status and setup, timezone, clock (manual or NTP)
 *   SYSTEM    live hardware readout and firmware updates
 */
#include "app_sys.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "deck_fx.h"
#include "deck_hal.h"
#include "deck_icons.h"
#include "deck_layout.h"
#include "deck_modal.h"
#include "deck_update.h"
#include "deck_net.h"
#include "deck_ota.h"
#include "deck_shell.h"
#include "deck_theme.h"
#include "deck_widgets.h"

typedef struct {
    lv_obj_t *bright_val;
    lv_obj_t *clock_val;
    lv_obj_t *info[10];
    lv_obj_t *modal;
    lv_obj_t *roll[5];
    lv_timer_t *timer;

    lv_obj_t *net_state;
    lv_obj_t *net_ssid;
    lv_obj_t *net_ip;
    lv_obj_t *net_signal;
    lv_obj_t *join_btn;
    lv_obj_t *repair_btn;    /* boards that can repair their radio; shown while it is missing */
    lv_obj_t *repair_status;
    lv_obj_t *tz_val;
    lv_obj_t *ntp;
    lv_timer_t *scan_poll;
    lv_obj_t *ota_status;
    lv_obj_t *ota_btn;
    lv_obj_t *orient_btn;
    bool ota_want_install; /* a CHECK the user pressed: offer the install as soon as it finds one */
} sys_ui_t;

static sys_ui_t s_ui;

enum { INFO_BOARD, INFO_CHIP, INFO_PSRAM, INFO_SRAM, INFO_UPTIME, INFO_KBD, INFO_SD, INFO_PWR, INFO_FW, INFO_SEC, INFO_COUNT };

static const char *s_info_keys[INFO_COUNT] = {
    "BOARD", "CHIP", "PSRAM", "SRAM", "UPTIME", "KEYBOARD", "STORAGE", "POWER", "FIRMWARE", "SECURITY",
};

/* ---- Layout helpers ------------------------------------------------------ */

static lv_obj_t *column_panel(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *p = deck_panel(parent, DECK_CUT_TL | DECK_CUT_BR, 22);
    /* Portrait: full width, as tall as its rows; the page scrolls. */
    lv_obj_set_size(p, DECK_LP(w, LV_PCT(100)), DECK_LSP(LV_PCT(100), LV_SIZE_CONTENT, LV_SIZE_CONTENT));
    /* Short landscape: at least the screen's height, taller if the rows
     * need it; the columns then scroll together. */
    if (DECK_SHORT) lv_obj_set_style_min_height(p, LV_PCT(100), 0);
    deck_panel_set_tab(p, true);
    lv_obj_set_style_pad_all(p, DECK_LSP(26, 20, 22), 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p, 14, 0);
    return p;
}

/* A full-width row with a label on the left; returns the row so the caller
 * can add a control aligned right. */
static lv_obj_t *setting_row(lv_obj_t *parent, const char *icon, const char *name)
{
    lv_obj_t *row = deck_box(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 46);
    lv_obj_t *l = deck_icon_text(row, icon, name, g_font.mono_m, g_pal.text);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    return row;
}

/* ---- Display settings ---------------------------------------------------- */

static void bright_changed(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target_obj(e);
    int32_t v   = lv_slider_get_value(s);
    if (s_ui.bright_val == NULL) return;
    hal_backlight_set((uint8_t)v);
    lv_label_set_text_fmt(s_ui.bright_val, "%ld%%", (long)v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED || lv_event_get_code(e) == LV_EVENT_DEFOCUSED) {
        hal_cfg_set_i32("bright", v);
    }
}

static void accent_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx == deck_theme_accent()) return;
    hal_cfg_set_i32("accent", idx);
    deck_theme_set_accent(idx);
    deck_shell_rebuild();
}

static void scan_changed(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    hal_cfg_set_i32("scan", on);
    deck_fx_scanlines(on);
}

static void boot_changed(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    hal_cfg_set_i32("boot", on);
}

static const char *const s_kbd_names[3] = {"OFF", "LOW", "HIGH"};

static void kbd_led_clicked(lv_event_t *e)
{
    int level = ((int)hal_cfg_get_i32("kbd_led", 1) + 1) % 3;
    hal_cfg_set_i32("kbd_led", level);
    lv_obj_t *b = lv_event_get_target_obj(e);
    lv_label_set_text(lv_obj_get_child_by_type(b, 0, &lv_label_class), s_kbd_names[level]);
    deck_shell_apply_kbd_light();
}

static void kbd_theme_changed(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    hal_cfg_set_i32("kbd_theme", on);
    deck_shell_apply_kbd_light();
}

static void sleep_clicked(lv_event_t *e)
{
    (void)e;
    deck_shell_sleep();
}

static lv_obj_t *add_switch(lv_obj_t *row, bool on, lv_event_cb_t cb)
{
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 76, 38);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

/* ---- Rotation (boards that turn) ----------------------------------------- */

static const char *const s_orient_names[HAL_ORIENT_MODE_COUNT] = {"AUTO", "PORTRAIT", "LANDSCAPE",
                                                                  "LANDSCAPE FLIPPED"};
static bool s_focus_orient; /* MODE was just used: keep the focus on it across the rebuild */

static const char *rotation_name(hal_rot_t r)
{
    switch (r) {
        case HAL_ROT_90: return "LANDSCAPE";
        case HAL_ROT_180: return "PORTRAIT FLIPPED";
        case HAL_ROT_270: return "LANDSCAPE FLIPPED";
        default: return "PORTRAIT";
    }
}

/* Cycle AUTO -> PORTRAIT -> LANDSCAPE -> LANDSCAPE FLIPPED. If that changes
 * the wanted rotation, the shell turns the screen and rebuilds SYSTEM. */
static void orient_clicked(lv_event_t *e)
{
    hal_orient_mode_t m = (hal_orient_mode_t)(((int)hal_orient_mode() + 1) % HAL_ORIENT_MODE_COUNT);
    hal_orient_set_mode(m);
    lv_obj_t *b = lv_event_get_target_obj(e);
    lv_label_set_text(lv_obj_get_child_by_type(b, 0, &lv_label_class), s_orient_names[m]);
    /* Only when the screen is about to turn and SYSTEM gets rebuilt. */
    s_focus_orient = hal_orient_wanted() != hal_display_rotation();
    deck_shell_orient_now();
}

/* CALIBRATE: two still poses, one dialog each. The shell does not turn the
 * screen while a dialog is open, so they stay put while the deck turns. */
static void calib_show(int step, const char *problem);

static void calib_answer(bool yes, void *ud)
{
    int step = (int)(intptr_t)ud;
    if (!yes) { /* CANCEL or Esc: nothing is saved */
        hal_orient_calibrate(-1);
        return;
    }
    if (!hal_orient_calibrate(step)) {
        calib_show(step, step == 0 ? "No steady reading. The deck was moving or lying flat."
                                   : "That reading did not work: the deck was moving, lying flat, leaning a lot "
                                     "more or less than in step 1, or not turned a quarter turn.");
        return;
    }
    if (step == 0) {
        calib_show(1, NULL);
        return;
    }
    deck_modal_confirm("CALIBRATED", "Saved. AUTO rotation follows this from now on (keyboard undocked).", "OK", NULL,
                       NULL);
}

static void calib_show(int step, const char *problem)
{
    static const char *const how[2] = {
        "Hold the deck upright in portrait, screen facing you, and keep it still. Then press NEXT.",
        "Now turn it clockwise onto its side, so the portrait screen's LEFT edge ends up on top. Keep the same "
        "lean as before and keep it still, then press DONE.",
    };
    char msg[320];
    snprintf(msg, sizeof(msg), "%s%s%s", problem ? problem : "", problem ? "\n\n" : "", how[step]);
    deck_modal_confirm(step == 0 ? "CALIBRATE 1/2" : "CALIBRATE 2/2", msg,
                       problem ? "RETRY" : step == 0 ? "NEXT" : "DONE", calib_answer, (void *)(intptr_t)step);
}

static void calib_clicked(lv_event_t *e)
{
    (void)e;
    calib_show(0, NULL);
}

static lv_obj_t *small_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud);

/* ROTATION: MODE (cycles), what the sensor is and where the screen is now,
 * CALIBRATE, and the docked-keyboard rule. */
static void build_rotation(lv_obj_t *col)
{
    lv_obj_t *rs = deck_section(col, "ROTATION");
    lv_obj_set_style_margin_top(rs, 4, 0);

    lv_obj_t *row = setting_row(col, ICON_REFRESH, "MODE");
    s_ui.orient_btn = small_button(row, s_orient_names[hal_orient_mode()], orient_clicked, NULL);
    lv_obj_set_style_pad_hor(s_ui.orient_btn, 16, 0); /* LANDSCAPE FLIPPED still fits beside MODE */
    lv_obj_align(s_ui.orient_btn, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *cal = deck_box(col);
    lv_obj_set_width(cal, LV_PCT(100));
    const char *sensor = hal_orient_sensor();
    lv_obj_t *st       = deck_label(cal, g_font.mono_s, g_pal.dim, "");
    lv_label_set_text_fmt(st, "SENSOR %s\nNOW %s", sensor ? sensor : "NONE", rotation_name(hal_display_rotation()));
    lv_obj_set_style_text_line_space(st, 4, 0);
    lv_obj_align(st, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *cb = small_button(cal, "CALIBRATE", calib_clicked, NULL);
    lv_obj_align(cb, LV_ALIGN_RIGHT_MID, 0, 0);
    if (sensor == NULL) {
        lv_label_set_text_fmt(st, "NO SENSOR\nNOW %s", rotation_name(hal_display_rotation()));
        lv_obj_add_state(cb, LV_STATE_DISABLED);
        lv_group_remove_obj(cb);
        lv_obj_remove_flag(cb, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *note = deck_label(col, g_font.mono_s, g_pal.dim,
                                "AUTO follows the sensor while the keyboard is undocked. Docked, it stays portrait.");
    lv_obj_set_width(note, LV_PCT(100));
    lv_obj_set_style_text_line_space(note, 4, 0);
}

/* ---- Clock modal --------------------------------------------------------- */

/* The modal took over the focus group, so closing it rebuilds the module,
 * which restores the normal focus order (and deletes the modal). */
static void modal_close(void)
{
    if (s_ui.modal) {
        s_ui.modal = NULL;
        deck_shell_orient_hold(false);
        deck_shell_rebuild();
    }
}

static void modal_cancel(lv_event_t *e)
{
    (void)e;
    modal_close();
}

static void modal_set(lv_event_t *e)
{
    (void)e;
    struct tm t = {0};
    t.tm_year   = 2024 + (int)lv_roller_get_selected(s_ui.roll[0]) - 1900;
    t.tm_mon    = (int)lv_roller_get_selected(s_ui.roll[1]);
    t.tm_mday   = 1 + (int)lv_roller_get_selected(s_ui.roll[2]);
    t.tm_hour   = (int)lv_roller_get_selected(s_ui.roll[3]);
    t.tm_min    = (int)lv_roller_get_selected(s_ui.roll[4]);
    t.tm_isdst  = -1;
    hal_rtc_set(&t);
    modal_close();
}

static lv_obj_t *make_roller(lv_obj_t *parent, int first, int count, int width_digits, int selected,
                             const char *caption)
{
    static char opts[31 * 5 + 64 * 3];
    size_t n = 0;
    opts[0]  = '\0';
    for (int i = 0; i < count; i++) {
        n += (size_t)snprintf(opts + n, sizeof(opts) - n, "%s%0*d", i ? "\n" : "", width_digits, first + i);
    }

    lv_obj_t *col = deck_box(parent);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 8, 0);
    deck_label(col, g_font.mono_s, g_pal.dim, caption);

    lv_obj_t *r = lv_roller_create(col);
    lv_roller_set_options(r, opts, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(r, 3);
    lv_obj_set_width(r, width_digits >= 4 ? DECK_LP(130, 104) : DECK_LP(96, 76));
    lv_obj_set_style_text_font(r, g_font.mono_l, 0);
    lv_obj_set_style_bg_color(r, g_pal.panel, 0);
    lv_obj_set_style_border_color(r, g_pal.line, 0);
    lv_obj_set_style_text_color(r, g_pal.dim, 0);
    lv_obj_set_style_bg_color(r, g_pal.accent, LV_PART_SELECTED);
    lv_obj_set_style_text_color(r, g_pal.bg, LV_PART_SELECTED);
    if (selected >= 0 && selected < count) lv_roller_set_selected(r, (uint32_t)selected, LV_ANIM_OFF);
    return r;
}

static void open_clock_modal(lv_event_t *e)
{
    lv_obj_t *app_root = (lv_obj_t *)lv_event_get_user_data(e);
    if (s_ui.modal) return;

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    s_ui.modal = lv_obj_create(app_root);
    deck_shell_orient_hold(true); /* not a deck_modal: tell the shell not to turn under it */
    lv_obj_remove_style_all(s_ui.modal);
    lv_obj_add_flag(s_ui.modal, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_ui.modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_ui.modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ui.modal, LV_OPA_70, 0);

    lv_obj_t *p = deck_panel(s_ui.modal, DECK_CUT_TL | DECK_CUT_BR, 22);
    deck_panel_set_tab(p, true);
    deck_panel_set_outline(p, g_pal.accent);
    lv_obj_set_size(p, DECK_FIT_W(760, 16), LV_SIZE_CONTENT);
    lv_obj_center(p);
    lv_obj_set_style_pad_all(p, DECK_LP(30, 20), 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p, 20, 0);

    deck_section(p, "SET CLOCK");

    /* Rebuild the focus order for the modal only. */
    lv_group_t *g = deck_input_group();
    lv_group_remove_all_objs(g);

    lv_obj_t *row = deck_box(p);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    int year     = tm.tm_year + 1900;
    s_ui.roll[0] = make_roller(row, 2024, 17, 4, year - 2024, "YEAR");
    s_ui.roll[1] = make_roller(row, 1, 12, 2, tm.tm_mon, "MON");
    s_ui.roll[2] = make_roller(row, 1, 31, 2, tm.tm_mday - 1, "DAY");
    s_ui.roll[3] = make_roller(row, 0, 24, 2, tm.tm_hour, "HOUR");
    s_ui.roll[4] = make_roller(row, 0, 60, 2, tm.tm_min, "MIN");

    lv_obj_t *btns = deck_box(p);
    lv_obj_set_width(btns, LV_PCT(100));
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btns, 16, 0);
    lv_obj_t *cancel = deck_button(btns, "CANCEL", NULL);
    lv_obj_add_event_cb(cancel, modal_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *set = deck_button(btns, "SET", NULL);
    lv_obj_add_event_cb(set, modal_set, LV_EVENT_CLICKED, NULL);

    lv_group_focus_obj(s_ui.roll[3]);
}

/* ---- Network ------------------------------------------------------------- */

#define SCAN_SHOW 16

static net_ap_t s_aps[SCAN_SHOW];
static int s_ap_count;
static char s_join_ssid[33];

static void password_entered(const char *text, void *ud)
{
    (void)ud;
    if (text) net_connect(s_join_ssid, text);
}

static void other_ssid_entered(const char *text, void *ud)
{
    (void)ud;
    if (text == NULL || text[0] == '\0') return;
    snprintf(s_join_ssid, sizeof(s_join_ssid), "%s", text);
    deck_modal_password(s_join_ssid, password_entered, NULL);
}

static void network_picked(int index, void *ud)
{
    (void)ud;
    if (index < 0) return;
    if (index == s_ap_count) { /* "Other network..." */
        deck_modal_prompt("HIDDEN NETWORK", "", other_ssid_entered, NULL);
        return;
    }
    snprintf(s_join_ssid, sizeof(s_join_ssid), "%s", s_aps[index].ssid);
    if (s_aps[index].secure) {
        deck_modal_password(s_join_ssid, password_entered, NULL);
    } else {
        net_connect(s_join_ssid, "");
    }
}

static void scan_poll(lv_timer_t *t)
{
    if (!net_scan_done()) return;
    lv_timer_delete(t);
    s_ui.scan_poll = NULL;
    if (s_ui.join_btn) lv_label_set_text(lv_obj_get_child(s_ui.join_btn, 0), "JOIN");

    s_ap_count = (int)net_scan_results(s_aps, SCAN_SHOW);
    static char rows[SCAN_SHOW + 1][64];
    const char *items[SCAN_SHOW + 1];
    for (int i = 0; i < s_ap_count; i++) {
        int bars = s_aps[i].rssi > -55 ? 4 : s_aps[i].rssi > -65 ? 3 : s_aps[i].rssi > -75 ? 2 : 1;
        /* Portrait rows are ~40 characters wide; very long names get cut. */
        snprintf(rows[i], sizeof(rows[i]), DECK_LP("%-32.32s %s %.*s", "%-26.26s %s %.*s"), s_aps[i].ssid,
                 s_aps[i].secure ? "LOCK" : "OPEN", bars, "||||");
        items[i] = rows[i];
    }
    snprintf(rows[s_ap_count], sizeof(rows[0]), "Other network...");
    items[s_ap_count] = rows[s_ap_count];
    deck_modal_list("JOIN NETWORK", items, s_ap_count + 1, network_picked, NULL);
}

static void join_clicked(lv_event_t *e)
{
    (void)e;
    if (s_ui.scan_poll) return;
    if (!net_scan_start()) {
        lv_label_set_text(lv_obj_get_child(s_ui.join_btn, 0), "NO RADIO");
        return;
    }
    lv_label_set_text(lv_obj_get_child(s_ui.join_btn, 0), "SCANNING");
    s_ui.scan_poll = lv_timer_create(scan_poll, 100, NULL);
}

static void forget_confirmed(bool yes, void *ud)
{
    (void)ud;
    if (yes) net_forget();
}

static void forget_clicked(lv_event_t *e)
{
    (void)e;
    if (net_ssid()[0] == '\0') return;
    char msg[96];
    snprintf(msg, sizeof(msg), "Forget %s? The deck will stop joining it.", net_ssid());
    deck_modal_confirm("FORGET NETWORK", msg, "FORGET", forget_confirmed, NULL);
}

/* REPAIR (T-Display P4): the C6 may still run Espressif's AT firmware. Ask
 * for a Wi-Fi network and the firmware URL, then the HAL restarts into the
 * repair. The network is also saved as the deck's, so it joins it once the
 * radio works. */
static char s_rep_ssid[33];
static char s_rep_pass[65];

static void repair_url_entered(const char *text, void *ud)
{
    (void)ud;
    if (text != NULL) {
        net_connect(s_rep_ssid, s_rep_pass); /* only saved: the radio is down */
        hal_radio_repair_request(s_rep_ssid, s_rep_pass, text[0] ? text : hal_radio_repair_default_url());
    }
    memset(s_rep_pass, 0, sizeof(s_rep_pass));
}

static void repair_pass_entered(const char *text, void *ud)
{
    (void)ud;
    if (text == NULL) return;
    snprintf(s_rep_pass, sizeof(s_rep_pass), "%s", text);
    /* Empty: LilyGo's release on GitHub. A short local URL is easier to type
     * than editing that one (http://<computer>:8000/<file>.bin). */
    deck_modal_prompt("FIRMWARE URL (EMPTY = LILYGO RELEASE)", "", repair_url_entered, NULL);
}

static void repair_ssid_entered(const char *text, void *ud)
{
    (void)ud;
    if (text == NULL || text[0] == '\0') return;
    snprintf(s_rep_ssid, sizeof(s_rep_ssid), "%s", text);
    deck_modal_password(s_rep_ssid, repair_pass_entered, NULL); /* empty for an open network */
}

static void repair_confirmed(bool yes, void *ud)
{
    (void)ud;
    if (yes) deck_modal_prompt("WI-FI NETWORK (2.4 GHZ)", net_ssid(), repair_ssid_entered, NULL);
}

static void repair_clicked(lv_event_t *e)
{
    (void)e;
    deck_modal_confirm("REPAIR RADIO",
                       "The Wi-Fi chip (ESP32-C6) is not answering. If it still runs Espressif's AT firmware, it "
                       "can download LilyGo's network adapter and install it itself, over a 2.4 GHz Wi-Fi network. "
                       "The deck restarts to do this and again when it is done (a few minutes). If it fails, "
                       "nothing changes.",
                       "CONTINUE", repair_confirmed, NULL);
}

static void tz_picked(int index, void *ud)
{
    (void)ud;
    if (index >= 0) hal_tz_set(index);
}

static void tz_clicked(lv_event_t *e)
{
    (void)e;
    static const char *names[32];
    int n = hal_tz_count();
    for (int i = 0; i < n && i < 32; i++) names[i] = hal_tz_name(i);
    deck_modal_list("TIMEZONE", names, n, tz_picked, NULL);
}

/* "KEY ........ value" row; returns the value label. */
static lv_obj_t *kv_row(lv_obj_t *parent, const char *key)
{
    lv_obj_t *r = deck_box(parent);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_t *k = deck_label(r, g_font.mono_m, g_pal.dim, key);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *v = deck_label(r, g_font.mono_m, g_pal.text, "");
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
    return v;
}

static void refresh_net(void)
{
    net_state_t st = net_state();
    lv_color_t c   = st == NET_CONNECTED ? g_pal.ok : st == NET_NO_RADIO ? g_pal.danger
                     : (st == NET_CONNECTING || st == NET_STARTING) ? g_pal.warn : g_pal.dim;
    lv_label_set_text(s_ui.net_state, net_state_name(st));
    lv_obj_set_style_text_color(s_ui.net_state, c, 0);
    lv_label_set_text(s_ui.net_ssid, net_ssid()[0] ? net_ssid() : "--");
    lv_label_set_text(s_ui.net_ip, net_ip()[0] ? net_ip() : "--");
    if (st == NET_CONNECTED) {
        lv_label_set_text_fmt(s_ui.net_signal, "%d dBm  %.*s", net_rssi(), net_bars(), "||||");
    } else {
        lv_label_set_text(s_ui.net_signal, "--");
    }
    if (s_ui.repair_btn) {
        char rs[112];
        bool ran    = hal_radio_repair_status(rs, sizeof(rs));
        bool failed = ran && strncmp(rs, "FAILED", 6) == 0;
        /* Offer it while the radio is missing and no repair is under way. */
        bool offer = st == NET_NO_RADIO && (!ran || failed);
        if (offer) lv_obj_remove_flag(s_ui.repair_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_ui.repair_btn, LV_OBJ_FLAG_HIDDEN);
        if (ran) {
            lv_label_set_text_fmt(s_ui.repair_status, "RADIO REPAIR: %s", rs);
            lv_obj_set_style_text_color(s_ui.repair_status, failed ? g_pal.danger : g_pal.warn, 0);
        } else if (offer) {
            lv_label_set_text(s_ui.repair_status, "The Wi-Fi chip did not answer. REPAIR can reinstall its firmware.");
            lv_obj_set_style_text_color(s_ui.repair_status, g_pal.dim, 0);
        }
        if (ran || offer) lv_obj_remove_flag(s_ui.repair_status, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_ui.repair_status, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(s_ui.tz_val, hal_tz_name(hal_tz_get()));
    lv_label_set_text(s_ui.ntp, net_time_synced() ? "NTP: SYNCED" : st == NET_CONNECTED ? "NTP: SYNCING"
                                                                                        : "NTP: WAITING FOR NETWORK");
    lv_obj_set_style_text_color(s_ui.ntp, net_time_synced() ? g_pal.ok : g_pal.dim, 0);
}

/* ---- Firmware updates ---------------------------------------------------- */

static void install_confirmed(bool yes, void *ud)
{
    (void)ud;
    if (yes) deck_update_install();
}

static void offer_install(void)
{
    char msg[400];
        const char *notes = ota_notes();
    snprintf(msg, sizeof(msg), "Install v%s? The deck reboots when done.\n\n%.300s%s", ota_latest(), notes,
             strlen(notes) > 300 ? "..." : "");
    deck_modal_confirm("UPDATE", msg, "INSTALL", install_confirmed, NULL);
}

static void start_check(void)
{
    ota_state_t st = ota_state();
    if (st == OTA_CHECKING || st == OTA_DOWNLOADING || st == OTA_REBOOTING) return;
    s_ui.ota_want_install = true;
    ota_check(hal_cfg_get_i32("ota_beta", 0) != 0);
}

static void ota_clicked(lv_event_t *e)
{
    (void)e;
    if (ota_state() == OTA_AVAILABLE) {
        offer_install();
    } else {
        start_check();
    }
}

static void beta_changed(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    hal_cfg_set_i32("ota_beta", on);
    start_check(); /* the channel changed, so what counts as latest did too */
}

static void refresh_ota(void)
{
    char buf[96];
    lv_color_t c = g_pal.dim;
    const char *btn = "CHECK";
    switch (ota_state()) {
        case OTA_IDLE:
            snprintf(buf, sizeof(buf), ota_pending_verify() ? "NEW BUILD: SELF-TEST RUNNING" : "RUNNING v%s",
                     ota_running());
            c = ota_pending_verify() ? g_pal.warn : g_pal.dim;
            break;
        case OTA_CHECKING:
            snprintf(buf, sizeof(buf), "CHECKING GITHUB...");
            c = g_pal.accent;
            break;
        case OTA_UP_TO_DATE:
            snprintf(buf, sizeof(buf), "UP TO DATE (v%s)", ota_running());
            c = g_pal.ok;
            break;
        case OTA_AVAILABLE:
            snprintf(buf, sizeof(buf), "v%s AVAILABLE", ota_latest());
            c   = g_pal.accent2;
            btn = "INSTALL";
            break;
        case OTA_DOWNLOADING:
            snprintf(buf, sizeof(buf), "DOWNLOADING %d%%", ota_progress());
            c = g_pal.accent;
            break;
        case OTA_REBOOTING: snprintf(buf, sizeof(buf), "REBOOTING..."); break;
        case OTA_ERROR:
            snprintf(buf, sizeof(buf), "%.80s", ota_error());
            c = g_pal.danger;
            break;
    }
    lv_label_set_text(s_ui.ota_status, buf);
    lv_obj_set_style_text_color(s_ui.ota_status, c, 0);
    lv_label_set_text(lv_obj_get_child_by_type(s_ui.ota_btn, 0, &lv_label_class), btn);

    /* One press: a check the user started goes straight to the install
     * dialog when it finds something. */
    ota_state_t st = ota_state();
    if (s_ui.ota_want_install && st != OTA_CHECKING) {
        s_ui.ota_want_install = false;
        if (st == OTA_AVAILABLE && !deck_modal_active()) offer_install();
    }
}

/* ---- Live info ----------------------------------------------------------- */

static void fmt_bytes(char *buf, size_t n, size_t used, size_t total)
{
    snprintf(buf, n, "%lu / %lu KB", (unsigned long)(used / 1024), (unsigned long)(total / 1024));
}

static void refresh(lv_timer_t *t)
{
    (void)t;
    hal_sysinfo_t si;
    hal_sysinfo(&si);
    char buf[64];

    lv_label_set_text(s_ui.info[INFO_BOARD], si.board);
    snprintf(buf, sizeof(buf), "%s @ %luMHz", si.chip, (unsigned long)si.cpu_mhz);
    lv_label_set_text(s_ui.info[INFO_CHIP], buf);
    fmt_bytes(buf, sizeof(buf), si.psram_total - si.psram_free, si.psram_total);
    lv_label_set_text(s_ui.info[INFO_PSRAM], buf);
    fmt_bytes(buf, sizeof(buf), si.heap_int_total - si.heap_int_free, si.heap_int_total);
    lv_label_set_text(s_ui.info[INFO_SRAM], buf);
    lv_label_set_text_fmt(s_ui.info[INFO_UPTIME], "%luh %02lum %02lus", (unsigned long)(si.uptime_s / 3600),
                          (unsigned long)(si.uptime_s / 60 % 60), (unsigned long)(si.uptime_s % 60));

    if (hal_kbd_present()) {
        lv_label_set_text_fmt(s_ui.info[INFO_KBD], "LINKED  FW 0x%02X", hal_kbd_fw_version());
    } else {
        lv_label_set_text(s_ui.info[INFO_KBD], "NOT DETECTED");
    }

    if (hal_sd_mounted()) {
        snprintf(buf, sizeof(buf), "SD %.1f / %.1f GB FREE", (double)hal_sd_free_bytes() / 1e9,
                 (double)hal_sd_total_bytes() / 1e9);
        lv_label_set_text(s_ui.info[INFO_SD], buf);
    } else {
        lv_label_set_text(s_ui.info[INFO_SD], "NO MEDIA");
    }

    hal_power_t p;
    hal_power_read(&p);
    if (!p.valid) {
        lv_label_set_text(s_ui.info[INFO_PWR], "NO SENSOR");
    } else if (p.percent < 0) {
        snprintf(buf, sizeof(buf), "USB  %.2fV", (double)p.volts);
        lv_label_set_text(s_ui.info[INFO_PWR], buf);
    } else {
        snprintf(buf, sizeof(buf), "%.2fV %+.0fmA %d%%%s", (double)p.volts, (double)(p.amps * 1000.0f), p.percent,
                 p.charging ? " CHG" : "");
        lv_label_set_text(s_ui.info[INFO_PWR], buf);
    }
    lv_label_set_text(s_ui.info[INFO_FW], "DECK//OS v" DECK_VERSION);
    /* Settings encryption is per device (eFuse key); update signing is built in. */
    lv_label_set_text(s_ui.info[INFO_SEC], hal_cfg_encrypted() ? "ENCRYPTED / SIGNED OTA" : "PLAIN / SIGNED OTA");

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    lv_label_set_text_fmt(s_ui.clock_val, "%04d-%02d-%02d  %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                          tm.tm_mday, tm.tm_hour, tm.tm_min);
    refresh_net();
    refresh_ota();
}

/* ---- Module -------------------------------------------------------------- */

static lv_obj_t *small_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = deck_button(parent, text, NULL);
    lv_obj_set_style_min_height(b, 46, 0);
    lv_obj_set_style_pad_ver(b, 8, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    (void)self;
    s_ui = (sys_ui_t){0};

    lv_obj_t *root = deck_box(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, DECK_FLOW_SPLIT);
    lv_obj_set_style_pad_all(root, DECK_LSP(24, 16, 16), 0);
    lv_obj_set_style_pad_column(root, 20, 0);
    if (HAL_PORTRAIT || DECK_SHORT) {
        /* Three panels do not fit one portrait (or short) screen: scroll
         * them, and let keyboard focus pull the focused control into view. */
        lv_obj_set_style_pad_row(root, 16, 0);
        lv_obj_add_flag(root, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(root, LV_DIR_VER);
    }
    /* ROTATION goes under DISPLAY, or on a short screen under TIME, where
     * the columns' heights even out. */
    bool turns = hal_orient_supported();

    /* ---- DISPLAY ---- */
    lv_obj_t *left = column_panel(root, DECK_LSP(372, 368, 0));
    lv_obj_set_style_pad_row(left, 9, 0); /* seven rows plus the accent grid */
    deck_section(left, "DISPLAY");

    lv_obj_t *row = setting_row(left, ICON_BRIGHTNESS, "BRIGHTNESS");
    s_ui.bright_val = deck_label(row, g_font.mono_m, g_pal.accent, "");
    lv_obj_align(s_ui.bright_val, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *slider = lv_slider_create(left);
    lv_obj_set_width(slider, LV_PCT(92));
    lv_obj_set_style_margin_left(slider, 10, 0);
    lv_obj_set_style_margin_bottom(slider, 8, 0);
    lv_slider_set_range(slider, 10, 100);
    int32_t bright = hal_cfg_get_i32("bright", 80);
    lv_slider_set_value(slider, bright, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_ui.bright_val, "%ld%%", (long)bright);
    lv_obj_add_event_cb(slider, bright_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, bright_changed, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(slider, bright_changed, LV_EVENT_DEFOCUSED, NULL);

    row = setting_row(left, ICON_MONITOR, "SCANLINES");
    add_switch(row, hal_cfg_get_i32("scan", 1) != 0, scan_changed);
    row = setting_row(left, ICON_CHIP, "BOOT POST");
    add_switch(row, hal_cfg_get_i32("boot", 1) != 0, boot_changed);

    row = setting_row(left, ICON_KEYBOARD, "KBD LIGHT");
    int kbd_level = (int)hal_cfg_get_i32("kbd_led", 1);
    if (kbd_level < 0 || kbd_level > 2) kbd_level = 1;
    lv_obj_t *kb = small_button(row, s_kbd_names[kbd_level], kbd_led_clicked, NULL);
    lv_obj_set_width(kb, 100);
    lv_obj_align(kb, LV_ALIGN_RIGHT_MID, 0, 0);
    row = setting_row(left, ICON_PALETTE, "KBD THEME COLOR");
    add_switch(row, hal_cfg_get_i32("kbd_theme", 0) != 0, kbd_theme_changed);
    /* Screen sleep is also Alt+0 or a tap on the clock; this makes it findable. */
    row = setting_row(left, WX_NIGHT, "SLEEP (ALT+0)");
    lv_obj_t *sl = small_button(row, "NOW", sleep_clicked, NULL);
    lv_obj_set_width(sl, 100);
    lv_obj_align(sl, LV_ALIGN_RIGHT_MID, 0, 0);
    if (turns && !DECK_SHORT) build_rotation(left);

    deck_section(left, "ACCENT");
    lv_obj_t *sw = deck_box(left);
    lv_obj_set_width(sw, LV_PCT(100));
    lv_obj_set_flex_flow(sw, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(sw, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(sw, 12, 0);
    for (int i = 0; i < DECK_ACCENT_COUNT; i++) {
        lv_obj_t *b = deck_panel(sw, DECK_CUT_BR, 12);
        /* Portrait: the full-width panel takes all four swatches in a row. */
        lv_obj_set_size(b, DECK_LP(150, 106), 58);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        if (HAL_PORTRAIT || DECK_SHORT) lv_obj_add_flag(b, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        deck_panel_set_outline(b, deck_theme_accent_color(i));
        if (i == deck_theme_accent()) {
            lv_obj_add_state(b, LV_STATE_CHECKED);
            deck_panel_set_fill(b, g_pal.panel_hi);
        }
        lv_obj_t *bar = lv_obj_create(b);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, DECK_LP(110, 78), 5);
        lv_obj_set_style_bg_color(bar, deck_theme_accent_color(i), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 12);
        lv_obj_t *l = deck_label(b, g_font.mono_s, deck_theme_accent_color(i), deck_theme_accent_name(i));
        lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -8);
        lv_obj_add_event_cb(b, accent_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_group_add_obj(deck_input_group(), b);
    }

    /* ---- NETWORK + TIME ---- */
    lv_obj_t *mid = column_panel(root, DECK_LSP(392, 384, 0)); /* SHORT: MODE + LANDSCAPE FLIPPED fit */
    lv_obj_set_style_pad_row(mid, 10, 0);
    deck_section(mid, "NETWORK");
    s_ui.net_state  = deck_label(mid, g_font.disp_m, g_pal.dim, "");
    s_ui.net_ssid   = kv_row(mid, "SSID");
    s_ui.net_ip     = kv_row(mid, "IP");
    s_ui.net_signal = kv_row(mid, "SIGNAL");
    lv_obj_t *nb = deck_box(mid);
    lv_obj_set_width(nb, LV_PCT(100));
    lv_obj_set_flex_flow(nb, LV_FLEX_FLOW_ROW_WRAP); /* REPAIR wraps on a narrow column */
    lv_obj_set_style_pad_column(nb, 12, 0);
    lv_obj_set_style_pad_row(nb, 10, 0);
    lv_obj_set_style_margin_top(nb, 4, 0);
    s_ui.join_btn = small_button(nb, "JOIN", join_clicked, NULL);
    small_button(nb, "FORGET", forget_clicked, NULL);
    if (hal_radio_repair_supported()) {
        s_ui.repair_btn = small_button(nb, "REPAIR", repair_clicked, NULL);
        lv_obj_add_flag(s_ui.repair_btn, LV_OBJ_FLAG_HIDDEN);
        s_ui.repair_status = deck_label(mid, g_font.mono_s, g_pal.dim, "");
        lv_obj_set_width(s_ui.repair_status, LV_PCT(100));
        lv_label_set_long_mode(s_ui.repair_status, LV_LABEL_LONG_WRAP);
        lv_obj_add_flag(s_ui.repair_status, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *ts = deck_section(mid, "TIME");
    lv_obj_set_style_margin_top(ts, 8, 0);
    lv_obj_t *tzr = deck_box(mid);
    lv_obj_set_width(tzr, LV_PCT(100));
    s_ui.tz_val = deck_label(tzr, g_font.mono_m, g_pal.text, "");
    lv_obj_align(s_ui.tz_val, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *zb = small_button(tzr, "ZONE", tz_clicked, NULL);
    lv_obj_align(zb, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *clk = deck_box(mid);
    lv_obj_set_width(clk, LV_PCT(100));
    s_ui.clock_val = deck_label(clk, g_font.mono_m, g_pal.text, "");
    lv_obj_align(s_ui.clock_val, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *set = small_button(clk, "SET", open_clock_modal, parent);
    lv_obj_align(set, LV_ALIGN_RIGHT_MID, 0, 0);
    s_ui.ntp = deck_label(mid, g_font.mono_s, g_pal.dim, "");
    if (turns && DECK_SHORT) build_rotation(mid);

    /* ---- SYSTEM ---- */
    lv_obj_t *right = column_panel(root, 0);
    if (!HAL_PORTRAIT) lv_obj_set_flex_grow(right, 1);
    lv_obj_set_style_pad_row(right, 8, 0);
    deck_section(right, "SYSTEM");
    for (int i = 0; i < INFO_COUNT; i++) s_ui.info[i] = kv_row(right, s_info_keys[i]);

    lv_obj_t *us = deck_section(right, "UPDATE");
    lv_obj_set_style_margin_top(us, 10, 0);
    s_ui.ota_status = deck_label(right, g_font.mono_m, g_pal.dim, "");
    lv_obj_t *urow  = deck_box(right);
    lv_obj_set_width(urow, LV_PCT(100));
    s_ui.ota_btn = small_button(urow, "CHECK", ota_clicked, NULL);
    lv_obj_align(s_ui.ota_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *bl = deck_label(urow, g_font.mono_s, g_pal.dim, "BETA");
    lv_obj_align(bl, LV_ALIGN_RIGHT_MID, -90, 0);
    add_switch(urow, hal_cfg_get_i32("ota_beta", 0) != 0, beta_changed);

    refresh(NULL);
    s_ui.timer = lv_timer_create(refresh, 1000, NULL);
    if (s_focus_orient && s_ui.orient_btn) {
        /* Back from turning the screen with MODE: stay on it. */
        lv_obj_update_layout(root);
        lv_group_focus_obj(s_ui.orient_btn);
        lv_obj_scroll_to_view_recursive(s_ui.orient_btn, LV_ANIM_OFF);
    } else {
        lv_group_focus_obj(slider);
    }
    s_focus_orient = false;
    return true;
}

static void stop(deck_app_t *self)
{
    (void)self;
    if (s_ui.timer) lv_timer_delete(s_ui.timer);
    if (s_ui.scan_poll) lv_timer_delete(s_ui.scan_poll);
    if (s_ui.modal) deck_shell_orient_hold(false);
    s_ui = (sys_ui_t){0};
}

static bool on_key(deck_app_t *self, const deck_key_t *k)
{
    (void)self;
    if (s_ui.modal && k->code == HID_ESC) {
        modal_close();
        return true;
    }
    return false;
}

static deck_app_t s_app = {
    .name     = "SYSTEM",
    .tagline  = "DECK CONFIG",
    .icon     = ICON_COG,
    .eta      = NULL,
    .on_start = start,
    .on_stop  = stop,
    .on_key   = on_key,
};

deck_app_t *app_sys(void) { return &s_app; }
