/* SPDX-License-Identifier: MIT */

#define DT_DRV_COMPAT steelseries_apex_stm32_kscan

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/kscan.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk_keyboard_apex_pro_mini_wl/protocol.h>

LOG_MODULE_REGISTER(apex_stm32_kscan, CONFIG_APEX_STM32_KSCAN_LOG_LEVEL);

#define APEX_MATRIX_UNUSED UINT8_MAX

/*
 * Capture-backed mapping from the STM32's LSB-first A1 bitmap to the
 * 5x14 matrix coordinates used by the 60% ANSI ZMK transform. The STM scan
 * order is almost row-major, except for Backspace and the bottom row.
 *
 * Evidence: com5-keymap-row1-retry-20260802.bin,
 *           com5-keymap-row2-20260802.bin,
 *           com5-keymap-row4-row5-20260802.bin
 */
static const uint8_t apex_scan_to_matrix[APEX_STM32_KEY_COUNT] = {
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

BUILD_ASSERT(ARRAY_SIZE(apex_scan_to_matrix) == APEX_STM32_KEY_COUNT);

struct apex_stm32_kscan_config {
  uint8_t rows;
  uint8_t columns;
};

struct apex_stm32_kscan_data {
  kscan_callback_t callback;
  uint8_t previous[APEX_STM32_KEY_BITMAP_SIZE];
  bool enabled;
};

static int apex_stm32_kscan_configure(const struct device *dev,
                                      kscan_callback_t callback) {
  struct apex_stm32_kscan_data *data = dev->data;

  if (callback == NULL) {
    return -EINVAL;
  }

  data->callback = callback;
  return 0;
}

static int apex_stm32_kscan_enable(const struct device *dev) {
  struct apex_stm32_kscan_data *data = dev->data;

  data->enabled = true;
  return 0;
}

static int apex_stm32_kscan_disable(const struct device *dev) {
  struct apex_stm32_kscan_data *data = dev->data;

  data->enabled = false;
  return 0;
}

int apex_stm32_kscan_ingest_bitmap(const struct device *dev,
                                   const uint8_t *bitmap, size_t bitmap_size) {
  const struct apex_stm32_kscan_config *config;
  struct apex_stm32_kscan_data *data;

  if (dev == NULL || bitmap == NULL ||
      bitmap_size != APEX_STM32_KEY_BITMAP_SIZE) {
    return -EINVAL;
  }

  config = dev->config;
  data = dev->data;

  if (!data->enabled || data->callback == NULL) {
    return -EACCES;
  }

  for (uint8_t position = 0; position < APEX_STM32_KEY_COUNT; position++) {
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

static int apex_stm32_kscan_init(const struct device *dev) {
  const struct apex_stm32_kscan_config *config = dev->config;

  if ((uint16_t)config->rows * config->columns != APEX_STM32_KEY_COUNT) {
    return -EINVAL;
  }

  LOG_WRN(
      "STM32 transport is intentionally disabled; keyboard input is inactive");
  return 0;
}

static const struct kscan_driver_api apex_stm32_kscan_api = {
    .config = apex_stm32_kscan_configure,
    .enable_callback = apex_stm32_kscan_enable,
    .disable_callback = apex_stm32_kscan_disable,
};

#define APEX_STM32_KSCAN_DEFINE(inst)                                          \
  static struct apex_stm32_kscan_data apex_stm32_kscan_data_##inst;            \
  static const struct apex_stm32_kscan_config apex_stm32_kscan_config_##inst = \
      {.rows = DT_INST_PROP(inst, rows),                                       \
       .columns = DT_INST_PROP(inst, columns)};                                \
  DEVICE_DT_INST_DEFINE(inst, apex_stm32_kscan_init, NULL,                     \
                        &apex_stm32_kscan_data_##inst,                         \
                        &apex_stm32_kscan_config_##inst, POST_KERNEL,          \
                        CONFIG_KSCAN_INIT_PRIORITY, &apex_stm32_kscan_api);

DT_INST_FOREACH_STATUS_OKAY(APEX_STM32_KSCAN_DEFINE)
