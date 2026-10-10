/*
 * K270 keyboard for the T-Display P4: a TCA8418 matrix scanner (10 x 7,
 * 68 keys) and an XL9555 expander (LEDs, scanner reset, RF power) on their
 * own I2C bus, plus an SY7200A backlight driver on GPIO 47.
 *
 * Key events are turned into what the rest of DECK//OS expects from the
 * Tab5 Keyboard in HID mode: a USB HID usage ID plus the HID modifier byte,
 * releases reported with code 0. The K270 has no number pad or punctuation
 * row; those characters sit on the Fn layer, printed on the keycaps, which
 * is LilyGo's map in lilygo_device_driver keyboard_expansion_config.h. Fn
 * also turns the arrows into Home/End/PgUp/PgDn and Backspace into Delete.
 *
 * The scanner's INT line is only used by LilyGo for presence detection; the
 * FIFO is polled every 10 ms here, like the Tab5 HAL polls its keyboard.
 */
#include "tdp4.h"
#include "deck_hal.h"

#include <string.h>

#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "kbd";

/* ---- Chips ---------------------------------------------------------------- */

/* XL9555 on the keyboard: IO0 RF power, IO3..5 LEDs (low = lit), IO6 TCA8418
 * reset (low = reset). */
#define KX_OUT0 0x02
#define KX_CFG0 0x06
#define KX_RF_EN  (1u << 0)
#define KX_LED1   (1u << 3)
#define KX_LED2   (1u << 4)
#define KX_LED3   (1u << 5)
#define KX_TCARST (1u << 6)

/* TCA8418 registers. */
#define TCA_CFG       0x01
#define TCA_INT_STAT  0x02
#define TCA_KEY_LCK_EC 0x03
#define TCA_KEY_EVENT_A 0x04
#define TCA_KP_GPIO1  0x1D
#define TCA_KP_GPIO2  0x1E
#define TCA_KP_GPIO3  0x1F
#define TCA_CFG_KE_IEN 0x01

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_kx, s_tca;
static QueueHandle_t s_q;
static volatile bool s_present;
static uint8_t s_kx_out;

static esp_err_t wr(i2c_master_dev_handle_t d, uint8_t reg, uint8_t v)
{
    uint8_t b[2] = {reg, v};
    return i2c_master_transmit(d, b, 2, 30);
}

static esp_err_t rd(i2c_master_dev_handle_t d, uint8_t reg, uint8_t *v)
{
    return i2c_master_transmit_receive(d, &reg, 1, v, 1, 30);
}

/* ---- Keymap --------------------------------------------------------------- */

typedef enum {
    K_NONE,
    K_CHAR, /* printable: base character, Fn character */
    K_ESC,
    K_BKSP,
    K_ENTER,
    K_TAB,
    K_UP,
    K_DOWN,
    K_LEFT,
    K_RIGHT,
    K_CAPS,
    K_SHIFT,
    K_CTRL,
    K_ALT,
    K_META,
    K_FN,
    K_REC,
    K_F, /* function key; `base` holds the number */
} kind_t;

typedef struct {
    uint8_t kind;
    char base;
    char fn;
} kmap_t;

/* Indexed by TCA8418 event number - 1 (row * 10 + column). */
static const kmap_t k_map[68] = {
    {K_F, 1, 0},     {K_F, 2, 0},     {K_F, 3, 0},     {K_F, 4, 0},     {K_F, 5, 0},
    {K_F, 6, 0},     {K_F, 7, 0},     {K_F, 8, 0},     {K_F, 9, 0},     {K_F, 10, 0},
    {K_ESC, 0, 0},   {K_ESC, 0, 0},   {K_CHAR, '1', '!'}, {K_CHAR, '2', '@'}, {K_CHAR, '3', '#'},
    {K_CHAR, '4', '$'}, {K_CHAR, '5', '%'}, {K_CHAR, '6', '^'}, {K_CHAR, '7', '&'}, {K_CHAR, '8', '*'},
    {K_CHAR, 'q', '\''}, {K_CHAR, 'w', '_'}, {K_CHAR, 'e', '-'}, {K_CHAR, 'r', '+'}, {K_CHAR, 't', '='},
    {K_CHAR, 'y', '\\'}, {K_CHAR, 'u', '|'}, {K_CHAR, 'i', ';'}, {K_CHAR, 'o', ':'}, {K_CHAR, 'p', '"'},
    {K_CAPS, 0, 0},  {K_CHAR, 'a', '~'}, {K_CHAR, 's', '['}, {K_CHAR, 'd', ']'}, {K_CHAR, 'f', '{'},
    {K_CHAR, 'g', '}'}, {K_CHAR, 'h', ','}, {K_CHAR, 'j', '`'}, {K_CHAR, 'k', '/'}, {K_CHAR, 'l', '?'},
    {K_ALT, 0, 0},   {K_CHAR, 'z', 0},  {K_CHAR, 'x', 0},  {K_CHAR, 'c', 0},  {K_CHAR, 'v', 0},
    {K_CHAR, 'b', '.'}, {K_CHAR, 'n', '<'}, {K_CHAR, 'm', '>'}, {K_CTRL, 0, 0},  {K_UP, 0, 0},
    {K_FN, 0, 0},    {K_META, 0, 0},  {K_SHIFT, 0, 0}, {K_TAB, 0, 0},   {K_CHAR, ' ', ' '},
    {K_CHAR, ' ', ' '}, {K_CHAR, ' ', ' '}, {K_FN, 0, 0},  {K_LEFT, 0, 0},  {K_DOWN, 0, 0},
    {K_F, 11, 0},    {K_CHAR, '9', '('}, {K_BKSP, 0, 0}, {K_ENTER, 0, 0}, {K_REC, 0, 0},
    {K_ENTER, 0, 0}, {K_CHAR, '0', ')'}, {K_RIGHT, 0, 0},
};

/* HID modifier bits (left-hand side). */
#define M_CTRL  0x01
#define M_SHIFT 0x02
#define M_ALT   0x04
#define M_GUI   0x08

/* US layout: character to HID usage, `*shift` set when Shift is needed. */
static uint8_t char_to_hid(char c, bool *shift)
{
    static const char plain[] = "1234567890\0\0\0\0 -=[]\\\0;'`,./";
    static const char shifted[] = "!@#$%^&*()\0\0\0\0 _+{}|\0:\"~<>?";
    *shift = false;
    if (c >= 'a' && c <= 'z') return (uint8_t)(0x04 + (c - 'a'));
    if (c >= 'A' && c <= 'Z') {
        *shift = true;
        return (uint8_t)(0x04 + (c - 'A'));
    }
    if (c == ' ') return 0x2C;
    for (int i = 0; i < (int)sizeof(plain) - 1; i++) {
        if (plain[i] && plain[i] == c) return (uint8_t)(0x1E + i);
        if (shifted[i] && shifted[i] == c) {
            *shift = true;
            return (uint8_t)(0x1E + i);
        }
    }
    return 0;
}

/* ---- Event handling ------------------------------------------------------- */

static bool s_shift, s_ctrl, s_alt, s_meta, s_caps;
static int s_fn; /* two Fn keys */

static uint8_t mods(void)
{
    return (uint8_t)((s_ctrl ? M_CTRL : 0) | (s_shift ? M_SHIFT : 0) | (s_alt ? M_ALT : 0) | (s_meta ? M_GUI : 0));
}

static void emit(uint8_t code, uint8_t m, bool pressed)
{
    hal_key_t k = {.code = pressed ? code : 0, .mods = m, .pressed = pressed};
    if (s_q && xQueueSend(s_q, &k, 0) != pdTRUE) ESP_LOGW(TAG, "key queue full, dropped 0x%02x", code);
}

static void set_led(uint8_t led, bool on)
{
    uint8_t v = on ? (uint8_t)(s_kx_out & ~led) : (uint8_t)(s_kx_out | led);
    if (wr(s_kx, KX_OUT0, v) == ESP_OK) s_kx_out = v;
}

static void handle_event(uint8_t ev)
{
    bool down   = (ev & 0x80) != 0;
    uint8_t num = ev & 0x7F;
    if (num == 0 || num > sizeof(k_map) / sizeof(k_map[0])) return;
    const kmap_t *k = &k_map[num - 1];
    bool fn         = s_fn > 0;

    switch (k->kind) {
        case K_SHIFT: s_shift = down; return;
        case K_CTRL: s_ctrl = down; return;
        case K_ALT: s_alt = down; return;
        case K_META: s_meta = down; return;
        case K_FN:
            s_fn += down ? 1 : -1;
            if (s_fn < 0) s_fn = 0;
            return;
        case K_CAPS:
            if (down) {
                s_caps = !s_caps;
                set_led(KX_LED1, s_caps);
            }
            return;
        default: break;
    }

    if (!down) {
        emit(0, mods(), false); /* releases carry code 0, like the Tab5 Keyboard */
        return;
    }

    uint8_t m    = mods();
    uint8_t code = 0;
    switch (k->kind) {
        case K_CHAR: {
            bool need_shift = false;
            if (fn && k->fn) {
                code = char_to_hid(k->fn, &need_shift);
                m    = (uint8_t)((m & ~M_SHIFT) | (need_shift ? M_SHIFT : 0));
            } else {
                code = char_to_hid(k->base, &need_shift);
                bool letter = k->base >= 'a' && k->base <= 'z';
                /* Caps Lock flips letters for typing, but not chords (Ctrl+C). */
                if (letter && s_caps && !(m & (M_CTRL | M_ALT))) m ^= M_SHIFT;
            }
            break;
        }
        case K_ESC: code = 0x29; break;
        case K_BKSP: code = fn ? 0x4C : 0x2A; break;   /* Fn: Delete */
        case K_ENTER: code = 0x28; break;
        case K_TAB: code = 0x2B; break;
        case K_UP: code = fn ? 0x4B : 0x52; break;     /* Fn: Page Up */
        case K_DOWN: code = fn ? 0x4E : 0x51; break;   /* Fn: Page Down */
        case K_LEFT: code = fn ? 0x4A : 0x50; break;   /* Fn: Home */
        case K_RIGHT: code = fn ? 0x4D : 0x4F; break;  /* Fn: End */
        case K_F: code = (uint8_t)(0x3A + k->base - 1); break; /* F1..F11 */
        case K_REC: code = 0x45; break;                /* F12 */
        default: break;
    }
    if (code) emit(code, m, true);
}

/* ---- Attach / poll -------------------------------------------------------- */

static bool attach(void)
{
    if (i2c_master_probe(s_bus, TDP4_ADDR_XL9555, 30) != ESP_OK ||
        i2c_master_probe(s_bus, TDP4_ADDR_TCA8418, 30) != ESP_OK) {
        return false;
    }
    if (s_kx == NULL) {
        i2c_device_config_t c = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = TDP4_ADDR_XL9555, .scl_speed_hz = 400000};
        if (i2c_master_bus_add_device(s_bus, &c, &s_kx) != ESP_OK) return false;
        c.device_address = TDP4_ADDR_TCA8418;
        if (i2c_master_bus_add_device(s_bus, &c, &s_tca) != ESP_OK) return false;
    }

    /* LEDs off (high), RF section unpowered, scanner held in reset; then
     * make those pins outputs (LilyGo's order: values before direction). */
    s_kx_out = (uint8_t)(0xFF & ~KX_RF_EN & ~KX_TCARST);
    if (wr(s_kx, KX_OUT0, s_kx_out) != ESP_OK) return false;
    uint8_t cfg = 0xFF & ~(KX_RF_EN | KX_LED1 | KX_LED2 | KX_LED3 | KX_TCARST);
    if (wr(s_kx, KX_CFG0, cfg) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(10));
    s_kx_out |= KX_TCARST; /* release the scanner */
    if (wr(s_kx, KX_OUT0, s_kx_out) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Scan rows 0..6 and columns 0..9, key-event interrupt on, then drain
     * anything left in the FIFO. */
    bool ok = wr(s_tca, TCA_KP_GPIO1, 0x7F) == ESP_OK && wr(s_tca, TCA_KP_GPIO2, 0xFF) == ESP_OK &&
              wr(s_tca, TCA_KP_GPIO3, 0x03) == ESP_OK && wr(s_tca, TCA_CFG, TCA_CFG_KE_IEN) == ESP_OK;
    if (!ok) return false;
    uint8_t ev = 0;
    for (int i = 0; i < 16 && rd(s_tca, TCA_KEY_EVENT_A, &ev) == ESP_OK && ev; i++) {
    }
    wr(s_tca, TCA_INT_STAT, 0x1F);

    s_shift = s_ctrl = s_alt = s_meta = false;
    s_fn                               = 0;
    set_led(KX_LED1, s_caps);
    ESP_LOGI(TAG, "K270 keyboard attached");
    return true;
}

static bool poll_once(void)
{
    uint8_t ec = 0;
    if (rd(s_tca, TCA_KEY_LCK_EC, &ec) != ESP_OK) return false;
    int n = ec & 0x0F;
    for (int i = 0; i < n; i++) {
        uint8_t ev = 0;
        if (rd(s_tca, TCA_KEY_EVENT_A, &ev) != ESP_OK) return false;
        if (ev) handle_event(ev);
    }
    if (n) wr(s_tca, TCA_INT_STAT, 0x01);
    return true;
}

static void kbd_task(void *arg)
{
    (void)arg;
    int misses = 0;
    for (;;) {
        if (!s_present) {
            if (attach()) {
                s_present = true;
                misses    = 0;
                tdp4_kbd_backlight(255); /* re-apply the remembered level */
            } else {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
        }
        if (poll_once()) {
            misses = 0;
        } else if (++misses >= 3) {
            ESP_LOGW(TAG, "keyboard detached");
            s_present = false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ---- Backlight ------------------------------------------------------------ */

#define BL_TIMER   LEDC_TIMER_0
#define BL_CHANNEL LEDC_CHANNEL_0
#define BL_RES     LEDC_TIMER_10_BIT
static bool s_bl_ready;
static uint8_t s_bl_level;

static void bl_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BL_RES,
        .timer_num       = BL_TIMER,
        .freq_hz         = 20000, /* SY7200A wants 20 kHz..1 MHz on EN */
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_channel_config_t c = {
        .gpio_num   = TDP4_KBD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = BL_CHANNEL,
        .timer_sel  = BL_TIMER,
        .duty       = 0,
    };
    s_bl_ready = ledc_timer_config(&t) == ESP_OK && ledc_channel_config(&c) == ESP_OK;
    if (!s_bl_ready) ESP_LOGW(TAG, "backlight PWM init failed");
}

/* 255 re-applies the last level (used after attach). */
void tdp4_kbd_backlight(uint8_t percent)
{
    if (percent != 255) s_bl_level = percent > 100 ? 100 : percent;
    if (!s_bl_ready) return;
    uint32_t level = s_present ? s_bl_level : 0;
    uint32_t duty  = (1023u * level * level) / 10000u; /* perceptual curve */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL);
}

bool tdp4_kbd_present(void) { return s_present; }

void tdp4_kbd_start(i2c_master_bus_handle_t bus, QueueHandle_t q)
{
    s_bus = bus;
    s_q   = q;
    bl_init();
    xTaskCreatePinnedToCore(kbd_task, "kbd", 4096, NULL, 4, NULL, 0);
}
