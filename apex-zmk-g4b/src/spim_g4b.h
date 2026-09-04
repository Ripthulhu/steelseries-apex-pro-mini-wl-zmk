/* SPDX-License-Identifier: MIT
 *
 * Direct-register SPIM3 link to the stock STM32.
 *
 * Zephyr's SPI driver is deliberately not used. CONFIG_PINCTRL would apply
 * pinctrl-0 at init priority 50, driving SCK/MOSI/MISO roughly two seconds
 * before the payload's control sequence runs and while the STM32 enable lines
 * are still undriven. Direct registers keep the whole sequence in one place and
 * in stock's exact order.
 *
 * There is no chip select on this link - PSEL.CSN stays disconnected and the
 * framing comes from the P0.05 ready line. The exchange is two half-duplex
 * phases, not one full-duplex transfer: TX 64 bytes with RXD.MAXCNT=0, then RX
 * 64 bytes with TXD.MAXCNT=0.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define G4B_SPIM_FRAME 64

enum g4b_spim_result {
    G4B_SPIM_OK = 0,
    G4B_SPIM_NOT_READY_TX = 1, /* ready line never rose before the TX phase */
    G4B_SPIM_NOT_READY_RX = 2, /* ready line never rose before the RX phase */
    G4B_SPIM_TX_TIMEOUT = 3,   /* EVENTS_END never set for the TX phase */
    G4B_SPIM_RX_TIMEOUT = 4,   /* EVENTS_END never set for the RX phase */
};

struct g4b_exchange_stats {
    uint32_t tx_amount;   /* hardware-written; 64 on a good TX phase */
    uint32_t rx_amount;   /* hardware-written; 64 on a good RX phase */
    uint32_t events_end;  /* EVENTS_END after the RX phase */
    uint32_t result;      /* enum g4b_spim_result */
    uint32_t wait_tx_us;  /* ready wait before the TX phase */
    uint32_t wait_rx_us;  /* ready wait before the RX phase */
};

/* Enable SPIM3 with the register-exact state captured from stock. Must be
 * called only after the control pins are up and the ready line has been seen.
 */
void g4b_spim_enable(void);
void g4b_spim_disable(void);

#if defined(CONFIG_APEX_G4B_STM32_STOP1_IDLE_MS) && \
    CONFIG_APEX_G4B_STM32_STOP1_IDLE_MS > 0
/* Re-synchronize SPIM3 with a still-running scanner using stock's GPIO
 * handshake: disconnect SPIM3, drive MOSI high, wait for READY low, restore
 * SPIM3/MOSI-low, then wait for READY high again. This is not a STOP1 wake;
 * mode 0 removes the READY-low acknowledgement. Both waits are bounded.
 * Returns true only after the complete low -> high acknowledgement.
 */
bool g4b_spim_link_resync(uint32_t *wait_low_us, uint32_t *wait_ready_us);
#endif

/* One logical 64-byte exchange: TX phase, wait ready, RX phase.
 * tx and rx must both be in RAM - EasyDMA cannot read from flash.
 */
enum g4b_spim_result g4b_spim_exchange(const uint8_t *tx, uint8_t *rx,
                                       struct g4b_exchange_stats *stats);

/* Arm the ready latch before an exchange. */
void g4b_spim_arm_ready(void);

/* External-bus arbitration when ZMK NVS lives on the external SPI-NOR.
 *
 * Scanner traffic normally runs only on the g4b thread. External-NOR settings
 * saves add SPIM0 access from the system workqueue and can otherwise interrupt
 * a two-phase SPIM3 exchange or the paced setup replay, desynchronizing the
 * STM32 scanner.
 *
 * Two guards restore the invariant, and BOTH are needed:
 *   - g4b_extbus_lock/unlock: ONE bus-wide K_MUTEX that the whole SPIM3 exchange
 *     AND every SPIM0 NVS op take, so an exchange is indivisible against NVS.
 *     Priority inheritance means a losing NVS caller SLEEPS (does not busy-wait)
 *     and boosts the g4b thread to finish the exchange first - the CPU stays with
 *     the exchange, so the scanner is never stranded. NVS then runs between
 *     frames; a rare ~1 s erase only adds scan latency.
 *   - g4b_extbus_replay_{begin,end}: hold the same mutex across the complete
 *     paced replay. Each exchange takes it recursively, while a flash operation
 *     waits until every replay frame has finished. g4b_extbus_wait_replay lets
 *     writes and erases sleep before they contend for that mutex. Reads are not
 *     gated because settings load runs before the g4b thread starts.
 *
 * When CONFIG_APEX_G4B_SPINOR_FLASHDEV is off, these compile to nothing because
 * no second thread uses the external bus.
 */
#if IS_ENABLED(CONFIG_APEX_G4B_SPINOR_FLASHDEV)
void g4b_extbus_lock(void);
void g4b_extbus_unlock(void);
void g4b_extbus_replay_begin(void);
void g4b_extbus_replay_end(void);
void g4b_extbus_wait_replay(void);
#else
static inline void g4b_extbus_lock(void) {}
static inline void g4b_extbus_unlock(void) {}
static inline void g4b_extbus_replay_begin(void) {}
static inline void g4b_extbus_replay_end(void) {}
static inline void g4b_extbus_wait_replay(void) {}
#endif
