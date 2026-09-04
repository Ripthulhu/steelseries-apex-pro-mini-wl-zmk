/* SPDX-License-Identifier: MIT */
#ifndef APEX_G4B_AB_ROLLBACK_H
#define APEX_G4B_AB_ROLLBACK_H

#include <stdbool.h>
#include <stdint.h>

/* App-side half of A/B auto-rollback.
 *
 * Every flash access here is on the EXTERNAL FM25Q08A SPI-NOR, through the
 * g4b_spinor_dev_* device ops (flash device backing, spinor_g4b.c) - so it
 * inherits their bus-wide g4b_extbus lock, the boot-replay gate and the
 * open/close-per-op discipline. Internal flash writes are confined to the
 * bootloader implementation in bootloader/apex_pro_mini_wl/ab_promote.c. */

/* Append one 0x00 to the NOR tally sector. NOR errors leave the tally unchanged. */
void g4b_ab_boot_pending(void);

/* The app reached HEALTHY: erase the tally sector so the bootloader sees zero
 * unhealthy boots. Latched to run at most once per boot. Costs one 4 KiB NOR
 * erase (~tens of ms, one-shot) on the calling thread. */
void g4b_ab_mark_healthy(void);

/* Stage image B through updater vendor HID. Commit its descriptor last. */
bool g4b_ab_stage_erase(void);
bool g4b_ab_stage_write(uint32_t off, const uint8_t *data, uint32_t len);
bool g4b_ab_commit(uint32_t b_len);
bool g4b_ab_disarm(void);

#endif /* APEX_G4B_AB_ROLLBACK_H */
