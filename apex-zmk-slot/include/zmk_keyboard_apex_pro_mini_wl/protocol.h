/* SPDX-License-Identifier: MIT */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

enum apex_stm32_command {
  APEX_STM32_CMD_SET_MODE = 0x20,
  APEX_STM32_CMD_CONFIG_PRIMARY = 0x30,
  APEX_STM32_CMD_CONFIG_SECONDARY = 0x33,
  APEX_STM32_CMD_KEY_MASK = 0x34,
  APEX_STM32_CMD_STATUS = 0xA0,
  APEX_STM32_CMD_FETCH_EVENT = 0xA1,
};

enum {
  APEX_STM32_FRAME_SIZE = 64,
  APEX_STM32_EVENT_SIZE = 19,
  APEX_STM32_KEY_COUNT = 70,
  APEX_STM32_KEY_BITMAP_SIZE = 9,
};

/**
 * Feed a decoded raw 70-position bitmap into the capture-backed kscan bridge.
 *
 * This function never performs I/O. A later, separately reviewed transport
 * implementation can call it after dequeuing an A1 event. The bridge remaps
 * STM scan bits to the physical 60% ANSI matrix and ignores unused positions.
 */
int apex_stm32_kscan_ingest_bitmap(const struct device *dev,
                                   const uint8_t *bitmap, size_t bitmap_size);

#ifdef __cplusplus
}
#endif
