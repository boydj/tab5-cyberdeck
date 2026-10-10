#include "deck_shell.h"

#include "deck_boot.h"
#include "deck_fx.h"
#include "deck_hal.h"
#include "deck_launcher.h"
#include "deck_audio.h"
#include "deck_modal.h"
#include "deck_statusbar.h"
#include "deck_theme.h"
#include "deck_update.h"
#include "deck_widgets.h"

#define TRANSITION_MS 260

static lv_obj_t *s_root;
static lv_obj_t *s_content;
static deck_app_t *s_current;
static deck_app_t *s_next;   /* app to start at the midpoint of a transition */
static bool s_busy;          /* a transition is running */
static bool s_orient_hold;   /* a module's own dialog is open */

/* ---- Building blocks ----------------------------------------------------- */

static void build_root(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, g_pal.bg, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_root = deck_box(scr);
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);

    deck_statusbar_create(s_root);

    s_content = deck_box(s_root);
    lv_obj_set_width(s_content, LV_PCT(100));
    lv_obj_set_flex_grow(s_content, 1);
}

/* Delete whatever is on screen, then stop the app. The order matters:
 * removing focused widgets fires DEFOCUSED events into the app's handlers,
 * which must still find the app's state intact. */
static void teardown(void)
{
    deck_app_t *app = s_current;
    s_current       = NULL;
    s_orient_hold   = false;
    if (app && app->on_exit) app->on_exit(app);
    deck_modal_discard();
    lv_group_remove_all_objs(deck_input_group());
    lv_obj_clean(s_content);
    deck_input_reset();
    if (app && app->on_stop) app->on_stop(app);
}

static void show_home_now(void)
{
    teardown();
    deck_launcher_create(s_content);
    deck_statusbar_set_app(NULL);
}

static void start_app_now(deck_app_t *app)
{
    teardown();
    if (!app->on_start(app, s_content)) {
        LV_LOG_ERROR("module %s failed to start", app->name);
        lv_group_remove_all_objs(deck_input_group());
        lv_obj_clean(s_content);
        deck_launcher_create(s_content);
        deck_statusbar_set_app(NULL);
        return;
    }
    s_current = app;
    deck_statusbar_set_app(app);
}

/* ---- Transitions --------------------------------------------------------- */

static void transition_mid(void *u)
{
    (void)u;
    if (s_next) {
        start_app_now(s_next);
    } else {
        show_home_now();
    }
}

static void transition_done(void *u)
{
    (void)u;
    s_busy = false;
}

void deck_shell_launch(deck_app_t *app)
{
    if (app == NULL || s_busy || app == s_current) return;
    s_busy = true;
    s_next = app;
    deck_fx_glitch(TRANSITION_MS, transition_mid, transition_done, NULL);
}

void deck_shell_home(void)
{
    if (s_busy || s_current == NULL) return;
    s_busy = true;
    s_next = NULL;
    deck_fx_glitch(TRANSITION_MS, transition_mid, transition_done, NULL);
}

deck_app_t *deck_shell_current(void) { return s_current; }

/* Deferred: rebuild is usually requested from an event handler of a widget
 * that the rebuild deletes. */
/* ---- Sleep and keyboard light -------------------------------------------- */

static lv_obj_t *s_sleep;

static void wake_async(void *u)
{
    (void)u;
    if (s_sleep) {
        lv_obj_delete(s_sleep);
        s_sleep = NULL;
    }
    hal_display_power(true);
}

static void sleep_pressed(lv_event_t *e)
{
    (void)e;
    lv_async_call(wake_async, NULL); /* not from inside the overlay's own event */
}

void deck_shell_sleep(void)
{
    if (s_sleep) return;
    /* A black, clickable layer above everything (modals included) so the
     * waking touch lands here instead of on a control. */
    s_sleep = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_sleep);
    lv_obj_set_size(s_sleep, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_sleep, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_sleep, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_sleep, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_sleep, sleep_pressed, LV_EVENT_PRESSED, NULL);
    hal_display_power(false);
}

bool deck_shell_sleeping(void) { return s_sleep != NULL; }

static const uint8_t s_kbd_levels[3] = {0, 3, 20}; /* OFF, LOW, HIGH (20 = keyboard default) */

void deck_shell_apply_kbd_light(void)
{
    int level = (int)hal_cfg_get_i32("kbd_led", 1);
    if (level < 0 || level > 2) level = 1;
    bool theme   = hal_cfg_get_i32("kbd_theme", 0) != 0;
    uint32_t rgb = lv_color_to_u32(g_pal.accent) & 0xFFFFFF;
    hal_kbd_light(s_kbd_levels[level], theme, rgb);
}

/* ---- Shortcut list (Alt+H) ---------------------------------------------- */

void deck_shell_shortcuts(void)
{
    /* Home is a row of tiles in landscape, a column of rows in portrait
     * (where Up/Down move too). */
#define SHORTCUTS_ANYWHERE(home_keys)                     \
    "ANYWHERE\n"                                          \
    "  ALT+1..7    open a module\n"                       \
    "  ALT+ESC     back to the deck\n"                    \
    "  ALT+0       sleep the screen (or tap the clock)\n" \
    "  ALT+H       this list\n"                           \
    "  ALT+P       pause / resume music\n"                \
    "  ESC         back / close\n"                        \
    "\n"                                                  \
    "HOME\n"                                              \
    "  " home_keys ", or 1..7   pick a module\n"          \
    "\n"                                                  \
    "TERMINAL\n"                                          \
    "  every key goes to the server, ESC too\n"           \
    "  ALT+ESC leaves  SHIFT+UP/DOWN scrolls back"
    const char *cols[] = {
        HAL_PORTRAIT ? SHORTCUTS_ANYWHERE("ARROWS ENTER") : SHORTCUTS_ANYWHERE("LEFT RIGHT ENTER"),

        "NOTES\n"
        "  CTRL+S save  CTRL+P preview  CTRL+T split\n"
        "  ESC save and close\n"
        "\n"
        "CALC\n"
        "  ENTER evaluate  UP/DOWN recall\n"
        "  CTRL+D deg/rad  CTRL+K keypad  CTRL+L clear tape\n"
        "\n"
        "WEATHER\n"
        "  R refresh  L location  U units\n"
        "\n"
        "PLAYER\n"
        "  ENTER play  SPACE pause  LEFT/RIGHT seek\n"
        "  [ ] prev/next  - = volume  S shuffle  R repeat\n"
        "\n"
        "FILES\n"
        "  ENTER open  BACKSPACE up  DEL or D delete\n"
        "  P preview / portal  SPACE engage / disengage",
    };
#undef SHORTCUTS_ANYWHERE
    deck_modal_info("SHORTCUTS", cols, 2);
}

static void rebuild_now(void)
{
    deck_app_t *app = s_current;
    teardown();
    deck_statusbar_destroy();
    lv_obj_delete(s_root);
    deck_input_reset();
    deck_shell_apply_kbd_light(); /* accent may have changed */
    build_root();
    if (app) {
        start_app_now(app);
    } else {
        show_home_now();
    }
}

static void rebuild_async(void *u)
{
    (void)u;
    rebuild_now();
}

void deck_shell_rebuild(void) { lv_async_call(rebuild_async, NULL); }

/* ---- Orientation ----------------------------------------------------------- */

/* Turn the screen to `rot` and lay everything out again at the new size.
 * A module with on_relayout keeps running (the terminal keeps its SSH
 * session) and only the shell's own chrome is rebuilt around it; any other
 * module is restarted in place, like after an accent change. */
static void apply_rotation(hal_rot_t rot)
{
    hal_display_set_rotation(rot);
    deck_app_t *app = s_current;
    if (app && app->on_relayout) {
        deck_statusbar_destroy();
        deck_statusbar_create(s_root);
        lv_obj_move_to_index(lv_obj_get_child(s_root, -1), 0); /* back on top */
        deck_statusbar_set_app(app);
        app->on_relayout(app);
    } else {
        rebuild_now();
    }
}

/* Polled: follow the ROTATION setting, the keyboard and the motion sensor,
 * but never under a dialog (ours or a module's own), a transition, the boot
 * sequence, the update progress screen or a dark screen (it turns as soon
 * as those are gone). */
static void orient_poll(lv_timer_t *t)
{
    (void)t;
    hal_rot_t want = hal_orient_wanted();
    if (want == hal_display_rotation()) return;
    if (s_busy || s_sleep || s_orient_hold || deck_boot_active() || deck_modal_active() || deck_update_flashing()) {
        return;
    }
    apply_rotation(want);
}

static void orient_async(void *u)
{
    (void)u;
    orient_poll(NULL);
}

/* Deferred: usually called from a SYSTEM control that the rebuild deletes. */
void deck_shell_orient_now(void)
{
    if (hal_orient_supported()) lv_async_call(orient_async, NULL);
}

void deck_shell_orient_hold(bool hold) { s_orient_hold = hold; }

/* ---- Keys ---------------------------------------------------------------- */

bool deck_shell_key(const deck_key_t *k)
{
    if (s_sleep) {
        wake_async(NULL); /* the waking key does nothing else */
        return true;
    }
    if (deck_boot_active()) {
        deck_boot_skip();
        return true;
    }
    if (s_busy) return true; /* swallow keys mid-transition */

    if (deck_modal_active()) {
        if (k->code == HID_ESC) {
            deck_modal_cancel();
            return true;
        }
        if (k->code == HID_ENTER) {
            deck_modal_enter();
            return true;
        }
        return (k->mods & DECK_MOD_ALT) != 0; /* no module switching under a dialog */
    }

    if (k->mods & DECK_MOD_ALT) {
        if (k->code == HID_ESC) {
            deck_shell_home();
            return true;
        }
        if (k->code == HID_0) {
            deck_shell_sleep();
            return true;
        }
        if (k->code == HID_H) {
            deck_shell_shortcuts();
            return true;
        }
        if (k->code == HID_P) {
            player_toggle(); /* music pause / resume from anywhere */
            return true;
        }
        if (k->code >= HID_1 && k->code <= HID_9) {
            deck_app_t *app = deck_app_get((size_t)(k->code - HID_1));
            if (app) deck_shell_launch(app);
            return true;
        }
    }

    if (s_current == NULL) {
        return deck_launcher_key(k);
    }
    if (s_current->on_key && s_current->on_key(s_current, k)) {
        return true;
    }
    if (k->code == HID_ESC) {
        deck_shell_home();
        return true;
    }
    return false;
}

/* ---- Start --------------------------------------------------------------- */

void deck_shell_start(bool boot_anim)
{
    deck_grid_bg(lv_screen_active());
    build_root();
    show_home_now();
    deck_fx_scanlines(hal_cfg_get_i32("scan", 1) != 0);
    deck_shell_apply_kbd_light();
    if (hal_orient_supported()) {
        /* A fixed setting turns before the POST (which holds AUTO back).
         * The device HAL may already have turned the display in hal_init;
         * then the UI above was built at that size and needs no rebuild. */
        hal_rot_t want = hal_orient_wanted();
        if (hal_orient_mode() != HAL_ORIENT_AUTO && want != hal_display_rotation()) apply_rotation(want);
        lv_timer_create(orient_poll, 250, NULL);
    }
    if (boot_anim) deck_boot_run();
}
