/*
 * Radio repair for the T-Display P4: replace ESP-AT on the ESP32-C6 with
 * LilyGo's ESP-Hosted network adapter, without a USB-UART adapter.
 *
 * Some boards ship with Espressif's AT firmware (SDIO build) on the C6.
 * DECK//OS talks ESP-Hosted, and the C6's UART and BOOT button are only on a
 * small connector, so the documented fix needs extra hardware. ESP-AT can
 * update itself, though: AT+USEROTA downloads an app image over its own
 * Wi-Fi into the other OTA slot and boots it. The network adapter is an
 * ordinary app image (1.15 MB; AT's slots are 1.8 MB) and only needs the
 * "nvs" partition, which AT's table has.
 *
 * This file is a minimal AT client over SDIO: the ESP SDIO slave protocol
 * (ESP-IDF docs, "Communication with ESP SDIO Slave"): the host reads
 * PKT_LEN for bytes waiting, TOKEN_RDATA for free receive buffers, and moves
 * data with CMD53 at 0x1F800 - remaining length. It polls instead of using
 * the DAT1 interrupt. Nothing here runs unless the user asked for a repair,
 * and then ESP-Hosted is not started on that boot (hal_radio_init).
 */
#include "tdp4.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/sdmmc_defs.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

static const char *TAG = "c6fix";

/* SDIO slot 1 to the C6 (same pins ESP-Hosted uses). */
#define PIN_CLK 18
#define PIN_CMD 19
#define PIN_D0  14
#define PIN_D1  15
#define PIN_D2  16
#define PIN_D3  17

/* ESP SDIO slave registers (function 1, address & 0x3FF) and FIFO window. */
#define REG_TOKEN_RDATA 0x044 /* bits 27..16: receive buffers the slave has loaded */
#define REG_INT_ST      0x058
#define REG_PKT_LEN     0x060 /* bits 19..0: bytes the slave has queued, cumulative */
#define REG_INT_CLR     0x0D4
#define FIFO_END        0x1F800
#define RX_MASK         0xFFFFF
#define TOKEN_MASK      0xFFF
#define TX_CHUNK        64  /* bytes per receive buffer used; AT's buffers are larger */
#define RX_CHUNK        512 /* CMD53 byte mode limit */

static sdmmc_card_t s_card;
static uint8_t *s_dma; /* RX_CHUNK bytes, DMA capable */
static uint32_t s_rx_got;  /* bytes read so far (mod 2^20) */
static uint32_t s_tx_used; /* receive buffers used so far (mod 2^12) */

/* ---- Status for the UI ----------------------------------------------------- */

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_status[112];
static bool s_ran;
static bool s_quiet; /* update the line without logging it (progress ticks) */

static void status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void status(const char *fmt, ...)
{
    char buf[sizeof(s_status)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (!s_quiet) ESP_LOGI(TAG, "%s", buf);
    taskENTER_CRITICAL(&s_lock);
    memcpy(s_status, buf, sizeof(s_status));
    s_ran = true;
    taskEXIT_CRITICAL(&s_lock);
}

bool tdp4_c6_repair_status(char *out, size_t n)
{
    if (n == 0) return false;
    taskENTER_CRITICAL(&s_lock);
    bool ran = s_ran;
    if (ran) {
        size_t len = strnlen(s_status, sizeof(s_status));
        if (len >= n) len = n - 1;
        memcpy(out, s_status, len);
        out[len] = '\0';
    }
    taskEXIT_CRITICAL(&s_lock);
    return ran;
}

/* ---- SDIO link --------------------------------------------------------------- */

static esp_err_t reg_read32(uint32_t reg, uint32_t *v)
{
    esp_err_t err = sdmmc_io_read_bytes(&s_card, 1, reg, s_dma, 4);
    if (err == ESP_OK) memcpy(v, s_dma, 4);
    return err;
}

static esp_err_t set_block_size(uint32_t fn, uint16_t bs)
{
    uint32_t base = SD_IO_FBR_START * fn;
    esp_err_t err = sdmmc_io_write_byte(&s_card, 0, base + SD_IO_CCCR_BLKSIZEL, bs & 0xFF, NULL);
    if (err == ESP_OK) err = sdmmc_io_write_byte(&s_card, 0, base + SD_IO_CCCR_BLKSIZEH, bs >> 8, NULL);
    return err;
}

/* Enable I/O function 1 and wait until the slave says it is ready. */
static esp_err_t fn1_enable(void)
{
    uint8_t ioe = 0, ior = 0;
    esp_err_t err = sdmmc_io_read_byte(&s_card, 0, SD_IO_CCCR_FN_ENABLE, &ioe);
    if (err != ESP_OK) return err;
    err = sdmmc_io_write_byte(&s_card, 0, SD_IO_CCCR_FN_ENABLE, ioe | BIT(1), NULL);
    if (err != ESP_OK) return err;
    for (int i = 0; i < 50; i++) {
        err = sdmmc_io_read_byte(&s_card, 0, SD_IO_CCCR_FN_READY, &ior);
        if (err != ESP_OK) return err;
        if (ior & BIT(1)) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!(ior & BIT(1))) return ESP_ERR_TIMEOUT;
    err = set_block_size(0, 512);
    if (err == ESP_OK) err = set_block_size(1, 512);
    return err;
}

static bool link_up(void)
{
    s_dma = heap_caps_aligned_calloc(64, 1, RX_CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_dma == NULL) return false;
    if (sdmmc_host_init() != ESP_OK) return false; /* already up (SD card): fine */
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk   = PIN_CLK;
    slot.cmd   = PIN_CMD;
    slot.d0    = PIN_D0;
    slot.d1    = PIN_D1;
    slot.d2    = PIN_D2;
    slot.d3    = PIN_D3;
    if (sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot) != ESP_OK) return false;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot         = SDMMC_HOST_SLOT_1;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT; /* 20 MHz: plenty for AT, kind to the bus */
    host.flags &= ~SDMMC_HOST_FLAG_DDR;
    host.flags |= SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF; /* P4: CMD53 byte mode needs it */
    for (int i = 0; i < 5; i++) {
        memset(&s_card, 0, sizeof(s_card));
        if (sdmmc_card_init(&host, &s_card) == ESP_OK && fn1_enable() == ESP_OK) {
            s_rx_got  = 0; /* both counters restart with the slave */
            s_tx_used = 0;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    return false;
}

/* Read whatever the slave has queued (up to `max`). Returns the byte count,
 * 0 if nothing is waiting, -1 on a bus error. */
static int link_read(char *out, int max)
{
    uint32_t pkt = 0;
    if (reg_read32(REG_PKT_LEN, &pkt) != ESP_OK) return -1;
    uint32_t avail = ((pkt & RX_MASK) - s_rx_got) & RX_MASK;
    if (avail == 0) return 0;
    if (avail > (uint32_t)max) avail = (uint32_t)max;
    uint32_t left = avail;
    int got       = 0;
    while (left > 0) {
        uint32_t n = left > RX_CHUNK ? RX_CHUNK : left;
        /* The address tells the slave how much of this transfer remains. */
        if (sdmmc_io_read_bytes(&s_card, 1, FIFO_END - left, s_dma, (n + 3) & ~3u) != ESP_OK) return -1;
        memcpy(out + got, s_dma, n);
        got += (int)n;
        left -= n;
    }
    s_rx_got = (s_rx_got + avail) & RX_MASK;
    uint32_t st = 0;
    if (reg_read32(REG_INT_ST, &st) == ESP_OK && st) {
        memcpy(s_dma, &st, 4);
        sdmmc_io_write_bytes(&s_card, 1, REG_INT_CLR, s_dma, 4);
    }
    return got;
}

static esp_err_t link_write(const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len > 0) {
        size_t n    = len > TX_CHUNK ? TX_CHUNK : len;
        int64_t end = esp_timer_get_time() + 3000000;
        for (;;) { /* wait for a free receive buffer at the slave */
            uint32_t tok = 0;
            if (reg_read32(REG_TOKEN_RDATA, &tok) != ESP_OK) return ESP_FAIL;
            if ((((tok >> 16) & TOKEN_MASK) - s_tx_used) & TOKEN_MASK) break;
            if (esp_timer_get_time() > end) return ESP_ERR_TIMEOUT;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        memset(s_dma, 0, (n + 3) & ~3u);
        memcpy(s_dma, p, n);
        if (sdmmc_io_write_bytes(&s_card, 1, FIFO_END - n, s_dma, (n + 3) & ~3u) != ESP_OK) return ESP_FAIL;
        s_tx_used = (s_tx_used + 1) & TOKEN_MASK;
        p += n;
        len -= n;
    }
    return ESP_OK;
}

/* ---- AT client ---------------------------------------------------------------- */

typedef enum { AT_OK, AT_ERROR, AT_FAIL, AT_PROMPT, AT_TIMEOUT, AT_IO } at_res_t;

static char s_line[256];
static int s_line_len;
static char s_version[48];  /* from AT+GMR */
static char s_join_err[16]; /* +CWJAP:<code> */

static void on_line(const char *l)
{
    ESP_LOGI(TAG, "< %s", l);
    if (strncmp(l, "AT version:", 11) == 0) {
        size_t n = strcspn(l + 11, "(");
        if (n > sizeof(s_version) - 1) n = sizeof(s_version) - 1;
        memcpy(s_version, l + 11, n);
        s_version[n] = '\0';
    } else if (strncmp(l, "+CWJAP:", 7) == 0) {
        snprintf(s_join_err, sizeof(s_join_err), "%.*s", (int)sizeof(s_join_err) - 1, l + 7);
    }
}

/* Read lines until a final result, up to timeout_ms. With `prompt`, wait for
 * the '>' that follows "OK" after a command that takes data (or ERROR). */
static at_res_t at_wait(int timeout_ms, bool prompt)
{
    static char buf[RX_CHUNK];
    int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    at_res_t res = AT_TIMEOUT;
    while (res == AT_TIMEOUT && esp_timer_get_time() < end) {
        int n = link_read(buf, sizeof(buf));
        if (n < 0) return AT_IO;
        if (n == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (prompt && c == '>' && res == AT_TIMEOUT) {
                res        = AT_PROMPT;
                s_line_len = 0; /* the prompt has no line end */
                continue;
            }
            if (c == '\r') continue;
            if (c != '\n') {
                if (s_line_len < (int)sizeof(s_line) - 1) s_line[s_line_len++] = c;
                continue;
            }
            s_line[s_line_len] = '\0';
            s_line_len         = 0;
            if (s_line[0] == '\0') continue;
            on_line(s_line);
            if (res != AT_TIMEOUT) continue; /* keep logging the rest of this chunk */
            if (strcmp(s_line, "OK") == 0 && !prompt) res = AT_OK;
            else if (strcmp(s_line, "ERROR") == 0) res = AT_ERROR;
            else if (strcmp(s_line, "FAIL") == 0) res = AT_FAIL;
        }
    }
    return res;
}

static at_res_t at_cmd(const char *cmd, int timeout_ms)
{
    ESP_LOGI(TAG, "> %s", strncmp(cmd, "AT+CWJAP=", 9) == 0 ? "AT+CWJAP=..." : cmd);
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "%s\r\n", cmd);
    if (n <= 0 || n >= (int)sizeof(buf)) return AT_IO;
    if (link_write(buf, (size_t)n) != ESP_OK) return AT_IO;
    return at_wait(timeout_ms, false);
}

/* Quote an AT string argument: \ , and " are escaped with a backslash. */
static void at_quote(char *out, size_t n, const char *s)
{
    size_t o = 0;
    for (; *s && o + 2 < n; s++) {
        if (*s == '\\' || *s == ',' || *s == '"') out[o++] = '\\';
        out[o++] = *s;
    }
    out[o] = '\0';
}

/* ---- The repair --------------------------------------------------------------- */

typedef struct {
    char ssid[33];
    char pass[65];
    char url[256];
} repair_args_t;

static void fail(const char *what)
{
    status("FAILED: %s", what);
}

static void repair_task(void *arg)
{
    repair_args_t *a = arg;

    status("RESETTING THE RADIO");
    tdp4_xl_write(TDP4_XL_C6_EN, false);
    vTaskDelay(pdMS_TO_TICKS(100));
    tdp4_xl_write(TDP4_XL_C6_EN, true);
    vTaskDelay(pdMS_TO_TICKS(1500)); /* AT boots and loads its buffers */

    status("CONNECTING TO THE RADIO");
    if (!link_up()) {
        fail("NO SDIO LINK TO THE RADIO");
        goto out;
    }
    at_wait(2000, false); /* "ready" */

    at_res_t r = AT_TIMEOUT;
    for (int i = 0; i < 3 && r != AT_OK; i++) r = at_cmd("ATE0", 1000);
    if (r != AT_OK) {
        /* Not ESP-AT (already the network adapter?), or not talking. */
        fail("THE RADIO DOES NOT ANSWER AT COMMANDS");
        goto out;
    }
    at_cmd("AT+GMR", 2000);
    status("RADIO RUNS ESP-AT %s", s_version[0] ? s_version : "");
    if (at_cmd("AT+CWMODE=1", 2000) != AT_OK) {
        fail("AT+CWMODE");
        goto out;
    }

    status("JOINING %s", a->ssid);
    char ssid[70], pass[132], cmd[220];
    at_quote(ssid, sizeof(ssid), a->ssid);
    at_quote(pass, sizeof(pass), a->pass);
    snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", ssid, pass);
    memset(pass, 0, sizeof(pass));
    r = at_cmd(cmd, 30000);
    memset(cmd, 0, sizeof(cmd));
    if (r != AT_OK) {
        /* +CWJAP: 1 timeout, 2 wrong password, 3 no such network, 4 failed */
        const char *why = strcmp(s_join_err, "2") == 0 ? "WRONG PASSWORD" : strcmp(s_join_err, "3") == 0 ? "NETWORK NOT FOUND"
                                                                                                           : "COULD NOT JOIN";
        status("FAILED: %s (%s)", why, a->ssid);
        goto out;
    }

    /* AT+USEROTA=<len>, wait for '>', send the URL (no line end). */
    size_t ulen = strlen(a->url);
    snprintf(cmd, sizeof(cmd), "AT+USEROTA=%u", (unsigned)ulen);
    ESP_LOGI(TAG, "> %s", cmd);
    strlcat(cmd, "\r\n", sizeof(cmd));
    if (link_write(cmd, strlen(cmd)) != ESP_OK || at_wait(5000, true) != AT_PROMPT) {
        fail("AT+USEROTA NOT ACCEPTED");
        goto out;
    }
    ESP_LOGI(TAG, "> %s", a->url);
    if (link_write(a->url, ulen) != ESP_OK) {
        fail("SDIO WRITE");
        goto out;
    }
    int64_t t0 = esp_timer_get_time();
    r          = AT_TIMEOUT;
    while (r == AT_TIMEOUT && esp_timer_get_time() - t0 < 600 * 1000000LL) {
        long long secs = (esp_timer_get_time() - t0) / 1000000;
        s_quiet        = secs % 15 != 0; /* log every 15 s, show every second */
        status("DOWNLOADING AND INSTALLING (%llds)", secs);
        s_quiet = false;
        r       = at_wait(1000, false);
    }
    if (r != AT_OK) {
        fail(r == AT_TIMEOUT ? "DOWNLOAD TIMED OUT" : "THE RADIO COULD NOT DOWNLOAD OR INSTALL IT");
        goto out;
    }

    status("INSTALLED. RESTARTING THE RADIO");
    at_cmd("AT+RST", 2000);
    vTaskDelay(pdMS_TO_TICKS(3000));
    status("DONE. RESTARTING THE DECK");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();

out:
    memset(a, 0, sizeof(*a));
    free(a);
    vTaskDelete(NULL);
}

void tdp4_c6_repair_start(const char *ssid, const char *pass, const char *url)
{
    repair_args_t *a = calloc(1, sizeof(*a));
    if (a == NULL) return;
    snprintf(a->ssid, sizeof(a->ssid), "%s", ssid);
    snprintf(a->pass, sizeof(a->pass), "%s", pass);
    snprintf(a->url, sizeof(a->url), "%s", url);
    status("STARTING");
    if (xTaskCreatePinnedToCore(repair_task, "c6fix", 6144, a, 4, NULL, 0) != pdPASS) {
        free(a);
        fail("NO MEMORY");
    }
}
