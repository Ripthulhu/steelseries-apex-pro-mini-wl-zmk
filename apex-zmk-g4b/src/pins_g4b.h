/* SPDX-License-Identifier: MIT
 *
 * The single choke point for every GPIO access in the G4B payload.
 *
 * No other G4B source file may touch P0/P1 OUT, OUTSET, OUTCLR, DIR or PIN_CNF.
 * verify_g4b.py greps every source except pins_g4b.c for those names and fails
 * on any hit. The reason is narrow and specific: on this board a mis-driven pin
 * is not a bug that shows up as a wrong answer, it is a bug that shows up as
 * silence, and silence here is indistinguishable from "the STM32 never came up".
 *
 * P0.18 is nRESET on the nRF52833. It is deliberately absent from the enum
 * below so it is not expressible, and link_g4b.c additionally asserts at
 * runtime that it is not configured as an output.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

enum g4b_port {
    G4B_PORT0 = 0,
    G4B_PORT1 = 1,
};

/* Pin roles, established from the stock firmware and the SPIM trace.
 *
 * P0.02 and P1.05 are STATIC ENABLES driven HIGH once at bring-up. They are not
 * per-frame strobes and there is no chip select on this link at all
 * (PSEL.CSN = 0xFFFFFFFF, nrfx ss_pin = 0xFF).
 *
 * P0.05 is the per-phase transfer-ready line from the STM32 (GPIOTE ch0,
 * LoToHi). P0.24 separately signals that an A1 event is queued.
 */
enum g4b_pin {
    G4B_P0_EN    = 2,  /* STM32 enable, port 0. HIGH when enabled. */
    G4B_P0_SCK   = 4,
    G4B_P0_READY = 5,  /* transfer-ready in, GPIOTE ch0 LoToHi */
    G4B_P0_MOSI  = 6,
    G4B_P0_MISO  = 7,
    G4B_P0_UART  = 10, /* wrapper's diagnostic UART TX; do not reconfigure */
    G4B_P0_ATTN  = 24, /* "A1 event queued" in, level-polled */
    G4B_P0_TWI_SCL = 16, /* BQ25895 charger bus, from stock's nrfx config */
    G4B_P0_TWI_SDA = 17,
    G4B_P1_EN    = 5,  /* STM32 enable, port 1. HIGH when enabled. */
};

/* Guard against the one confusion this enum cannot express away: P0_EN and
 * P1_EN are different pins on different ports, and P0_READY shares a number
 * with P1_EN.
 */
BUILD_ASSERT((int)G4B_P0_READY == (int)G4B_P1_EN,
             "P0.05 and P1.05 share a pin number - every call must pass a port");

/* PIN_CNF field encodings. PULL is bits[3:2]: 0=disabled, 1=pulldown, 3=pullup.
 * Written out because an inverted pull encoding would silently invert the
 * floating-line test that stage 0 exists to perform.
 */
#define G4B_PINCNF_DIR_INPUT    0x00000000u
#define G4B_PINCNF_DIR_OUTPUT   0x00000001u
#define G4B_PINCNF_INBUF_CONN   0x00000000u
#define G4B_PINCNF_INBUF_DISC   0x00000002u
#define G4B_PINCNF_PULL_NONE    0x00000000u
#define G4B_PINCNF_PULL_DOWN    0x00000004u /* PULL=1 */
#define G4B_PINCNF_PULL_UP      0x0000000Cu /* PULL=3 */

/* Register-exact target states, from the stock snapshot. */
#define G4B_CNF_EN_OUT   (G4B_PINCNF_DIR_OUTPUT | G4B_PINCNF_INBUF_DISC)  /* 0x03 */
#define G4B_CNF_SCK_OUT  (G4B_PINCNF_DIR_OUTPUT | G4B_PINCNF_INBUF_CONN)  /* 0x01 */
#define G4B_CNF_MOSI_OUT (G4B_PINCNF_DIR_OUTPUT | G4B_PINCNF_INBUF_DISC)  /* 0x03 */
#define G4B_CNF_MISO_IN  (G4B_PINCNF_DIR_INPUT  | G4B_PINCNF_PULL_DOWN)   /* 0x04 */
#define G4B_CNF_IN_NOPULL (G4B_PINCNF_DIR_INPUT | G4B_PINCNF_PULL_NONE)   /* 0x00 */

void g4b_pin_cfg(enum g4b_port port, enum g4b_pin pin, uint32_t cnf);
void g4b_pin_set(enum g4b_port port, enum g4b_pin pin);
void g4b_pin_clr(enum g4b_port port, enum g4b_pin pin);
bool g4b_pin_read(enum g4b_port port, enum g4b_pin pin);

uint32_t g4b_port_in(enum g4b_port port);
uint32_t g4b_port_dir(enum g4b_port port);

/* RGB chip-select on P0.11 (SPIM2 IS31FL3743B). CS bit-bang lives here so
 * pins_g4b.c stays the only file that writes GPIO registers.
 */
void g4b_rgb_cs_init(void);
void g4b_rgb_cs_park(void);
void g4b_rgb_cs_low(void);
void g4b_rgb_cs_high(void);
uint32_t g4b_pin_cnf_read(enum g4b_port port, enum g4b_pin pin);

/* Read-only survey of candidate pins: pull each up, read, pull down, read,
 * then leave it as a plain input. Never drives a pin, never sets DIR to output.
 * A pin whose level follows the pull in both directions has nothing else on it
 * and is a candidate for reuse (I2C, for instance). P0.18 is nRESET and is
 * masked out internally whatever the caller passes.
 */
/* Claim and release the charger bus pins.
 *
 * Here rather than in twi_g4b.c because this file is the sole owner of the
 * GPIO registers - verify_g4b.py fails the build if anything else touches
 * them, and that check is worth more than the convenience of configuring a
 * pin next to the peripheral that uses it.
 *
 * Claimed as open-drain with a pull-up, which is what an I2C bus requires and
 * what stock's driver writes. A push-pull output here would be a short against
 * the charger pulling the line low.
 */
void g4b_twi_pins_claim(void);
void g4b_twi_pins_release(void);

/* The LED rails, P0.19 and P0.23, are raised and lowered in stock's order.
 * Stock lowers them on every idle sleep. The controller loses its register state
 * while P0.19 is down, so g4b_rgb_rail_up() must be followed by a full
 * controller bring-up - see g4b_rgb_bringup() in rgb_g4b.c. Callers on the g4b
 * thread only, like everything else that ends in a SPIM2 transfer.
 */
/* Was the P1.07 boot strap pulled low at boot?
 *
 * Latched once during init, so it cannot flap and cannot be re-read. An
 * unconnected pad reads HIGH through the internal pull-up, i.e. "boot
 * normally" - a missing strap can never force recovery, only a deliberate
 * short can.
 */
bool g4b_strap_asserted(void);

void g4b_rgb_rail_up(void);
void g4b_rgb_rail_down(void);

void g4b_pin_survey(uint32_t p0_mask, uint32_t p1_mask,
                    uint32_t *p0_up, uint32_t *p1_up,
                    uint32_t *p0_down, uint32_t *p1_down);

/* GPIOTE channel 0 as stock configures it: Event, PSEL = P0.05, LoToHi. */
void g4b_gpiote_ready_configure(void);
void g4b_gpiote_ready_clear(void);
bool g4b_gpiote_ready_event(void);
uint32_t g4b_gpiote_config0(void);

/* GPIOTE channel 1 on the STM32 attention line (P0.24), interrupt-driven: the
 * ISR gives a semaphore on each rising edge (a queued key event). g4b_attn_wait()
 * blocks the scan loop until an edge OR timeout_ms elapses, so a fresh keypress
 * wakes it immediately instead of at the next poll tick. Call configure() once
 * before the loop; wait() replaces the loop's idle k_msleep. */
void g4b_gpiote_attn_configure(void);
int g4b_attn_wait(uint32_t timeout_ms);
#if (defined(CONFIG_APEX_G4B_STM32_STOP1_IDLE_MS) && \
     CONFIG_APEX_G4B_STM32_STOP1_IDLE_MS > 0) || \
    (defined(CONFIG_APEX_G4B_STM32_IDLE_SCAN_PERIOD_MS) && \
     CONFIG_APEX_G4B_STM32_IDLE_SCAN_PERIOD_MS > 1)
void g4b_attn_drain(void);
#endif

/* Number of queued-report ATTN interrupts since boot. Used to distinguish the
 * interrupt path from the timeout fallback. */
uint32_t g4b_attn_isr_fires(void);

/* Drive the sixteen pins the survey proved free, each at its own duty cycle, so
 * a multimeter on DC identifies a pad by voltage. init() makes them outputs;
 * tick() advances one step and wants calling every millisecond. See the table
 * in pins_g4b.c - 194 mV between neighbours.
 *
 * Only ever the pins that followed an internal pull in BOTH directions. The six
 * that are externally held are excluded and must stay excluded.
 */
void g4b_pin_beacon_init(void);
void g4b_pin_beacon_tick(void);
