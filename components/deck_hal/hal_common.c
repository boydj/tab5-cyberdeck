/*
 * deck_hal parts that do not depend on the board: encrypted settings (NVS),
 * timezones, the internal-flash storage fallback and system info. Each board
 * file (hal_tab5.cpp, hal_tdisplay_p4.c) owns the hardware and calls into
 * these through hal_common.h.
 */
#include "deck_hal.h"
#include "deck_tz.h"
#include "hal_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_chip_info.h"
#include "esp_efuse.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "nvs_sec_provider.h"

static const char *TAG = "hal";

static nvs_handle_t s_nvs;
static bool s_nvs_ok;
static bool s_nvs_encrypted;
static bool s_sd_ok;
static char s_board_name[32] = "UNKNOWN";
static char s_chip_name[32];

void hal_common_set_sd_mounted(bool mounted) { s_sd_ok = mounted; }

void hal_common_set_board_name(const char *name)
{
    snprintf(s_board_name, sizeof(s_board_name), "%s", name);
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    snprintf(s_chip_name, sizeof(s_chip_name), "ESP32-P4 rev%d.%d x%d", ci.revision / 100, ci.revision % 100,
             ci.cores);
}

/* ---- Timezone ------------------------------------------------------------ */

static int s_tz = DECK_TZ_DEFAULT;

void hal_common_tz_apply(int index)
{
    s_tz = (index >= 0 && index < DECK_TZ_COUNT) ? index : DECK_TZ_DEFAULT;
    setenv("TZ", g_deck_tz[s_tz].posix, 1);
    tzset();
}

int hal_tz_count(void) { return DECK_TZ_COUNT; }
const char *hal_tz_name(int index) { return (index >= 0 && index < DECK_TZ_COUNT) ? g_deck_tz[index].name : "?"; }
int hal_tz_get(void) { return s_tz; }

void hal_tz_set(int index)
{
    time_t now = time(NULL);
    hal_common_tz_apply(index);
    hal_cfg_set_i32("tz", s_tz);
    struct tm local;
    localtime_r(&now, &local);
    hal_rtc_set(&local); /* same instant, new wall time */
}

/* ---- Storage fallback ---------------------------------------------------- */

/* Internal flash fallback so notes work without a card. */
#define FLASH_MOUNT "/flash"
static bool s_flash_ok;
static wl_handle_t s_wl = WL_INVALID_HANDLE;

void hal_common_flash_fs_mount(void)
{
    esp_vfs_fat_mount_config_t mcfg = {};
    mcfg.format_if_mount_failed      = true; /* first boot after the v0.2 partition change */
    mcfg.max_files                   = 4;
    mcfg.allocation_unit_size        = 4096;
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(FLASH_MOUNT, "storage", &mcfg, &s_wl);
    s_flash_ok    = err == ESP_OK;
    if (!s_flash_ok) ESP_LOGW(TAG, "internal storage not mounted (%s)", esp_err_to_name(err));
}

const char *hal_storage_root(void)
{
    if (s_sd_ok) return HAL_SD_MOUNT;
    if (s_flash_ok) return FLASH_MOUNT;
    return NULL;
}

bool hal_storage_is_sd(void) { return s_sd_ok; }

bool hal_sd_mounted(void) { return s_sd_ok; }

uint64_t hal_sd_total_bytes(void)
{
    uint64_t total = 0, free_b = 0;
    if (s_sd_ok) esp_vfs_fat_info(HAL_SD_MOUNT, &total, &free_b);
    return total;
}

uint64_t hal_sd_free_bytes(void)
{
    uint64_t total = 0, free_b = 0;
    if (s_sd_ok) esp_vfs_fat_info(HAL_SD_MOUNT, &total, &free_b);
    return free_b;
}

/* ---- Settings ------------------------------------------------------------ */

/* Settings live in the "deck" namespace of the default NVS partition.
 *
 * NVS is encrypted with XTS keys derived (by the HMAC peripheral) from a key
 * in an eFuse key block. On the first boot of a build with this code there is
 * no such key: the plain settings are copied to RAM, a random HMAC key is
 * burned into the first free key block, the partition is erased, re-opened
 * encrypted and the settings are written back. That burn is the only eFuse
 * change and it does not restrict flashing. With CONFIG_DECK_NVS_KEY_BURN
 * off nothing is burned: a key burned earlier is still used, otherwise the
 * settings stay plain.
 *
 * Anything going wrong on that path falls back to plain NVS, so settings may
 * be lost but the deck always boots. */

#define NVS_PART "nvs"
#ifdef CONFIG_DECK_NVS_KEY_BURN
#define KEY_BURN true
#else
#define KEY_BURN false
#endif
#define MIGRATE_MAX 48

typedef struct {
    char key[NVS_KEY_NAME_MAX_SIZE];
    nvs_type_t type;
    int32_t i32;
    void *data; /* str (with NUL) or blob */
    size_t len;
} kv_t;

static esp_err_t plain_open(void)
{
    esp_err_t err = nvs_flash_init_partition(NVS_PART);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase_partition(NVS_PART);
        err = nvs_flash_init_partition(NVS_PART);
    }
    return err;
}

static esp_err_t secure_open(nvs_sec_cfg_t *cfg)
{
    esp_err_t err = nvs_flash_secure_init_partition(NVS_PART, cfg);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase_partition(NVS_PART);
        err = nvs_flash_secure_init_partition(NVS_PART, cfg);
    }
    return err;
}

/* Copy the plain "deck" namespace (already initialised) into RAM. */
static int dump_settings(kv_t *kv, int max)
{
    int n = 0;
    nvs_handle_t h;
    if (nvs_open_from_partition(NVS_PART, "deck", NVS_READONLY, &h) != ESP_OK) return 0;
    nvs_iterator_t it = NULL;
    esp_err_t err     = nvs_entry_find(NVS_PART, "deck", NVS_TYPE_ANY, &it);
    while (err == ESP_OK && n < max) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        kv_t *e = &kv[n];
        memset(e, 0, sizeof(*e));
        snprintf(e->key, sizeof(e->key), "%s", info.key);
        e->type = info.type;
        bool ok = false;
        if (info.type == NVS_TYPE_I32) {
            ok = nvs_get_i32(h, info.key, &e->i32) == ESP_OK;
        } else if (info.type == NVS_TYPE_STR || info.type == NVS_TYPE_BLOB) {
            bool str = info.type == NVS_TYPE_STR;
            if ((str ? nvs_get_str(h, info.key, NULL, &e->len) : nvs_get_blob(h, info.key, NULL, &e->len)) == ESP_OK &&
                (e->data = malloc(e->len ? e->len : 1)) != NULL) {
                ok = (str ? nvs_get_str(h, info.key, (char *)e->data, &e->len)
                          : nvs_get_blob(h, info.key, e->data, &e->len)) == ESP_OK;
            }
        }
        if (ok) {
            n++;
        } else {
            free(e->data);
            ESP_LOGW("hal", "settings migration: skipped %s", info.key);
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    nvs_close(h);
    return n;
}

static void restore_settings(kv_t *kv, int n)
{
    for (int i = 0; i < n; i++) {
        kv_t *e = &kv[i];
        if (e->type == NVS_TYPE_I32) nvs_set_i32(s_nvs, e->key, e->i32);
        if (e->type == NVS_TYPE_STR) nvs_set_str(s_nvs, e->key, (const char *)e->data);
        if (e->type == NVS_TYPE_BLOB) nvs_set_blob(s_nvs, e->key, e->data, e->len);
    }
    nvs_commit(s_nvs);
}

static void wipe_settings(kv_t *kv, int n)
{
    for (int i = 0; i < n; i++) {
        if (kv[i].data) {
            memset(kv[i].data, 0, kv[i].len); /* Wi-Fi password, SSH key */
            free(kv[i].data);
        }
    }
    memset(kv, 0, sizeof(kv_t) * (size_t)n);
}

/* Derive the XTS keys from the HMAC key in `blk`; with `generate`, burn a new
 * random HMAC key into it first (it must be unused). */
static esp_err_t sec_cfg(esp_efuse_block_t blk, bool generate, nvs_sec_cfg_t *cfg)
{
    nvs_sec_config_hmac_t hcfg = {};
    hcfg.hmac_key_id           = (hmac_key_id_t)(blk - EFUSE_BLK_KEY0);
    nvs_sec_scheme_t *scheme   = NULL;
    esp_err_t err              = nvs_sec_provider_register_hmac(&hcfg, &scheme);
    if (err != ESP_OK) return err;
    err = generate ? nvs_flash_generate_keys_v2(scheme, cfg) : nvs_flash_read_security_cfg_v2(scheme, cfg);
    return err;
}

void hal_common_nvs_init(void)
{
    static kv_t kv[MIGRATE_MAX];
    int migrated = -1; /* entries waiting to be written back, or -1 */
    nvs_sec_cfg_t cfg  = {};
    esp_err_t err      = ESP_FAIL;
    esp_efuse_block_t blk;

    if (esp_efuse_find_purpose(ESP_EFUSE_KEY_PURPOSE_HMAC_UP, &blk)) {
        /* Normal boot: the key exists. */
        if (sec_cfg(blk, false, &cfg) == ESP_OK) err = secure_open(&cfg);
        if (err != ESP_OK) ESP_LOGE("hal", "encrypted settings failed (0x%x), using plain NVS", err);
    } else if (!KEY_BURN) {
        ESP_LOGI("hal", "settings stay unencrypted (eFuse key burn disabled in this build)");
    } else if ((blk = esp_efuse_find_unused_key_block()) != EFUSE_BLK_KEY_MAX) {
        /* First boot with encryption: migrate. */
        int n = 0;
        if (plain_open() == ESP_OK) {
            n = dump_settings(kv, MIGRATE_MAX);
            nvs_flash_deinit_partition(NVS_PART);
        }
        ESP_LOGW("hal", "encrypting settings: burning an HMAC key into eFuse key block %d", (int)(blk - EFUSE_BLK_KEY0));
        if (sec_cfg(blk, true, &cfg) == ESP_OK) {
            nvs_flash_erase_partition(NVS_PART);
            err = secure_open(&cfg);
        } else {
            ESP_LOGE("hal", "could not create the settings key, keeping plain NVS");
        }
        migrated = n;
    } else {
        ESP_LOGW("hal", "no free eFuse key block, settings stay unencrypted");
    }

    s_nvs_encrypted = err == ESP_OK;
    if (err != ESP_OK) err = plain_open(); /* never fail to boot over settings */
    s_nvs_ok = (err == ESP_OK) && nvs_open_from_partition(NVS_PART, "deck", NVS_READWRITE, &s_nvs) == ESP_OK;

    if (migrated >= 0) {
        /* If the burn failed the plain partition was never erased, and the
         * values are already there; writing them again is harmless. */
        if (s_nvs_ok) restore_settings(kv, migrated);
        ESP_LOGI("hal", "settings migrated (%d entries, %s)", migrated, s_nvs_encrypted ? "encrypted" : "plain");
        wipe_settings(kv, migrated);
    } else if (s_nvs_encrypted) {
        ESP_LOGI("hal", "settings encrypted (eFuse key block %d)", (int)(blk - EFUSE_BLK_KEY0));
    }
}

bool hal_cfg_encrypted(void) { return s_nvs_encrypted; }

bool hal_cfg_get_blob(const char *key, void *out, size_t *len)
{
    return s_nvs_ok && nvs_get_blob(s_nvs, key, out, len) == ESP_OK;
}

void hal_cfg_set_blob(const char *key, const void *data, size_t len)
{
    if (!s_nvs_ok) return;
    if (data) {
        nvs_set_blob(s_nvs, key, data, len);
    } else {
        nvs_erase_key(s_nvs, key);
    }
    nvs_commit(s_nvs);
}

int32_t hal_cfg_get_i32(const char *key, int32_t def)
{
    int32_t v = def;
    if (s_nvs_ok && nvs_get_i32(s_nvs, key, &v) != ESP_OK) v = def;
    return v;
}

void hal_cfg_set_i32(const char *key, int32_t value)
{
    if (!s_nvs_ok) return;
    nvs_set_i32(s_nvs, key, value);
    nvs_commit(s_nvs);
}

bool hal_cfg_get_str(const char *key, char *out, size_t n)
{
    if (n) out[0] = '\0';
    size_t len = n;
    return s_nvs_ok && n && nvs_get_str(s_nvs, key, out, &len) == ESP_OK;
}

void hal_cfg_set_str(const char *key, const char *value)
{
    if (!s_nvs_ok) return;
    if (value) {
        nvs_set_str(s_nvs, key, value);
    } else {
        nvs_erase_key(s_nvs, key);
    }
    nvs_commit(s_nvs);
}

/* ---- System info --------------------------------------------------------- */

void hal_sysinfo(hal_sysinfo_t *out)
{
    out->board          = s_board_name;
    out->chip           = s_chip_name;
    out->cpu_mhz        = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    out->heap_int_free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->heap_int_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    out->psram_free     = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    out->psram_total    = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    out->uptime_s       = (uint32_t)(esp_timer_get_time() / 1000000);
}
