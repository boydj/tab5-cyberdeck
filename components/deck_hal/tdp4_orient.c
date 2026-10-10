/*
 * Orientation for the T-Display P4: which edge of the deck is up, from the
 * board's motion sensor, for the AUTO rotation setting.
 *
 * v1.0 boards carry either an ICM20948 (0x68) or a QMI8658 (0x6A) on the
 * audio I2C bus. Only the accelerometer is used, in its low-power mode at
 * about 10 Hz, polled by a small task.
 *
 * The sensor's axes are not documented relative to the screen, so the
 * mapping chip -> screen is a setting ("imu_map") with a guessed default
 * that the deck corrects once, on its own:
 *   - while the keyboard is docked the deck is in portrait, so the first
 *     steady, clearly tilted docked reading tells which chip axis points
 *     down the screen;
 *   - a hand-held screen leans back, and a docked deck lying flat is face
 *     up, so the first such reading tells which way the chip's Z axis
 *     faces, and with that the screen X axis.
 * Each is learned once and then kept, so a deck later left docked on its
 * side or face down does not unlearn it. SYSTEM > ROTATION > CALIBRATE sets
 * both directly (hal_orient_calibrate) and turns the learning off.
 */
#include "tdp4.h"
#include "deck_hal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "orient";

#define ADDR_ICM20948 0x68
#define ADDR_QMI8658  0x6A
#define PERIOD_MS     100
#define SETTLE_N      5   /* readings a new orientation must hold (0.5 s) */
#define LEARN_N       30  /* steady docked readings before learning (3 s) */
#define CALIB_HOLD_MS 120000 /* learning pauses this long after calibration step 0 */

static i2c_master_dev_handle_t s_dev;
static enum { IMU_NONE, IMU_ICM20948, IMU_QMI8658 } s_type;

/* ---- Sensor access -------------------------------------------------------- */

static esp_err_t wr(uint8_t reg, uint8_t v)
{
    uint8_t b[2] = {reg, v};
    return i2c_master_transmit(s_dev, b, 2, 50);
}

static esp_err_t rd(uint8_t reg, uint8_t *out, size_t n) { return i2c_master_transmit_receive(s_dev, &reg, 1, out, n, 50); }

static bool add_dev(i2c_master_bus_handle_t bus, uint8_t addr)
{
    if (i2c_master_probe(bus, addr, 50) != ESP_OK) return false;
    i2c_device_config_t c = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 400000};
    return i2c_master_bus_add_device(bus, &c, &s_dev) == ESP_OK;
}

/* ICM20948, bank 0 unless noted: WHO_AM_I 0x00 = 0xEA, LP_CONFIG 0x05,
 * PWR_MGMT_1 0x06, PWR_MGMT_2 0x07, ACCEL_XOUT_H 0x2D (big-endian), bank
 * select 0x7F; bank 2: ACCEL_SMPLRT_DIV 0x10/0x11, ACCEL_CONFIG 0x14. */
static bool icm_init(void)
{
    uint8_t id = 0;
    if (wr(0x7F, 0x00) != ESP_OK || rd(0x00, &id, 1) != ESP_OK || id != 0xEA) return false;
    wr(0x06, 0x80); /* reset */
    vTaskDelay(pdMS_TO_TICKS(100));
    bool ok = wr(0x7F, 0x00) == ESP_OK && wr(0x06, 0x01) == ESP_OK; /* awake, best clock */
    vTaskDelay(pdMS_TO_TICKS(10));
    ok = ok && wr(0x07, 0x07) == ESP_OK;                               /* accel on, gyro off */
    ok = ok && wr(0x7F, 0x20) == ESP_OK;                               /* bank 2 */
    ok = ok && wr(0x10, 0x00) == ESP_OK && wr(0x11, 111) == ESP_OK;    /* 1125 / 112 = ~10 Hz */
    ok = ok && wr(0x14, (7 << 3) | 0x01) == ESP_OK;                    /* +-2 g, DLPF 7 (duty-cycled, 4x avg) */
    ok = ok && wr(0x7F, 0x00) == ESP_OK;
    ok = ok && wr(0x05, 0x20) == ESP_OK;                               /* accel duty-cycled */
    ok = ok && wr(0x06, 0x21) == ESP_OK;                               /* low-power digital */
    return ok;
}

static bool icm_read(float a[3])
{
    uint8_t b[6];
    if (rd(0x2D, b, 6) != ESP_OK) return false;
    for (int i = 0; i < 3; i++) a[i] = (int16_t)((b[2 * i] << 8) | b[2 * i + 1]) / 16384.0f;
    return true;
}

/* QMI8658: WHO_AM_I 0x00 = 0x05, CTRL1 0x02, CTRL2 0x03 (accel range and
 * rate), CTRL7 0x08 (enables), RESET 0x60, AX_L 0x35 (little-endian). */
static bool qmi_init(void)
{
    uint8_t id = 0;
    if (rd(0x00, &id, 1) != ESP_OK || id != 0x05) return false;
    wr(0x60, 0xB0); /* reset */
    vTaskDelay(pdMS_TO_TICKS(20));
    return wr(0x02, 0x40) == ESP_OK      /* register auto-increment, little-endian */
           && wr(0x03, 0x0D) == ESP_OK   /* +-2 g, 21 Hz low-power (accel only) */
           && wr(0x08, 0x01) == ESP_OK;  /* accelerometer on, gyro off */
}

static bool qmi_read(float a[3])
{
    uint8_t b[6];
    if (rd(0x35, b, 6) != ESP_OK) return false;
    for (int i = 0; i < 3; i++) a[i] = (int16_t)(b[2 * i] | (b[2 * i + 1] << 8)) / 16384.0f;
    return true;
}

/* ---- Chip -> screen mapping ----------------------------------------------- */

/* A signed chip axis: 0..2 = x, y, z; sign +1 / -1. */
typedef struct {
    int8_t axis, sign;
} sax_t;

/* Screen X (right), Y (down), Z (into the screen, X x Y) in chip axes. */
static sax_t s_x, s_y, s_z;
static bool s_calibrated; /* set by hal_orient_calibrate; learning stays off */
static bool s_learned_y, s_learned_z; /* each is learned once, then kept */
/* The mapping is written by the orient task (learning) and the LVGL task
 * (calibration) and read by the orient task. */
static portMUX_TYPE s_map_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile TickType_t s_calib_tick; /* calibration step 0 time, 0 = none */

/* Default guess: the chip faces the back of the board (Z into the screen)
 * and its X axis runs up the screen. */
static void map_default(void)
{
    s_y = (sax_t){0, -1};
    s_z = (sax_t){2, +1};
    s_x = (sax_t){1, +1};
    s_calibrated = false;
    s_learned_y = s_learned_z = false;
}

/* Cross product of two signed axes (they must differ). */
static sax_t cross(sax_t a, sax_t b)
{
    int c = 3 - a.axis - b.axis;                        /* the remaining axis */
    int even = (b.axis == (a.axis + 1) % 3) ? 1 : -1;  /* x*y = z, y*z = x, z*x = y */
    return (sax_t){(int8_t)c, (int8_t)(a.sign * b.sign * even)};
}

/* "imu_map": bits 0-1 X axis, bit 2 X negative, bits 3-4 Y axis, bit 5 Y
 * negative, bits 6-7 Z axis, bit 8 Z negative, bit 9 calibrated, bit 10 valid,
 * bit 11 Y learned, bit 12 Z learned. Written only when something changes. */
static void map_save(void)
{
    int32_t v = s_x.axis | (s_x.sign < 0) << 2 | s_y.axis << 3 | (s_y.sign < 0) << 5 | s_z.axis << 6 |
                (s_z.sign < 0) << 8 | (s_calibrated ? 1 << 9 : 0) | 1 << 10 | (s_learned_y ? 1 << 11 : 0) |
                (s_learned_z ? 1 << 12 : 0);
    hal_cfg_set_i32("imu_map", v);
}

static void map_load(void)
{
    map_default();
    int32_t v = hal_cfg_get_i32("imu_map", 0);
    if (!(v & (1 << 10))) return;
    sax_t x = {(int8_t)(v & 3), (int8_t)((v >> 2) & 1 ? -1 : 1)};
    sax_t y = {(int8_t)((v >> 3) & 3), (int8_t)((v >> 5) & 1 ? -1 : 1)};
    sax_t z = {(int8_t)((v >> 6) & 3), (int8_t)((v >> 8) & 1 ? -1 : 1)};
    if (x.axis > 2 || y.axis > 2 || z.axis > 2 || x.axis == y.axis) return;
    s_x = x;
    s_y = y;
    s_z = z;
    s_calibrated = (v >> 9) & 1;
    s_learned_y  = (v >> 11) & 1;
    s_learned_z  = (v >> 12) & 1;
}

static float along(const float a[3], sax_t s) { return a[s.axis] * s.sign; }

/* The dominant chip axis of a reading, as a signed axis, and its share. */
static sax_t dominant(const float a[3], float *mag)
{
    int k = 0;
    for (int i = 1; i < 3; i++) {
        if (fabsf(a[i]) > fabsf(a[k])) k = i;
    }
    *mag = fabsf(a[k]);
    return (sax_t){(int8_t)k, (int8_t)(a[k] < 0 ? -1 : 1)};
}

/* ---- Decision and learning ------------------------------------------------ */

static volatile hal_rot_t s_sensed = HAL_ROT_0;
static float s_last[3];

/* The reading is an "up" vector (accelerometers at rest measure the
 * reaction to gravity). In screen terms: ux > 0 means the right edge is up. */
static int classify(const float a[3])
{
    taskENTER_CRITICAL(&s_map_mux);
    sax_t sx = s_x, sy = s_y;
    taskEXIT_CRITICAL(&s_map_mux);
    float ux = along(a, sx), uy = along(a, sy);
    float plane = sqrtf(ux * ux + uy * uy);
    if (plane < 0.55f) return -1; /* close to flat: keep what we have */
    if (fabsf(uy) >= 1.5f * fabsf(ux)) return uy < 0 ? HAL_ROT_0 : -1; /* upside down: ignore */
    if (fabsf(ux) >= 1.5f * fabsf(uy)) return ux > 0 ? HAL_ROT_270 : HAL_ROT_90;
    return -1; /* near 45 degrees */
}

/* Calibration in progress (step 0 taken, step 1 not yet, within the hold). */
static bool calibrating(void)
{
    TickType_t t = s_calib_tick;
    return t != 0 && (xTaskGetTickCount() - t) < pdMS_TO_TICKS(CALIB_HOLD_MS);
}

static void learn(const float a[3], bool steady)
{
    static int n_down, n_face;
    static sax_t cand_down, cand_face;
    if (s_calibrated || (s_learned_y && s_learned_z) || calibrating() || !steady || !tdp4_kbd_present()) {
        n_down = n_face = 0;
        return;
    }
    float m;
    sax_t up = dominant(a, &m);
    sax_t face = {-1, 0}; /* chip direction out of the screen, if this reading shows it */

    /* Docked and held tilted: the up vector lies along the screen, toward
     * its top edge (screen -Y). Ignore Z: that is the flat case below. */
    bool tilted = up.axis != s_z.axis && m > 0.6f && fabsf(a[s_z.axis]) < 0.8f &&
                  fabsf(a[3 - up.axis - s_z.axis]) < 0.25f;
    if (tilted && !s_learned_y) {
        if (n_down == 0 || cand_down.axis != up.axis || cand_down.sign != up.sign) {
            cand_down = up;
            n_down    = 0;
        }
        if (++n_down == LEARN_N) {
            taskENTER_CRITICAL(&s_map_mux);
            s_y         = (sax_t){up.axis, (int8_t)-up.sign};
            s_x         = cross(s_y, s_z);
            s_learned_y = true;
            taskEXIT_CRITICAL(&s_map_mux);
            map_save();
            ESP_LOGI(TAG, "learned: screen down = %c%c", s_y.sign < 0 ? '-' : '+', 'x' + s_y.axis);
        }
    } else {
        n_down = 0;
    }

    /* Which way the screen faces: held tilted it leans back (up has a share
     * out of the screen); docked and lying flat it is face up. */
    if (tilted && fabsf(a[s_z.axis]) > 0.3f) face = (sax_t){s_z.axis, (int8_t)(a[s_z.axis] < 0 ? -1 : 1)};
    if (up.axis == s_z.axis && m > 0.9f) face = up;
    if (face.axis >= 0 && !s_learned_z) {
        if (n_face == 0 || cand_face.sign != face.sign) {
            cand_face = face;
            n_face    = 0;
        }
        if (++n_face == LEARN_N) {
            taskENTER_CRITICAL(&s_map_mux);
            s_z         = (sax_t){face.axis, (int8_t)-face.sign}; /* screen Z points into it */
            s_x         = cross(s_y, s_z);
            s_learned_z = true;
            taskEXIT_CRITICAL(&s_map_mux);
            map_save();
            ESP_LOGI(TAG, "learned: chip Z %s the screen", s_z.sign > 0 ? "into" : "out of");
        }
    } else {
        n_face = 0;
    }
}

static bool sensor_read(float a[3]) { return s_type == IMU_ICM20948 ? icm_read(a) : qmi_read(a); }

static void orient_task(void *arg)
{
    (void)arg;
    int cand = -1, held = 0;
    float prev[3] = {0};
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PERIOD_MS));
        float a[3];
        if (!sensor_read(a)) continue;
        memcpy(s_last, a, sizeof(s_last));
        float mag    = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        float dx     = a[0] - prev[0], dy = a[1] - prev[1], dz = a[2] - prev[2];
        bool steady  = mag > 0.85f && mag < 1.15f && sqrtf(dx * dx + dy * dy + dz * dz) < 0.06f;
        memcpy(prev, a, sizeof(prev));
        learn(a, steady);
        if (mag < 0.75f || mag > 1.25f) { /* shaken or moving: not a pose */
            held = 0;
            continue;
        }
        int c = classify(a);
        if (c < 0) {
            held = 0;
            continue;
        }
        if (c != cand) {
            cand = c;
            held = 0;
        }
        if (++held >= SETTLE_N) s_sensed = (hal_rot_t)c;
    }
}

/* ---- Public --------------------------------------------------------------- */

bool tdp4_orient_start(i2c_master_bus_handle_t bus)
{
    if (add_dev(bus, ADDR_ICM20948) && icm_init()) {
        s_type = IMU_ICM20948;
    } else {
        if (s_dev) i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        if (add_dev(bus, ADDR_QMI8658) && qmi_init()) s_type = IMU_QMI8658;
    }
    if (s_type == IMU_NONE) {
        ESP_LOGW(TAG, "no motion sensor found; AUTO rotation stays portrait");
        return false;
    }
    map_load();
    ESP_LOGI(TAG, "%s, screen X=%c%c Y=%c%c Z=%c%c%s", tdp4_orient_sensor(), s_x.sign < 0 ? '-' : '+', 'x' + s_x.axis,
             s_y.sign < 0 ? '-' : '+', 'x' + s_y.axis, s_z.sign < 0 ? '-' : '+', 'x' + s_z.axis,
             s_calibrated ? " (calibrated)" : "");
    xTaskCreatePinnedToCore(orient_task, "orient", 4096, NULL, 3, NULL, 0);
    return true;
}

const char *tdp4_orient_sensor(void)
{
    switch (s_type) {
        case IMU_ICM20948: return "ICM20948";
        case IMU_QMI8658: return "QMI8658";
        default: return NULL;
    }
}

hal_rot_t tdp4_orient_sensed(void) { return s_sensed; }

/* Averages a few readings; false if the deck moved or is being shaken. */
static bool steady_reading(float out[3])
{
    float first[3];
    memset(out, 0, 3 * sizeof(float));
    for (int i = 0; i < 5; i++) {
        float a[3];
        if (!sensor_read(a)) return false;
        if (i == 0) memcpy(first, a, sizeof(first));
        for (int k = 0; k < 3; k++) {
            if (fabsf(a[k] - first[k]) > 0.12f) return false; /* moving */
            out[k] += a[k] / 5;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    float mag = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    return mag > 0.8f && mag < 1.2f;
}

/* Two poses: upright portrait (up = top edge = screen -Y) and turned
 * clockwise (up = left edge = screen -X). A hand-held deck leans back, so
 * each reading also carries a share along the screen normal, about the same
 * in both poses. The axis that changes least between them is the normal;
 * the other two are screen Y (large in pose 0) and X (large in pose 1). */
bool tdp4_orient_calibrate(int step)
{
    static float s_a0[3];
    static bool s_have0;
    float a[3];
    if (step < 0) { /* cancelled */
        s_have0      = false;
        s_calib_tick = 0;
        return true;
    }
    if (s_type == IMU_NONE || !steady_reading(a)) return false;
    if (step == 0) {
        memcpy(s_a0, a, sizeof(s_a0));
        s_have0      = true;
        s_calib_tick = xTaskGetTickCount() | 1; /* never 0 */
        return true;
    }
    if (!s_have0) return false;
    int n = 0;
    for (int k = 1; k < 3; k++) {
        if (fabsf(a[k] - s_a0[k]) < fabsf(a[n] - s_a0[n])) n = k;
    }
    int p = (n + 1) % 3, q = (n + 2) % 3;
    int yi = fabsf(s_a0[p]) >= fabsf(s_a0[q]) ? p : q; /* down axis: big in pose 0 */
    int xi = 3 - n - yi;
    /* The lean must be about the same in both poses (else the least-changed
     * axis need not be the normal), each pose must be clearly on its edge,
     * and the turn must have moved gravity from one in-plane axis to the
     * other. */
    if (fabsf(a[n] - s_a0[n]) > 0.5f || fabsf(s_a0[yi]) < 0.45f || fabsf(a[xi]) < 0.45f || fabsf(a[yi]) > 0.35f ||
        fabsf(s_a0[xi]) > 0.35f) {
        ESP_LOGW(TAG, "calibration rejected: pose0 %.2f %.2f %.2f, pose1 %.2f %.2f %.2f", s_a0[0], s_a0[1], s_a0[2], a[0],
                 a[1], a[2]);
        return false;
    }
    taskENTER_CRITICAL(&s_map_mux);
    s_y          = (sax_t){(int8_t)yi, (int8_t)(s_a0[yi] < 0 ? 1 : -1)};
    s_x          = (sax_t){(int8_t)xi, (int8_t)(a[xi] < 0 ? 1 : -1)};
    s_z          = cross(s_x, s_y);
    s_calibrated = true;
    taskEXIT_CRITICAL(&s_map_mux);
    s_have0      = false;
    s_calib_tick = 0;
    map_save();
    ESP_LOGI(TAG, "calibrated: screen X=%c%c Y=%c%c", s_x.sign < 0 ? '-' : '+', 'x' + s_x.axis, s_y.sign < 0 ? '-' : '+',
             'x' + s_y.axis);
    return true;
}
