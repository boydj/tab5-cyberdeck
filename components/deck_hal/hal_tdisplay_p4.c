/*
 * deck_hal implementation for the LilyGo T-Display P4 v1.0 (4.1" AMOLED)
 * with the K270 keyboard.
 *
 * The AMOLED (RM69A10, 568x1232 over two MIPI-DSI lanes) is used in its
 * native portrait orientation with the keyboard docked below, so LVGL draws
 * straight into the DPI frame buffers: no rotation, no PPA.
 *
 * Bring-up follows LilyGo's lilygo_device_driver (v1): the XL9535 expander
 * gates the peripheral 3V3 rail, the panel, touch, SD and audio resets and
 * the ESP32-C6 enable, so it is configured first. On-chip LDO 3 (2.5 V)
 * feeds the DSI PHY and LDO 4 (3.3 V) the SD card and expansion-header IO
 * (which the keyboard uses).
 *
 * Settings, timezones, storage fallback and system info are shared with the
 * Tab5 build in hal_common.c.
 */
#include "deck_hal.h"
#include "deck_tz.h"
#include "hal_common.h"
#include "tdp4.h"

#include <string.h>
#include <sys/time.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/sdmmc_host.h"
#include "esp_hosted.h"
#include "esp_hosted_transport_config.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_lvgl_port.h"
#include "sdmmc_cmd.h"

static const char *TAG = "hal";

static i2c_master_bus_handle_t s_main_bus;
static i2c_master_bus_handle_t s_audio_bus;
static i2c_master_bus_handle_t s_kbd_bus;
static i2c_master_dev_handle_t s_xl;
static i2c_master_dev_handle_t s_bq;
static i2c_master_dev_handle_t s_rtc;

static QueueHandle_t s_key_q;
static bool s_rtc_ok;
static bool s_bq_ok;
static bool s_sd_ok;
static sdmmc_card_t *s_card;

#define I2C_TIMEOUT_MS 50

/* ---- I2C helpers ---------------------------------------------------------- */

static esp_err_t reg_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *out, size_t n)
{
    return i2c_master_transmit_receive(dev, &reg, 1, out, n, I2C_TIMEOUT_MS);
}

static esp_err_t add_dev(i2c_master_bus_handle_t bus, uint8_t addr, uint32_t hz, i2c_master_dev_handle_t *out)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = hz,
    };
    return i2c_master_bus_add_device(bus, &cfg, out);
}

static esp_err_t new_bus(int port, int sda, int scl, i2c_master_bus_handle_t *out)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port          = port,
        .sda_io_num        = sda,
        .scl_io_num        = scl,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
#if SOC_LP_I2C_SUPPORTED
    if (port == LP_I2C_NUM_0) {
        cfg.lp_source_clk = LP_I2C_SCLK_DEFAULT;
    } else {
        cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    }
#else
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
#endif
    return i2c_new_master_bus(&cfg, out);
}

/* ---- XL9535 --------------------------------------------------------------- */

/* PCA9535-compatible: 0/1 input, 2/3 output, 6/7 direction (1 = input). */
#define XL_IN0  0x00
#define XL_OUT0 0x02
#define XL_CFG0 0x06

static SemaphoreHandle_t s_xl_lock;
static uint8_t s_xl_out[2];

bool tdp4_xl_write(int pin, bool level)
{
    if (s_xl == NULL) return false;
    int port = pin >= 10 ? 1 : 0;
    uint8_t bit = (uint8_t)(1u << (pin % 10));
    xSemaphoreTake(s_xl_lock, portMAX_DELAY);
    uint8_t v = level ? (s_xl_out[port] | bit) : (s_xl_out[port] & ~bit);
    esp_err_t err = reg_write(s_xl, XL_OUT0 + port, v);
    if (err == ESP_OK) s_xl_out[port] = v;
    xSemaphoreGive(s_xl_lock);
    return err == ESP_OK;
}

/* Preload a safe output state, then switch directions, so no power, reset or
 * enable line glitches while the expander is configured (LilyGo's order). */
static bool xl9535_init(void)
{
    s_xl_lock = xSemaphoreCreateMutex();
    if (add_dev(s_main_bus, TDP4_ADDR_XL9535, 400000, &s_xl) != ESP_OK) return false;

    /* Port 0: 3V3 on (low), SKY13453 high, LCD/touch resets asserted (low),
     * Ethernet held in reset, audio off. Inputs: touch INT (4), IMU INT (7). */
    s_xl_out[0] = 0x92;
    const uint8_t cfg0 = 0x90;
    /* Port 1: USB PHY power on, GPS wake low (L76K asleep), C6 held off until
     * hal_radio_init(), SD power off (high), LoRa radio held in reset (both
     * unused, as LilyGo leaves them). Inputs: RTC INT, C6 wake, radio IRQ. */
    s_xl_out[1] = 0xAD;
    const uint8_t cfg1 = 0x8C;

    bool ok = reg_write(s_xl, XL_OUT0, s_xl_out[0]) == ESP_OK && reg_write(s_xl, XL_OUT0 + 1, s_xl_out[1]) == ESP_OK &&
              reg_write(s_xl, XL_CFG0, cfg0) == ESP_OK && reg_write(s_xl, XL_CFG0 + 1, cfg1) == ESP_OK;
    if (!ok) {
        ESP_LOGE(TAG, "XL9535 IO expander not responding");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10)); /* let the 3V3 rail settle */
    return true;
}

/* ---- Power rails ---------------------------------------------------------- */

static esp_ldo_channel_handle_t s_ldo_dsi, s_ldo_io;

static bool ldo_init(void)
{
    esp_ldo_channel_config_t dsi = {.chan_id = 3, .voltage_mv = 2500};
    esp_ldo_channel_config_t io  = {.chan_id = 4, .voltage_mv = 3300};
    if (esp_ldo_acquire_channel(&dsi, &s_ldo_dsi) != ESP_OK) {
        ESP_LOGE(TAG, "LDO3 (DSI PHY) not acquired");
        return false;
    }
    if (esp_ldo_acquire_channel(&io, &s_ldo_io) != ESP_OK) {
        ESP_LOGW(TAG, "LDO4 (SD / expansion IO) not acquired");
    }
    return true;
}

/* ---- Display -------------------------------------------------------------- */

static esp_lcd_panel_io_handle_t s_lcd_io;
static esp_lcd_panel_handle_t s_lcd;

/* RM69A10 video-mode init (from LilyGo's cpp_bus_driver rm69a10.h): unlock,
 * full-panel window, TE on, brightness 0, sleep out, display on. */
typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t data[4];
    uint16_t delay_ms;
} lcd_cmd_t;

static const lcd_cmd_t k_rm69a10_init[] = {
    {0xFE, 1, {0xFD}, 0},
    {0x80, 1, {0xFC}, 0},
    {0xFE, 1, {0x00}, 0},
    {0x2A, 4, {0x00, 0x00, 0x02, 0x37}, 0}, /* columns 0..567 */
    {0x2B, 4, {0x00, 0x00, 0x04, 0xCF}, 0}, /* rows 0..1231 */
    {0x31, 4, {0x00, 0x03, 0x02, 0x34}, 0},
    {0x30, 4, {0x00, 0x00, 0x04, 0xCF}, 0},
    {0x12, 1, {0x00}, 0},
    {0x35, 1, {0x00}, 0},  /* tearing effect line on */
    {0x51, 1, {0x00}, 0},  /* brightness 0 until hal_backlight_set */
    {0x11, 0, {0}, 120},   /* sleep out */
    {0x29, 0, {0}, 0},     /* display on */
};

static esp_err_t lcd_cmd(uint8_t cmd, const uint8_t *data, size_t len)
{
    return esp_lcd_panel_io_tx_param(s_lcd_io, cmd, data, len);
}

static bool panel_init(void)
{
    esp_lcd_dsi_bus_handle_t dsi = NULL;
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id             = 0,
        .num_data_lanes     = 2,
        .phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = 1000,
    };
    if (esp_lcd_new_dsi_bus(&bus_cfg, &dsi) != ESP_OK) {
        ESP_LOGE(TAG, "DSI bus init failed");
        return false;
    }
    esp_lcd_dbi_io_config_t io_cfg = {.virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8};
    if (esp_lcd_new_panel_io_dbi(dsi, &io_cfg, &s_lcd_io) != ESP_OK) {
        ESP_LOGE(TAG, "DSI command IO init failed");
        return false;
    }

    /* One frame buffer in PSRAM that the panel scans out; LVGL's finished
     * areas are copied (and rotated) into it by the PPA, see flush_cb. */
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel    = 0,
        .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 60,
        .pixel_format       = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .in_color_format    = LCD_COLOR_FMT_RGB565,
        .out_color_format   = LCD_COLOR_FMT_RGB565,
        .num_fbs            = 1,
        .video_timing =
            {
                .h_size            = TDP4_LCD_W,
                .v_size            = TDP4_LCD_H,
                .hsync_pulse_width = 50,
                .hsync_back_porch  = 150,
                .hsync_front_porch = 50,
                .vsync_pulse_width = 40,
                .vsync_back_porch  = 120,
                .vsync_front_porch = 80,
            },
    };
    if (esp_lcd_new_panel_dpi(dsi, &dpi_cfg, &s_lcd) != ESP_OK) {
        ESP_LOGE(TAG, "DPI panel init failed");
        return false;
    }

    /* Hardware reset through the expander: low 10 ms, then 120 ms to boot. */
    tdp4_xl_write(TDP4_XL_LCD_RST, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    tdp4_xl_write(TDP4_XL_LCD_RST, true);
    vTaskDelay(pdMS_TO_TICKS(120));

    uint8_t id = 0;
    if (esp_lcd_panel_io_rx_param(s_lcd_io, 0xA1, &id, 1) == ESP_OK) {
        ESP_LOGI(TAG, "RM69A10 id 0x%02x%s", id, id == 0x01 ? "" : " (expected 0x01)");
    } else {
        ESP_LOGW(TAG, "RM69A10 id read failed");
    }

    for (size_t i = 0; i < sizeof(k_rm69a10_init) / sizeof(k_rm69a10_init[0]); i++) {
        const lcd_cmd_t *c = &k_rm69a10_init[i];
        if (lcd_cmd(c->cmd, c->len ? c->data : NULL, c->len) != ESP_OK) {
            ESP_LOGE(TAG, "RM69A10 command 0x%02x failed", c->cmd);
            return false;
        }
        if (c->delay_ms) vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
    }
    if (esp_lcd_panel_init(s_lcd) != ESP_OK) { /* starts the video stream */
        ESP_LOGE(TAG, "DPI panel start failed");
        return false;
    }
    return true;
}

/* LVGL renders the UI in 1/8-screen strips (partial mode, two buffers in
 * PSRAM) at the logical size: 568 x 1232 in portrait, 1232 x 568 when turned.
 * Each finished strip goes to the panel's frame buffer through the PPA, which
 * copies, and for landscape also rotates, in one pass. Rotation is LVGL's own
 * (lv_display_set_rotation), so touch points are turned to match by LVGL. */
static lv_display_t *s_disp;
static ppa_client_handle_t s_ppa;
static void *s_fb;
static bool s_ppa_cw; /* the PPA turns "90 degrees" clockwise (probed; documented CCW) */

#define FB_BYTES    ((size_t)TDP4_LCD_W * TDP4_LCD_H * 2)
#define DRAW_BUF_PX ((size_t)TDP4_LCD_W * TDP4_LCD_H / 8)

/* Which way does the PPA turn a 90-degree rotation? Turn a 4 x 2 test
 * picture and see where a known pixel lands, rather than trust the docs. */
static void ppa_probe(void)
{
    const size_t sz = 128;
    uint16_t *in    = heap_caps_aligned_calloc(128, 1, sz, MALLOC_CAP_SPIRAM);
    uint16_t *out   = heap_caps_aligned_calloc(128, 1, sz, MALLOC_CAP_SPIRAM);
    if (in && out) {
        for (int i = 0; i < 8; i++) in[i] = (uint16_t)(1 + i); /* pixel (x, y) = 1 + x + 4y */
        ppa_srm_oper_config_t c = {
            .in      = {.buffer = in, .pic_w = 4, .pic_h = 2, .block_w = 4, .block_h = 2, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
            .out     = {.buffer = out, .buffer_size = sz, .pic_w = 2, .pic_h = 4, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,
            .scale_x = 1.0f,
            .scale_y = 1.0f,
            .mode    = PPA_TRANS_MODE_BLOCKING,
        };
        if (ppa_do_scale_rotate_mirror(s_ppa, &c) == ESP_OK) {
            esp_cache_msync(out, sz, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            /* counterclockwise: out(0,0) = in(3,0) = 4; clockwise: out(0,0) = in(0,1) = 5 */
            s_ppa_cw = out[0] == 5;
            ESP_LOGI(TAG, "PPA rotate-90 probe: %s (out[0]=%u)", out[0] == 4 ? "counterclockwise" : out[0] == 5 ? "clockwise" : "unexpected", out[0]);
        }
    }
    heap_caps_free(in);
    heap_caps_free(out);
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    int32_t w     = lv_area_get_width(area);
    int32_t h     = lv_area_get_height(area);
    lv_area_t dst = *area;
    lv_display_rotate_area(disp, &dst); /* logical strip -> its place on the panel */

    /* LVGL's ROTATION_90 shows the picture turned counterclockwise on the
     * panel (logical x runs up the panel); PPA_SRM_ROTATION_ANGLE_90 is
     * counterclockwise unless the probe said otherwise. */
    ppa_srm_rotation_angle_t ang = PPA_SRM_ROTATION_ANGLE_0;
    switch (lv_display_get_rotation(disp)) {
        case LV_DISPLAY_ROTATION_90: ang = s_ppa_cw ? PPA_SRM_ROTATION_ANGLE_270 : PPA_SRM_ROTATION_ANGLE_90; break;
        case LV_DISPLAY_ROTATION_180: ang = PPA_SRM_ROTATION_ANGLE_180; break;
        case LV_DISPLAY_ROTATION_270: ang = s_ppa_cw ? PPA_SRM_ROTATION_ANGLE_90 : PPA_SRM_ROTATION_ANGLE_270; break;
        default: break;
    }
    ppa_srm_oper_config_t c = {
        .in =
            {
                .buffer  = px,
                .pic_w   = (uint32_t)w,
                .pic_h   = (uint32_t)h,
                .block_w = (uint32_t)w,
                .block_h = (uint32_t)h,
                .srm_cm  = PPA_SRM_COLOR_MODE_RGB565,
            },
        .out =
            {
                .buffer         = s_fb,
                .buffer_size    = FB_BYTES,
                .pic_w          = TDP4_LCD_W,
                .pic_h          = TDP4_LCD_H,
                .block_offset_x = (uint32_t)dst.x1,
                .block_offset_y = (uint32_t)dst.y1,
                .srm_cm         = PPA_SRM_COLOR_MODE_RGB565,
            },
        .rotation_angle = ang,
        .scale_x        = 1.0f,
        .scale_y        = 1.0f,
        .mode           = PPA_TRANS_MODE_BLOCKING,
    };
    esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa, &c);
    if (err != ESP_OK) ESP_LOGE(TAG, "PPA copy %ldx%ld @%ld,%ld failed: %s", (long)w, (long)h, (long)dst.x1, (long)dst.y1, esp_err_to_name(err));
    lv_display_flush_ready(disp);
}

/* esp_lvgl_port's task sleeps until LVGL has work; tell it when a redraw is
 * requested (its own displays do this; ours is not one of them). */
static void disp_wake_cb(lv_event_t *e)
{
    (void)e;
    lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, NULL);
}

static bool display_init(void)
{
    if (!panel_init()) return false;
    if (esp_lcd_dpi_panel_get_frame_buffer(s_lcd, 1, &s_fb) != ESP_OK || s_fb == NULL) {
        ESP_LOGE(TAG, "no DPI frame buffer");
        return false;
    }
    ppa_client_config_t ppa_cfg = {.oper_type = PPA_OPERATION_SRM};
    if (ppa_register_client(&ppa_cfg, &s_ppa) != ESP_OK) {
        ESP_LOGE(TAG, "PPA client register failed");
        return false;
    }
    ppa_probe();

    lvgl_port_cfg_t lvgl_cfg   = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_priority     = 6;
    lvgl_cfg.task_stack        = 24 * 1024;
    lvgl_cfg.task_affinity     = 1;
    lvgl_cfg.task_max_sleep_ms = 500;
    lvgl_cfg.task_stack_caps   = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT;
    lvgl_cfg.timer_period_ms   = 5;
    if (lvgl_port_init(&lvgl_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed");
        return false;
    }

    size_t bytes = DRAW_BUF_PX * 2;
    void *buf1   = heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM);
    void *buf2   = heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM);
    if (buf1 == NULL || buf2 == NULL) {
        ESP_LOGE(TAG, "no memory for LVGL draw buffers");
        return false;
    }
    lvgl_port_lock(0);
    s_disp = lv_display_create(TDP4_LCD_W, TDP4_LCD_H);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, buf1, buf2, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_add_event_cb(s_disp, disp_wake_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(s_disp, disp_wake_cb, LV_EVENT_REFR_REQUEST, NULL);
    lvgl_port_unlock();

    if (!tdp4_touch_init(s_main_bus, s_disp)) {
        ESP_LOGW(TAG, "GT9895 touch not found (TFT board? only the AMOLED is supported)");
    }
    return true;
}

hal_rot_t hal_display_rotation(void) { return s_disp ? (hal_rot_t)lv_display_get_rotation(s_disp) : HAL_ROT_0; }

void hal_display_set_rotation(hal_rot_t rot)
{
    if (s_disp == NULL || rot == hal_display_rotation()) return;
    ESP_LOGI(TAG, "rotation %d -> %d", (int)hal_display_rotation() * 90, (int)rot * 90);
    lv_display_set_rotation(s_disp, (lv_display_rotation_t)rot);
    lv_obj_invalidate(lv_screen_active()); /* repaint everything at the new angle */
}

static uint8_t s_brightness = 80;
static bool s_display_on    = true;
static uint8_t s_kbd_light  = 20;

static void panel_brightness(uint8_t percent)
{
    uint8_t v = (uint8_t)(((uint32_t)percent * 255u + 50u) / 100u);
    lcd_cmd(0x51, &v, 1);
}

void hal_backlight_set(uint8_t percent)
{
    if (percent > 100) percent = 100;
    if (percent < 3) percent = 3; /* never fully dark: the user could not find the slider */
    s_brightness = percent;
    if (s_display_on && s_lcd_io) panel_brightness(percent);
}

/* AMOLED sleep: display off and sleep in; the DPI stream keeps running and
 * the panel ignores it. Waking needs 120 ms after sleep out (datasheet). */
static int64_t s_sleep_in_us; /* when sleep-in was sent */

void hal_display_power(bool on)
{
    if (on == s_display_on || s_lcd_io == NULL) return;
    s_display_on = on;
    if (on) {
        /* Sleep-out must wait 120 ms after sleep-in (RM69A10 timing). */
        int64_t since_ms = (esp_timer_get_time() - s_sleep_in_us) / 1000;
        if (since_ms < 120) vTaskDelay(pdMS_TO_TICKS(120 - since_ms));
        lcd_cmd(0x11, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(120));
        lcd_cmd(0x29, NULL, 0);
        panel_brightness(s_brightness);
        tdp4_kbd_backlight(s_kbd_light);
    } else {
        panel_brightness(0);
        lcd_cmd(0x28, NULL, 0);
        lcd_cmd(0x10, NULL, 0);
        s_sleep_in_us = esp_timer_get_time();
        tdp4_kbd_backlight(0);
    }
}

bool hal_lvgl_lock(uint32_t timeout_ms) { return lvgl_port_lock(timeout_ms); }
void hal_lvgl_unlock(void) { lvgl_port_unlock(); }

/* ---- Orientation ---------------------------------------------------------- */

static bool s_orient_ok; /* a motion sensor answered */

bool hal_orient_supported(void) { return true; }

/* The setting, read from NVS once: the shell asks four times a second. */
static int8_t s_orient_mode = -1;

hal_orient_mode_t hal_orient_mode(void)
{
    if (s_orient_mode < 0) {
        int32_t m     = hal_cfg_get_i32("orient", HAL_ORIENT_AUTO);
        s_orient_mode = (int8_t)((m >= 0 && m < HAL_ORIENT_MODE_COUNT) ? m : HAL_ORIENT_AUTO);
    }
    return (hal_orient_mode_t)s_orient_mode;
}

void hal_orient_set_mode(hal_orient_mode_t mode)
{
    s_orient_mode = (int8_t)mode;
    hal_cfg_set_i32("orient", (int32_t)mode);
}

/* Docked, the keyboard sits below the portrait screen, so AUTO only turns
 * the screen while it is off. */
hal_rot_t hal_orient_wanted(void)
{
    switch (hal_orient_mode()) {
        case HAL_ORIENT_PORTRAIT: return HAL_ROT_0;
        case HAL_ORIENT_LANDSCAPE: return HAL_ROT_90;
        case HAL_ORIENT_LANDSCAPE_FLIP: return HAL_ROT_270;
        default: break;
    }
    if (!s_orient_ok || tdp4_kbd_present()) return HAL_ROT_0;
    return tdp4_orient_sensed();
}

const char *hal_orient_sensor(void) { return tdp4_orient_sensor(); }
bool hal_orient_calibrate(int step) { return s_orient_ok && tdp4_orient_calibrate(step); }

/* ---- Keyboard ------------------------------------------------------------- */

bool hal_key_poll(hal_key_t *out) { return s_key_q && xQueueReceive(s_key_q, out, 0) == pdTRUE; }
bool hal_kbd_present(void) { return tdp4_kbd_present(); }
uint8_t hal_kbd_fw_version(void) { return 0; } /* the K270 has no firmware to ask */

/* The K270 has one white backlight; there are no RGB LEDs to colour. */
void hal_kbd_light(uint8_t brightness, bool use_color, uint32_t rgb)
{
    (void)use_color;
    (void)rgb;
    s_kbd_light = brightness > 100 ? 100 : brightness;
    if (s_display_on) tdp4_kbd_backlight(s_kbd_light);
}

/* ---- Audio board control -------------------------------------------------- */

/* ES8311 + NS4150B share one switched supply on XL9535 IO6. */
void hal_speaker_amp(bool on) { tdp4_xl_write(TDP4_XL_AUDIO_EN, on); }
bool hal_headphones(void) { return false; } /* no jack detect on this board */
void *hal_tab5_sys_i2c(void) { return (void *)s_audio_bus; }
void *hal_codec_i2c(void) { return (void *)s_audio_bus; }

/* ---- Radio (ESP32-C6 over SDIO, ESP-Hosted) -------------------------------- */

/* The C6 enable is on the expander, so ESP-Hosted toggles it through this
 * callback instead of a GPIO (LilyGo's esp-hosted-mcu fork). */
static esp_err_t c6_reset_cb(void *arg, bool level)
{
    (void)arg;
    return tdp4_xl_write(TDP4_XL_C6_EN, level) ? ESP_OK : ESP_FAIL;
}

/* LilyGo's ESP-Hosted network adapter for this board's C6 (LilygoBox
 * release), matching the host side DECK//OS is built with (2.12.3). */
#define C6_ADAPTER_URL                                                                                        \
    "https://github.com/Xinyuan-LilyGO/lilygobox-espidf/releases/download/v1.1.2/"                            \
    "lilygobox-t-display-p4-device-v1.0-esp32c6-rev0.0-v2.12.3.bin"

bool hal_radio_repair_supported(void) { return true; }
const char *hal_radio_repair_default_url(void) { return C6_ADAPTER_URL; }

void hal_radio_repair_request(const char *ssid, const char *pass, const char *url)
{
    hal_cfg_set_str("rfx_ssid", ssid);
    hal_cfg_set_str("rfx_pass", pass);
    hal_cfg_set_str("rfx_url", url && url[0] ? url : C6_ADAPTER_URL);
    hal_cfg_set_i32("rfx", 1);
    ESP_LOGW(TAG, "radio repair requested; restarting");
    vTaskDelay(pdMS_TO_TICKS(200)); /* let the log and NVS settle */
    esp_restart();
}

bool hal_radio_repair_status(char *out, size_t n) { return tdp4_c6_repair_status(out, n); }

/* A repair was requested: run it instead of ESP-Hosted, which would hold the
 * SDIO link. The request is cleared first, so a crash cannot loop. */
static bool radio_repair_pending(void)
{
    if (hal_cfg_get_i32("rfx", 0) == 0) return false;
    hal_cfg_set_i32("rfx", 0);
    char ssid[33] = "", pass[65] = "", url[256] = "";
    hal_cfg_get_str("rfx_ssid", ssid, sizeof(ssid));
    hal_cfg_get_str("rfx_pass", pass, sizeof(pass));
    hal_cfg_get_str("rfx_url", url, sizeof(url));
    hal_cfg_set_str("rfx_pass", NULL);
    tdp4_c6_repair_start(ssid, pass, url[0] ? url : C6_ADAPTER_URL);
    memset(pass, 0, sizeof(pass));
    return true;
}

bool hal_radio_init(void)
{
    static bool s_done, s_ok;
    if (s_done) return s_ok;
    s_done = true;
    if (s_xl == NULL) return false;
    if (radio_repair_pending()) return false; /* Wi-Fi stays off this boot */
    if (esp_hosted_sdio_set_reset_callback(c6_reset_cb, NULL) != ESP_TRANSPORT_OK) {
        ESP_LOGE(TAG, "ESP-Hosted reset callback not accepted");
        return false;
    }
    int err = esp_hosted_init();
    s_ok    = err == ESP_OK || err == ESP_ERR_INVALID_STATE;
    if (!s_ok) ESP_LOGE(TAG, "ESP-Hosted init failed (%d)", err);
    return s_ok;
}

/* ---- Power (BQ27220 fuel gauge) ------------------------------------------- */

static int16_t bq_read16(uint8_t cmd, bool *ok)
{
    uint8_t b[2] = {0};
    if (reg_read(s_bq, cmd, b, 2) != ESP_OK) {
        *ok = false;
        return 0;
    }
    return (int16_t)(b[0] | (b[1] << 8));
}

void hal_power_read(hal_power_t *out)
{
    memset(out, 0, sizeof(*out));
    out->percent = -1;
    if (!s_bq_ok) return;
    bool ok         = true;
    int16_t mv      = bq_read16(0x08, &ok); /* Voltage, mV */
    int16_t ma      = bq_read16(0x0C, &ok); /* Current, mA, + charging */
    int16_t soc     = bq_read16(0x2C, &ok); /* StateOfCharge, % */
    if (!ok) return;
    out->valid    = true;
    out->volts    = mv / 1000.0f;
    out->amps     = ma / 1000.0f;
    out->percent  = (mv < 2500) ? -1 : (soc < 0 ? 0 : (soc > 100 ? 100 : soc));
    out->charging = ma > 20;
}

/* ---- Clock (PCF8563) ------------------------------------------------------ */

static uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static bool rtc_read(struct tm *t)
{
    uint8_t r[7];
    if (reg_read(s_rtc, 0x02, r, sizeof(r)) != ESP_OK) return false;
    if (r[0] & 0x80) return false; /* VL: clock integrity lost (battery ran out) */
    memset(t, 0, sizeof(*t));
    t->tm_sec  = bcd2bin(r[0] & 0x7F);
    t->tm_min  = bcd2bin(r[1] & 0x7F);
    t->tm_hour = bcd2bin(r[2] & 0x3F);
    t->tm_mday = bcd2bin(r[3] & 0x3F);
    t->tm_wday = r[4] & 0x07;
    t->tm_mon  = bcd2bin(r[5] & 0x1F) - 1;
    t->tm_year = bcd2bin(r[6]) + 100 + ((r[5] & 0x80) ? 100 : 0); /* years since 1900 */
    t->tm_isdst = -1;
    return true;
}

static void rtc_seed_system_time(void)
{
    struct tm t;
    if (!rtc_read(&t) || t.tm_year < 124) {
        ESP_LOGW(TAG, "RTC not set");
        return;
    }
    struct timeval tv = {mktime(&t), 0};
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "time from RTC: %04d-%02d-%02d %02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
             t.tm_min);
}

bool hal_rtc_set(const struct tm *local)
{
    struct tm t   = *local;
    time_t epoch  = mktime(&t); /* normalizes fields and fills tm_wday */
    struct timeval tv = {epoch, 0};
    settimeofday(&tv, NULL);
    if (!s_rtc_ok) return false;
    int year = t.tm_year + 1900;
    uint8_t w[8] = {
        0x02,
        bin2bcd((uint8_t)t.tm_sec),
        bin2bcd((uint8_t)t.tm_min),
        bin2bcd((uint8_t)t.tm_hour),
        bin2bcd((uint8_t)t.tm_mday),
        (uint8_t)t.tm_wday,
        (uint8_t)(bin2bcd((uint8_t)(t.tm_mon + 1)) | (year >= 2100 ? 0x80 : 0)),
        bin2bcd((uint8_t)(year % 100)),
    };
    return i2c_master_transmit(s_rtc, w, sizeof(w), I2C_TIMEOUT_MS) == ESP_OK;
}

bool hal_rtc_present(void) { return s_rtc_ok; }

/* ---- Storage -------------------------------------------------------------- */

static void sd_mount(void)
{
    tdp4_xl_write(TDP4_XL_SD_PWR, false); /* power on (active low) */
    vTaskDelay(pdMS_TO_TICKS(20));

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot         = SDMMC_HOST_SLOT_0; /* IOMUX pins 39..44, powered by LDO4 */
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width               = 4;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files              = 8,
        .allocation_unit_size   = 16 * 1024,
    };
    esp_err_t err = esp_vfs_fat_sdmmc_mount(HAL_SD_MOUNT, &host, &slot, &mcfg, &s_card);
    if (err == ESP_OK) {
        s_sd_ok = true;
        ESP_LOGI(TAG, "SD mounted: %s, %llu MB", s_card->cid.name,
                 (unsigned long long)((uint64_t)s_card->csd.capacity * s_card->csd.sector_size >> 20));
    } else {
        ESP_LOGW(TAG, "no SD card (%s)", esp_err_to_name(err));
        tdp4_xl_write(TDP4_XL_SD_PWR, true); /* power it back off */
    }
}

/* ---- Init ----------------------------------------------------------------- */

bool hal_init(void)
{
    hal_common_nvs_init();
    hal_common_set_board_name("T-DISPLAY P4 / RM69A10");

    if (new_bus(LP_I2C_NUM_0, TDP4_MAIN_SDA, TDP4_MAIN_SCL, &s_main_bus) != ESP_OK ||
        new_bus(I2C_NUM_1, TDP4_AUDIO_SDA, TDP4_AUDIO_SCL, &s_audio_bus) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed");
        return false;
    }
    if (!xl9535_init()) return false;
    if (!ldo_init()) return false;
    /* Keyboard bus after LDO4: its pins sit in that power domain. */
    if (new_bus(I2C_NUM_0, TDP4_KBD_SDA, TDP4_KBD_SCL, &s_kbd_bus) != ESP_OK) {
        ESP_LOGW(TAG, "keyboard I2C bus init failed");
    }

    if (!display_init()) {
        return false;
    }
    hal_backlight_set((uint8_t)hal_cfg_get_i32("bright", 80));

    hal_common_tz_apply((int)hal_cfg_get_i32("tz", DECK_TZ_DEFAULT)); /* before the RTC is read as local time */
    s_rtc_ok = add_dev(s_main_bus, TDP4_ADDR_PCF8563, 400000, &s_rtc) == ESP_OK &&
               i2c_master_probe(s_main_bus, TDP4_ADDR_PCF8563, I2C_TIMEOUT_MS) == ESP_OK;
    if (s_rtc_ok) {
        reg_write(s_rtc, 0x00, 0x00); /* control 1: clock running, normal mode */
        rtc_seed_system_time();
    } else {
        ESP_LOGW(TAG, "RTC not found");
    }

    s_bq_ok = add_dev(s_main_bus, TDP4_ADDR_BQ27220, 100000, &s_bq) == ESP_OK &&
              i2c_master_probe(s_main_bus, TDP4_ADDR_BQ27220, I2C_TIMEOUT_MS) == ESP_OK;
    if (!s_bq_ok) ESP_LOGW(TAG, "BQ27220 fuel gauge not found");

    sd_mount();
    hal_common_set_sd_mounted(s_sd_ok);
    if (!s_sd_ok) hal_common_flash_fs_mount();

    s_key_q = xQueueCreate(64, sizeof(hal_key_t));
    if (s_kbd_bus) tdp4_kbd_start(s_kbd_bus, s_key_q);

    s_orient_ok = tdp4_orient_start(s_audio_bus);
    /* Start in the wanted rotation if it is fixed (manual setting); with
     * AUTO the shell turns the screen once the sensor has settled. */
    if (hal_orient_mode() != HAL_ORIENT_AUTO) {
        hal_lvgl_lock(0);
        hal_display_set_rotation(hal_orient_wanted());
        hal_lvgl_unlock();
    }
    return true;
}
