/* SPDX-License-Identifier: MIT
 *
 * Board-specific bootloader config for the Apex Pro Mini WL 2.4 GHz dongle
 * (nRF52833). Unlike the keyboard, the dongle needs no pre-USB board bring-up:
 * D+/D- go straight to the nRF52833 USB pins with no external data-path mux, so
 * board_init2()/board_teardown2() are left as the framework's empty weak
 * defaults (see src/boards/boards.c) and USB enumerates on its own.
 *
 * This file exists only to provide the CF2 bootloaderConfig block that
 * ghostfat/self-update read (bootloaderConfig[]). No RGB indicator, no SPIM.
 */
#include "boards.h"
#include "board.h"
#include "uf2/configkeys.h"

__attribute__((used, section(".bootloaderConfig")))
const uint32_t bootloaderConfig[] =
{
  /* CF2 START */
  CFG_MAGIC0, CFG_MAGIC1,                       // magic
  5, 100,                                       // used entries, total entries

  204, 0x80000,                                 // FLASH_BYTES  (nRF52833 512 KB)
  205, 0x20000,                                 // RAM_BYTES    (nRF52833 128 KB)
  208, (USB_DESC_VID << 16) | USB_DESC_UF2_PID, // BOOTLOADER_BOARD_ID = USB VID+PID
  209, 0x621e937a,                              // UF2_FAMILY (metadata; write
                                                //   acceptance uses the board id
                                                //   (VID<<16)|PID = 0x1d506170)
  210, 0x20,                                    // PINS_PORT_SIZE = PA_32

  0, 0, 0, 0, 0, 0, 0, 0
  /* CF2 END */
};
