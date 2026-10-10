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
#include "esp_ldo_regulator.h"
#include "esp_log.h"
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
    /* Port 1: USB PHY power on, C6 held off until hal_radio_init(), SD power
     * off (high). Inputs: GPS wake, RTC INT, C6 wake, radio reset/IRQ. */
    s_xl_out[1] = 0xEF;
    const uint8_t cfg1 = 0xCE;

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

    /* Two frame buffers in PSRAM: esp_lvgl_port's tear-free direct mode
     * draws into the hidden one and swaps on the panel's refresh. */
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel    = 0,
        .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 60,
        .pixel_format       = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .in_color_format    = LCD_COLOR_FMT_RGB565,
        .out_color_format   = LCD_COLOR_FMT_RGB565,
        .num_fbs            = 2,
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
        .flags.use_dma2d = true,
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

static lv_display_t *s_disp;

static bool display_init(void)
{
    if (!panel_init()) return false;

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

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = s_lcd_io,
        .panel_handle  = s_lcd,
        .buffer_size   = TDP4_LCD_W * TDP4_LCD_H,
        .hres          = TDP4_LCD_W,
        .vres          = TDP4_LCD_H,
        .color_format  = LV_COLOR_FORMAT_RGB565,
        .flags =
            {
                .buff_spiram = 1,
                .direct_mode = 1,
            },
    };
    lvgl_port_display_dsi_cfg_t dsi_cfg = {
        .flags.avoid_tearing = 1,
    };
    s_disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (s_disp == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp_dsi failed");
        return false;
    }

    if (!tdp4_touch_init(s_main_bus, s_disp)) {
        ESP_LOGW(TAG, "GT9895 touch not found (TFT board? only the AMOLED is supported)");
    }
    return true;
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
void hal_display_power(bool on)
{
    if (on == s_display_on || s_lcd_io == NULL) return;
    s_display_on = on;
    if (on) {
        lcd_cmd(0x11, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(120));
        lcd_cmd(0x29, NULL, 0);
        panel_brightness(s_brightness);
        tdp4_kbd_backlight(s_kbd_light);
    } else {
        panel_brightness(0);
        lcd_cmd(0x28, NULL, 0);
        lcd_cmd(0x10, NULL, 0);
        tdp4_kbd_backlight(0);
    }
}

bool hal_lvgl_lock(uint32_t timeout_ms) { return lvgl_port_lock(timeout_ms); }
void hal_lvgl_unlock(void) { lvgl_port_unlock(); }

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

bool hal_radio_init(void)
{
    static bool s_done, s_ok;
    if (s_done) return s_ok;
    s_done = true;
    if (s_xl == NULL) return false;
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
    return true;
}
