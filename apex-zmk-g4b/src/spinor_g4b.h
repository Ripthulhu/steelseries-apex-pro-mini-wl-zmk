/* SPDX-License-Identifier: MIT */
#ifndef APEX_G4B_SPINOR_H
#define APEX_G4B_SPINOR_H

#include <stdbool.h>
#include <stdint.h>

/* One-shot read-only dump of the external FM25Q08A SPI-NOR (1 MiB), framed
 * through the evidence channel. This path issues no erase or program commands. */
void g4b_spinor_dump(void);

/* Prove the SPIM0 write path on a spare, blank region of the external flash
 * (touches no config, profiles, or the staged firmware) and report the result
 * over UART. See spinor_g4b.c and CONFIG_APEX_G4B_SPINOR_WRITETEST.
 */
void g4b_spinor_write_test(void);

/* Legacy vendor-loader staging primitives (fs=3/file=11 at external 0x014000).
 * Open the
 * bus, erase just the staged region, stream the new vendor.bin in with
 * page-boundary splitting, optionally read it back to verify, then close. The
 * SteelSeries loader applies it to the live app slot on the next reset. Drive
 * these from a single owner (the updater thread). Compiled only when
 * CONFIG_APEX_G4B_SPINOR_WRITE is set.
 */
void g4b_spinor_open(void);
void g4b_spinor_close(void);
bool g4b_spinor_stage_erase(void);
bool g4b_spinor_stage_write(uint32_t off, const uint8_t *data, uint32_t len);
bool g4b_spinor_stage_read(uint32_t off, uint8_t *out, uint32_t len);

/* Generic whole-device flash backing for the Zephyr flash driver
 * (flash_spinor_g4b.c, CONFIG_APEX_G4B_SPINOR_FLASHDEV). Absolute offsets in the
 * 1 MiB device; 4 KiB erase, 256 B page program. Return 0 or -errno. */
int g4b_spinor_dev_init(void);
int g4b_spinor_dev_read(uint32_t addr, void *out, uint32_t len);
int g4b_spinor_dev_erase(uint32_t addr, uint32_t size);
int g4b_spinor_dev_program(uint32_t addr, const void *data, uint32_t len);

/* Crash/coredump ring backing (CONFIG_APEX_G4B_COREDUMP, src/coredump_g4b.c).
 *
 * g4b_spinor_fault_write() runs in FAULT CONTEXT and is LOCK-FREE: it drives
 * SPIM0 directly through the static nor_* primitives and NEVER takes the
 * g4b_extbus mutex or the replay gate (both are k_mutex/sleep operations and
 * are illegal from an exception). It PREFLIGHTS the WIP bit first, because an
 * NVS program/erase on the same chip may have been mid-flight when the fault
 * hit. It erases the 4 KiB slot then page-programs the record (len <= 256).
 * Returns true on a clean erase+program.
 *
 * g4b_spinor_coredump_read() is the boot-time counterpart: a normal, LOCKED
 * read (the g4b_extbus lock is a no-op inline unless FLASHDEV is on) used to
 * scan the ring and read back the last record. Never call it from a fault. */
bool g4b_spinor_fault_write(uint32_t addr, const uint8_t *rec, uint32_t len);
bool g4b_spinor_coredump_read(uint32_t addr, uint8_t *out, uint32_t len);

#endif /* APEX_G4B_SPINOR_H */
