/*
 * Board-independent deck_hal pieces (hal_common.c), shared by the board
 * files. Not part of the public HAL API.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Open the (encrypted when possible) settings store. Call first. */
void hal_common_nvs_init(void);
/* Apply a timezone index (falls back to the default if out of range). */
void hal_common_tz_apply(int index);
/* Mount the internal FAT partition used when there is no SD card. */
void hal_common_flash_fs_mount(void);
/* Tell the storage helpers whether the SD card mounted at HAL_SD_MOUNT. */
void hal_common_set_sd_mounted(bool mounted);
/* Board label for SYSTEM (e.g. "TAB5 / ST7123"); also fills in the chip. */
void hal_common_set_board_name(const char *name);

#ifdef __cplusplus
}
#endif
