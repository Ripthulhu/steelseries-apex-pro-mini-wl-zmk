/* SPDX-License-Identifier: MIT */
/* Direct USB needs no board initialisation. CF2 supplies the image family
 * and memory sizes used by the bootloader's update handlers. */
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
  209, (USB_DESC_VID << 16) | USB_DESC_UF2_PID, // Application UF2 family
  210, 0x20,                                    // PINS_PORT_SIZE = PA_32

  0, 0, 0, 0, 0, 0, 0, 0
  /* CF2 END */
};
