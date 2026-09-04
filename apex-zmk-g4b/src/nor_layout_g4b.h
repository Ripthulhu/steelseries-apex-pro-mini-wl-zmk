/* SPDX-License-Identifier: MIT */
#ifndef APEX_G4B_NOR_LAYOUT_H
#define APEX_G4B_NOR_LAYOUT_H

/* Shared layout for app-side raw access to the 1 MiB FM25Q08A.
 * Devicetree owns the two mounted partitions; these constants name the same
 * ranges so overlap assertions and raw clients stay in sync with it.
 */
#define G4B_NOR_TOTAL_SIZE          0x100000u
#define G4B_NOR_SECTOR_SIZE         0x001000u

#define G4B_NOR_NVS_ADDR            0x060000u
#define G4B_NOR_NVS_SIZE            0x008000u
#define G4B_NOR_NVS_MARK_ADDR       0x068000u
#define G4B_NOR_AB_HEADER_ADDR      0x069000u
#define G4B_NOR_AB_TALLY_ADDR       0x06A000u
#define G4B_NOR_LFS_ADDR            0x06B000u
#define G4B_NOR_LFS_SIZE            0x015000u

#define G4B_NOR_RAW_TEST_ADDR       0x080000u
#define G4B_NOR_FLASHDEV_TEST_ADDR  0x081000u
#define G4B_NOR_FREE_DATA_ADDR      0x082000u
#define G4B_NOR_FREE_DATA_SIZE      0x009000u

/* Layout version 2 makes B exactly as large as the internal code partition. */
#define G4B_NOR_AB_IMAGE_ADDR       0x08B000u
#define G4B_NOR_AB_IMAGE_SIZE       0x071000u
#define G4B_NOR_COREDUMP_ADDR       0x0FC000u
#define G4B_NOR_COREDUMP_SIZE       0x004000u

#define G4B_INTERNAL_APP_ADDR       0x001000u
#define G4B_INTERNAL_APP_SIZE       0x071000u

#endif /* APEX_G4B_NOR_LAYOUT_H */
