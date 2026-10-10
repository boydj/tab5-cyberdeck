/*
 * GT9895 touch (Goodix "Berlin" family) for the T-Display P4 AMOLED, as an
 * LVGL pointer input.
 *
 * Protocol, as used by LilyGo's cpp_bus_driver and Linux's goodix_berlin
 * driver: 32-bit big-endian register addresses; the runtime info block at
 * 0x10070 gives the touch report address; a report is an 8-byte event
 * header (byte 0 = event flags, bit 7 = touch; byte 2 low nibble = contact
 * count; 16-bit checksum in bytes 6-7) followed by 8 bytes per contact
 * (x, y little-endian at offsets 2 and 4) and a 16-bit checksum. Writing 0
 * to the report address acknowledges it.
 *
 * The controller reports in a 1060 x 2400 space that maps linearly onto the
 * 568 x 1232 panel (LilyGo's coordinate transform). The INT line is behind
 * the IO expander, so the report is polled from the LVGL read callback.
 */
#include "tdp4.h"
#include "deck_hal.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";

#define RAW_W 1060
#define RAW_H 2400

#define RUNTIME_INFO_ADDR 0x00010070u
#define HEADER_LEN        8
#define CONTACT_LEN       8
#define CSUM_LEN          2
#define REPORT_LEN        (HEADER_LEN + CONTACT_LEN + CSUM_LEN) /* single touch */
#define RELEASE_AFTER_US  (150 * 1000)                          /* missed lift-off guard */

static i2c_master_dev_handle_t s_dev;
static uint32_t s_report_addr;
static bool s_pressed;
static int32_t s_x, s_y;
static int64_t s_last_report_us;

static esp_err_t gt_read(uint32_t addr, uint8_t *out, size_t n)
{
    uint8_t a[4] = {(uint8_t)(addr >> 24), (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr};
    return i2c_master_transmit_receive(s_dev, a, sizeof(a), out, n, 50);
}

static esp_err_t gt_write_byte(uint32_t addr, uint8_t v)
{
    uint8_t b[5] = {(uint8_t)(addr >> 24), (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr, v};
    return i2c_master_transmit(s_dev, b, sizeof(b), 50);
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* Sum of all bytes but the last two equals the little-endian 16-bit tail. */
static bool csum_ok(const uint8_t *p, size_t n)
{
    if (n < CSUM_LEN) return false;
    uint32_t sum = 0;
    for (size_t i = 0; i < n - CSUM_LEN; i++) sum += p[i];
    return (uint16_t)sum == le16(p + n - CSUM_LEN);
}

/* Walk the runtime info block to the touch report address (offset layout
 * from LilyGo's Gt9895::ReadRuntimeInfo). */
static bool read_report_addr(void)
{
    uint8_t lenb[2];
    if (gt_read(RUNTIME_INFO_ADDR, lenb, 2) != ESP_OK) return false;
    size_t len = le16(lenb);
    if (len < 2 + 16 + 10 + 4 + 5 + 48 + CSUM_LEN || len > 1024) {
        ESP_LOGE(TAG, "runtime info length %u invalid", (unsigned)len);
        return false;
    }
    uint8_t *d = malloc(len);
    if (d == NULL) return false;
    bool ok = gt_read(RUNTIME_INFO_ADDR, d, len) == ESP_OK && csum_ok(d, len);
    if (ok) {
        size_t off = 2 + 16 + 10 + 4; /* length, version, features, fixed params */
        for (int arr = 0; arr < 5 && ok; arr++) {
            if (off >= len - CSUM_LEN) {
                ok = false;
                break;
            }
            size_t count = d[off++];
            off += count * 2;
        }
        if (ok && off + 48 <= len - CSUM_LEN) {
            s_report_addr = le32(d + off + 44);
            ok            = s_report_addr != 0;
        } else {
            ok = false;
        }
    }
    free(d);
    if (!ok) ESP_LOGE(TAG, "runtime info unreadable");
    return ok;
}

static void poll_report(void)
{
    uint8_t r[REPORT_LEN];
    if (gt_read(s_report_addr, r, sizeof(r)) != ESP_OK) return;
    if (r[0] == 0) {
        /* Nothing new this scan. Hold the last state, but let go if the
         * lift-off report was lost. */
        if (s_pressed && esp_timer_get_time() - s_last_report_us > RELEASE_AFTER_US) s_pressed = false;
        return;
    }
    if (!csum_ok(r, HEADER_LEN)) {
        gt_write_byte(s_report_addr, 0);
        return;
    }
    s_last_report_us = esp_timer_get_time();
    uint8_t count    = r[2] & 0x0F;
    if ((r[0] & 0x80) == 0 || count == 0) {
        s_pressed = false; /* lift-off or a non-touch event */
    } else if (csum_ok(r + HEADER_LEN, CONTACT_LEN + CSUM_LEN) || count > 1) {
        /* With more than one contact the payload checksum covers bytes we did
         * not read; the first contact is still valid. */
        uint32_t x = le16(r + HEADER_LEN + 2), y = le16(r + HEADER_LEN + 4);
        x          = x * TDP4_LCD_W / RAW_W;
        y          = y * TDP4_LCD_H / RAW_H;
        s_x        = (int32_t)(x < TDP4_LCD_W ? x : TDP4_LCD_W - 1);
        s_y        = (int32_t)(y < TDP4_LCD_H ? y : TDP4_LCD_H - 1);
        s_pressed  = true;
    }
    gt_write_byte(s_report_addr, 0); /* acknowledge */
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    poll_report();
    data->point.x = s_x;
    data->point.y = s_y;
    data->state   = s_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

bool tdp4_touch_init(i2c_master_bus_handle_t bus, lv_display_t *disp)
{
    /* Reset through the expander (LilyGo: low 30 ms, then 100 ms). */
    tdp4_xl_write(TDP4_XL_TP_RST, false);
    vTaskDelay(pdMS_TO_TICKS(30));
    tdp4_xl_write(TDP4_XL_TP_RST, true);
    vTaskDelay(pdMS_TO_TICKS(100));

    if (i2c_master_probe(bus, TDP4_ADDR_GT9895, 50) != ESP_OK) return false;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = TDP4_ADDR_GT9895,
        .scl_speed_hz    = 400000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) return false;
    if (!read_report_addr()) return false;
    ESP_LOGI(TAG, "GT9895 ready, reports at 0x%08lx", (unsigned long)s_report_addr);

    hal_lvgl_lock(0); /* the LVGL task is already running */
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    lv_indev_set_display(indev, disp);
    hal_lvgl_unlock();
    return true;
}
