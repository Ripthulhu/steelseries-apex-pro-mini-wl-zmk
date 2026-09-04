/* SPDX-License-Identifier: MIT
 *
 * G4B kscan bridge.
 *
 * The 70-entry scan map and level-diff ingest match
 * apex-zmk-slot/src/kscan_apex_stm32.c and the capture-derived scanner order.
 * apex_g4b_kscan_ingest_bitmap() bridges absolute STM32 bitmaps into ZMK's
 * matrix callbacks.
 */

#define DT_DRV_COMPAT steelseries_apex_g4b_spim_kscan

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/kscan.h>
#include <zephyr/sys/util.h>

#include "kscan_g4b.h"


#define APEX_MATRIX_UNUSED UINT8_MAX

/*
 * Capture-backed mapping from the STM32's LSB-first A1 bitmap to the
 * 5x14 matrix coordinates used by the 60% ANSI ZMK transform. The STM scan
 * order is almost row-major, except for Backspace and the bottom row.
 *
 * The same mapping is recorded in apex-zmk-slot/scan-map.json and checked by
 * apex-zmk-slot/scripts/verify_scan_map.py.
 */
static const uint8_t apex_scan_to_matrix[APEX_G4B_KEY_COUNT] = {
    /* STM bits  0..13 */
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, APEX_MATRIX_UNUSED,
    /* STM bits 14..27 */
    14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
    /* STM bits 28..41 */
    28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
    APEX_MATRIX_UNUSED, 41,
    /* STM bits 42..55 */
    42, APEX_MATRIX_UNUSED, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52,
    APEX_MATRIX_UNUSED, 55,
    /* STM bits 56..69 */
    56, 57, 58, APEX_MATRIX_UNUSED, 61, APEX_MATRIX_UNUSED,
    APEX_MATRIX_UNUSED, 65, 66, 67, 69, 13, APEX_MATRIX_UNUSED,
    APEX_MATRIX_UNUSED,
};

BUILD_ASSERT(ARRAY_SIZE(apex_scan_to_matrix) == APEX_G4B_KEY_COUNT);

/* The same information as apex_scan_to_matrix's APEX_MATRIX_UNUSED holes, in
 * the shape the 9-byte wire bitmap uses: a 1 bit means "this scan position is
 * mapped to a real key". Unused positions are 13, 40, 43, 54, 59, 61, 62, 68
 * and 69; bits 70 and 71 exist in the bitmap but not in the map at all
 * (APEX_G4B_KEY_COUNT is 70, APEX_G4B_KEY_BITMAP_SIZE is 9 = 72 bits), so they
 * are cleared too. Bit order matches the ingest loop below: bit (n & 7) of
 * byte (n >> 3). Popcount is 61, exactly the number of mapped keys.
 *
 * Hand-written rather than derived, because apex_scan_to_matrix[13] is not an
 * integer constant expression and making it macro-derivable would mean
 * rewriting the one table that decides which physical key emits which keycode.
 *
 * Stock does the same thing: valid_mask@0x20009510, built once at boot from a
 * per-position classification table and AND'd into every 0xA1 read. It is NOT
 * a stuck-key measure and it does not touch / or Z, which are real mapped
 * positions.
 */
static const uint8_t apex_g4b_valid_mask[APEX_G4B_KEY_BITMAP_SIZE] = {
    0xFFu, 0xDFu, 0xFFu, 0xFFu, 0xFFu, 0xF6u, 0xBFu, 0x97u, 0x0Fu,
};

BUILD_ASSERT(ARRAY_SIZE(apex_g4b_valid_mask) == APEX_G4B_KEY_BITMAP_SIZE);

const uint8_t *apex_g4b_kscan_valid_mask(void)
{
    return apex_g4b_valid_mask;
}

struct apex_g4b_kscan_config {
  uint8_t rows;
  uint8_t columns;
};

struct apex_g4b_kscan_data {
  kscan_callback_t callback;
  uint8_t previous[APEX_G4B_KEY_BITMAP_SIZE];
  bool enabled;
};

static int apex_g4b_kscan_configure(const struct device *dev,
                                      kscan_callback_t callback) {
  struct apex_g4b_kscan_data *data = dev->data;

  if (callback == NULL) {
    return -EINVAL;
  }

  data->callback = callback;
  return 0;
}

static int apex_g4b_kscan_enable(const struct device *dev) {
  struct apex_g4b_kscan_data *data = dev->data;

  data->enabled = true;
  return 0;
}

static int apex_g4b_kscan_disable(const struct device *dev) {
  struct apex_g4b_kscan_data *data = dev->data;

  data->enabled = false;
  return 0;
}

int apex_g4b_kscan_ingest_bitmap(const struct device *dev,
                                   const uint8_t *bitmap, size_t bitmap_size) {
  const struct apex_g4b_kscan_config *config;
  struct apex_g4b_kscan_data *data;

  if (dev == NULL || bitmap == NULL ||
      bitmap_size != APEX_G4B_KEY_BITMAP_SIZE) {
    return -EINVAL;
  }

  config = dev->config;
  data = dev->data;

  if (!data->enabled || data->callback == NULL) {
    return -EACCES;
  }

  for (uint8_t position = 0; position < APEX_G4B_KEY_COUNT; position++) {
    const uint8_t matrix_position = apex_scan_to_matrix[position];
    const uint8_t mask = (uint8_t)(1U << (position & 7U));
    const bool was_pressed = (data->previous[position >> 3U] & mask) != 0U;
    const bool is_pressed = (bitmap[position >> 3U] & mask) != 0U;

    if (matrix_position != APEX_MATRIX_UNUSED && was_pressed != is_pressed) {
      const uint32_t row = matrix_position / config->columns;
      const uint32_t column = matrix_position % config->columns;

      data->callback(dev, row, column, is_pressed);
    }
  }

  memcpy(data->previous, bitmap, sizeof(data->previous));
  return 0;
}

static int apex_g4b_kscan_init(const struct device *dev) {
  const struct apex_g4b_kscan_config *config = dev->config;

  if ((uint16_t)config->rows * config->columns != APEX_G4B_KEY_COUNT) {
    return -EINVAL;
  }

  /* Stage 0 never ingests; the driver exists so the kscan node resolves and
   * ZMK boots identically to the passing G4A2 image. CONFIG_LOG is n, so there
   * is nothing to log to.
   */
  return 0;
}

static const struct kscan_driver_api apex_g4b_kscan_api = {
    .config = apex_g4b_kscan_configure,
    .enable_callback = apex_g4b_kscan_enable,
    .disable_callback = apex_g4b_kscan_disable,
};

#define APEX_G4B_KSCAN_DEFINE(inst)                                          \
  static struct apex_g4b_kscan_data apex_g4b_kscan_data_##inst;            \
  static const struct apex_g4b_kscan_config apex_g4b_kscan_config_##inst = \
      {.rows = DT_INST_PROP(inst, rows),                                       \
       .columns = DT_INST_PROP(inst, columns)};                                \
  DEVICE_DT_INST_DEFINE(inst, apex_g4b_kscan_init, NULL,                     \
                        &apex_g4b_kscan_data_##inst,                         \
                        &apex_g4b_kscan_config_##inst, POST_KERNEL,          \
                        CONFIG_KSCAN_INIT_PRIORITY, &apex_g4b_kscan_api);

DT_INST_FOREACH_STATUS_OKAY(APEX_G4B_KSCAN_DEFINE)
