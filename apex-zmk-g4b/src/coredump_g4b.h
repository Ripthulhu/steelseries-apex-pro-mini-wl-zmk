/* SPDX-License-Identifier: MIT
 *
 * Crash/coredump capture to the external SPI-NOR (CONFIG_APEX_G4B_COREDUMP).
 *
 * On a CPU fault this overrides Zephyr's weak k_sys_fatal_error_handler(),
 * snapshots the exception frame (R0-R3, R12, LR, PC, xPSR), the callee-saved
 * bank (R4-R11, requires CONFIG_EXTRA_EXCEPTION_INFO), the SCB fault-status
 * registers (CFSR/HFSR/BFAR/MMFAR read direct), RESETREAS, and a short slice of
 * the faulting stack into a fixed-size record, writes it to a small ring in the
 * NOR free tail, and resets. The last record is read back and emitted over the
 * evidence/USB-CDC channel on the next boot for host-side decode.
 *
 * The write runs in FAULT CONTEXT, so it is LOCK-FREE by construction: it never
 * touches the g4b_extbus mutex or the replay gate (both illegal from an
 * exception). See g4b_spinor_fault_write() in spinor_g4b.c.
 */

#ifndef APEX_G4B_COREDUMP_H
#define APEX_G4B_COREDUMP_H

#include <stdbool.h>
#include <stdint.h>

#include "nor_layout_g4b.h"

/* "APXC" little-endian. Distinct from APXG(0x47585041)/APXB/APXD/APEX so the
 * host decoder and a raw capture both tell a coredump record from everything
 * else on the same wire. */
#define G4B_COREDUMP_MAGIC   0x43585041u
#define G4B_COREDUMP_VERSION 1u

/* Words of faulting-stack copied into each record. 32 x 4 = 128 bytes, which is
 * enough to see the exception frame plus a few callers and keeps the whole
 * record inside a single 256-byte page-program (one NOR write, no page split in
 * fault context). */
#define G4B_COREDUMP_STACK_WORDS 32u

/* The ring lives in the TOP four sectors of the 1 MiB device (0xFC000..0x100000),
 * deliberately isolated at the very top so it can never collide with the other
 * NOR-tail users - LittleFS (0x6B000..0x80000) and the A/B image-B stage
 * (0x8B000..0xFC000) - per the reconciled NOR-tail memory map. One record per
 * 4 KiB sector so a capture is a single erase + single page-program; the ring
 * round-robins, overwriting the oldest slot. BUILD_ASSERTs in coredump_g4b.c pin
 * the base at/above the free floor and the top inside the 1 MiB device, clear of
 * the ext NVS (0x60000) and its provision marker (0x68000). */
#define G4B_COREDUMP_RING_BASE  G4B_NOR_COREDUMP_ADDR
#define G4B_COREDUMP_RING_SLOTS 4u
#define G4B_COREDUMP_SLOT_SIZE  G4B_NOR_SECTOR_SIZE

/* Fixed layout, little-endian, written raw. 248 bytes (<= 256). The host
 * decoder derives the same layout; a size change here must be matched there. */
struct g4b_coredump_record {
    uint32_t magic;
    uint16_t version;
    uint16_t seq;          /* monotonically increasing; newest wins */

    uint32_t reason;       /* enum k_fatal_error_reason */
    uint32_t resetreas;    /* POWER->RESETREAS as read in the handler */

    /* Hardware-stacked basic exception frame. */
    uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr;

    /* Callee-saved bank (R4-R11). Zero when CONFIG_EXTRA_EXCEPTION_INFO is off
     * or the callee pointer was unavailable. */
    uint32_t r4, r5, r6, r7, r8, r9, r10, r11;

    /* SCB fault status, read direct in the handler. */
    uint32_t cfsr;         /* 0xE000ED28 */
    uint32_t hfsr;         /* 0xE000ED2C */
    uint32_t bfar;         /* 0xE000ED38 */
    uint32_t mmfar;        /* 0xE000ED34 */

    /* Stack pointers. sp is the ESF address (the pre-fault SP baseline the
     * stack slice is copied from). */
    uint32_t sp;
    uint32_t msp;
    uint32_t psp;
    uint32_t exc_return;

    uint32_t stack_valid;  /* 1 iff the slice below is a real RAM copy */
    uint32_t stack[G4B_COREDUMP_STACK_WORDS];

    uint32_t crc32;        /* over every byte before this one */
} __packed;

/* Emit the most-recent valid record over the evidence channel, once, if there
 * is one. Called from a delayed boot thread so the USB CDC has enumerated. */
void g4b_coredump_emit_last(void);

#endif /* APEX_G4B_COREDUMP_H */
