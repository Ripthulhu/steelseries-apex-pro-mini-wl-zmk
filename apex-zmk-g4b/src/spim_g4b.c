/* SPDX-License-Identifier: MIT */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <nrfx.h>

#include "evidence_g4b.h"
#include "pins_g4b.h"
#include "spim_g4b.h"

#define G4B_CPU_HZ 64000000u
#define G4B_US_CYCLES (G4B_CPU_HZ / 1000000u)

/* Ready-wait budget. Measured enable-to-ready time is about 80.2 ms with a
 * 66 us spread over six launches, so 250 ms is generous without being unbounded.
 * Stock's own wait has no timeout. A bounded wait lets the caller report a
 * failure instead of hanging the Nordic scan thread.
 */
#define G4B_READY_TIMEOUT_US 250000u
#define G4B_END_TIMEOUT_US 50000u

/* Inter-phase settle, from stock: 640 cycles at 64 MHz. */
#define G4B_PHASE_GAP_US 10u

static uint32_t elapsed_us(uint32_t start)
{
    return (g4b_cyccnt() - start) / G4B_US_CYCLES;
}

void g4b_spim_arm_ready(void)
{
    g4b_gpiote_ready_clear();
}

/* READY is level-sensitive. It asserts once after bring-up and remains high
 * until a transfer makes the STM32 busy. Check the level first; if it is low,
 * wait for the next rising-edge latch.
 */
static bool wait_ready(uint32_t timeout_us, uint32_t *waited_us)
{
    uint32_t start = g4b_cyccnt();

    for (;;) {
        if (g4b_pin_read(G4B_PORT0, G4B_P0_READY)) {
            g4b_gpiote_ready_clear();
            *waited_us = elapsed_us(start);
            return true;
        }
        if (g4b_gpiote_ready_event()) {
            g4b_gpiote_ready_clear();
            *waited_us = elapsed_us(start);
            return true;
        }
        if (elapsed_us(start) > timeout_us) {
            *waited_us = elapsed_us(start);
            return false;
        }
    }
}

#if CONFIG_APEX_G4B_STM32_STOP1_IDLE_MS > 0
static bool wait_ready_low(uint32_t timeout_us, uint32_t *waited_us)
{
    uint32_t start = g4b_cyccnt();

    for (;;) {
        if (!g4b_pin_read(G4B_PORT0, G4B_P0_READY)) {
            *waited_us = elapsed_us(start);
            return true;
        }
        if (elapsed_us(start) > timeout_us) {
            *waited_us = elapsed_us(start);
            return false;
        }
    }
}
#endif

void g4b_spim_enable(void)
{
    /* nrfx's order: pin levels first, then direction, then the peripheral. */
    g4b_pin_clr(G4B_PORT0, G4B_P0_SCK);
    g4b_pin_cfg(G4B_PORT0, G4B_P0_SCK, G4B_CNF_SCK_OUT);
    g4b_pin_clr(G4B_PORT0, G4B_P0_MOSI);
    g4b_pin_cfg(G4B_PORT0, G4B_P0_MOSI, G4B_CNF_MOSI_OUT);
    g4b_pin_cfg(G4B_PORT0, G4B_P0_MISO, G4B_CNF_MISO_IN);

    NRF_SPIM3->PSEL.SCK = G4B_P0_SCK;
    NRF_SPIM3->PSEL.MOSI = G4B_P0_MOSI;
    NRF_SPIM3->PSEL.MISO = G4B_P0_MISO;
    NRF_SPIM3->PSEL.CSN = 0xFFFFFFFFu; /* there is no chip select on this link */
    NRF_SPIM3->FREQUENCY = 0x40000000u; /* M4 = 4 Mbit/s, not 8 */
    NRF_SPIM3->CONFIG = 0u;             /* mode 0, MSB first */
    NRF_SPIM3->ORC = 0u;                /* MOSI held low during the RX phase */
    NRF_SPIM3->SHORTS = 0u;
    NRF_SPIM3->INTENCLR = 0xFFFFFFFFu;  /* fully polled */
    /* IFTIMING.RXDELAY / CSNDUR / CSNPOL are left at reset values. Stock never
     * writes them and the snapshot shows the reset values in a working link.
     */
    NRF_SPIM3->ENABLE = 7u;
    __DSB();
}

void g4b_spim_disable(void)
{
    NRF_SPIM3->ENABLE = 0u;
    __DSB();
    NRF_SPIM3->PSEL.SCK = 0xFFFFFFFFu;
    NRF_SPIM3->PSEL.MOSI = 0xFFFFFFFFu;
    NRF_SPIM3->PSEL.MISO = 0xFFFFFFFFu;
}

#if CONFIG_APEX_G4B_STM32_STOP1_IDLE_MS > 0
bool g4b_spim_link_resync(uint32_t *wait_low_us, uint32_t *wait_ready_us)
{
    uint32_t low_us = 0u;
    uint32_t ready_us = 0u;
    bool saw_low;
    bool saw_ready = false;

    /* Keep an external-NVS operation from preempting this timing-sensitive
     * handshake. This is the same bus-wide arbitration used by an exchange;
     * it protects CPU ownership here even though the NOR is on SPIM0. */
    g4b_extbus_lock();

    /* Stock 0x28EE4 does this in precisely this order: uninit SPIM3, restore
     * the READY event, make P0.06 a plain output HIGH, and wait until a RUNNING
     * STM32 acknowledges by pulling READY low. This resynchronizes the link;
     * it cannot wake the mode-0 hard stop or a STOP1'd core. */
    g4b_spim_disable();
    g4b_gpiote_ready_clear();
    g4b_pin_cfg(G4B_PORT0, G4B_P0_MOSI, G4B_CNF_MOSI_OUT);
    g4b_pin_set(G4B_PORT0, G4B_P0_MOSI);
    __DSB();

    saw_low = wait_ready_low(G4B_READY_TIMEOUT_US, &low_us);

    /* Always restore the proven active-link pin/peripheral state, including
     * MOSI low. On the success path stock then delays 10 ms and waits for the
     * scanner to raise READY after rebuilding its clocks and scan state. */
    g4b_spim_enable();
    if (saw_low) {
        k_msleep(10);
        saw_ready = wait_ready(G4B_READY_TIMEOUT_US, &ready_us);
    }

    g4b_extbus_unlock();

    if (wait_low_us != NULL) {
        *wait_low_us = low_us;
    }
    if (wait_ready_us != NULL) {
        *wait_ready_us = ready_us;
    }
    return saw_low && saw_ready;
}
#endif

/* SPIM2 (RGB) and SPIM3 (STM32) are independent EasyDMA peripherals. All
 * access to them is serialized by the g4b thread; see g4b_rgb_flush().
 *
 * That invariant holds only while nothing else touches a SPIM bus. Putting ZMK
 * NVS on the external SPI-NOR (SPIM0) breaks it: settings saves run on the system
 * workqueue, a cooperative thread that outranks the g4b thread. The guards below
 * (compiled in only for CONFIG_APEX_G4B_SPINOR_FLASHDEV) restore it - see the
 * long note at the g4b_extbus_* declarations in spim_g4b.h. When the feature is
 * off, none of this code is compiled.
 */
#if IS_ENABLED(CONFIG_APEX_G4B_SPINOR_FLASHDEV)
/* One bus-wide lock shared with the SPIM0 NVS driver (spinor_g4b.c). A K_MUTEX,
 * not a spinlock/irq_lock: the flash busy-waits reach ~1 s, so a loser must SLEEP
 * (yield the CPU) rather than mask interrupts. Priority inheritance boosts the
 * g4b thread so an in-progress exchange always finishes before a waiting NVS op
 * runs - the STM32 is never stranded mid-transaction. */
static K_MUTEX_DEFINE(g4b_extbus_mutex);

void g4b_extbus_lock(void)   { k_mutex_lock(&g4b_extbus_mutex, K_FOREVER); }
void g4b_extbus_unlock(void) { k_mutex_unlock(&g4b_extbus_mutex); }

/* The paced 59-frame replay cannot tolerate SPIM0 traffic between frames. The
 * g4b thread holds the bus mutex from begin to end; the per-frame exchange locks
 * are recursive. The flags let writes and erases sleep before taking the mutex.
 * Reads are not gated because settings load runs before this thread starts. */
static volatile bool g4b_replay_active;
static volatile bool g4b_replay_done;

void g4b_extbus_replay_begin(void)
{
    /* Hold the outer mutex for the complete replay. Individual exchanges lock
     * it recursively, which Zephyr mutexes support. This closes the gap between
     * a flash writer checking the replay flag and taking the bus lock. */
    g4b_extbus_lock();
    g4b_replay_done = false;
    g4b_replay_active = true;
}

void g4b_extbus_replay_end(void)
{
    g4b_replay_done = true;
    g4b_replay_active = false;
    g4b_extbus_unlock();
}

void g4b_extbus_wait_replay(void)
{
    /* Block only while a replay is actually in flight. Before the first replay
     * (boot mount/load on main) and after it completes, this returns at once. */
    while (g4b_replay_active && !g4b_replay_done) {
        k_msleep(2);
    }
}
#endif /* CONFIG_APEX_G4B_SPINOR_FLASHDEV */

static bool run_phase(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    uint32_t start;
    bool ok = true;

    NRF_SPIM3->TXD.PTR = (uint32_t)tx;
    NRF_SPIM3->TXD.MAXCNT = tx ? len : 0u;
    NRF_SPIM3->RXD.PTR = (uint32_t)rx;
    NRF_SPIM3->RXD.MAXCNT = rx ? len : 0u;
    NRF_SPIM3->TXD.LIST = 0u;
    NRF_SPIM3->RXD.LIST = 0u;
    NRF_SPIM3->EVENTS_END = 0u;
    (void)NRF_SPIM3->EVENTS_END;
    __DSB();

    NRF_SPIM3->TASKS_START = 1u;

    start = g4b_cyccnt();
    while (NRF_SPIM3->EVENTS_END == 0u) {
        if (elapsed_us(start) > G4B_END_TIMEOUT_US) {
            ok = false;
            break;
        }
    }

    if (ok) {
        k_busy_wait(G4B_PHASE_GAP_US);
    }

    return ok;
}

static enum g4b_spim_result spim_exchange_inner(const uint8_t *tx, uint8_t *rx,
                                                struct g4b_exchange_stats *stats)
{
    stats->tx_amount = 0u;
    stats->rx_amount = 0u;
    stats->events_end = 0u;

    if (!wait_ready(G4B_READY_TIMEOUT_US, &stats->wait_tx_us)) {
        stats->result = G4B_SPIM_NOT_READY_TX;
        return G4B_SPIM_NOT_READY_TX;
    }

    if (!run_phase(tx, NULL, G4B_SPIM_FRAME)) {
        stats->tx_amount = NRF_SPIM3->TXD.AMOUNT;
        stats->result = G4B_SPIM_TX_TIMEOUT;
        return G4B_SPIM_TX_TIMEOUT;
    }
    stats->tx_amount = NRF_SPIM3->TXD.AMOUNT;

    if (!wait_ready(G4B_READY_TIMEOUT_US, &stats->wait_rx_us)) {
        stats->result = G4B_SPIM_NOT_READY_RX;
        return G4B_SPIM_NOT_READY_RX;
    }

    if (!run_phase(NULL, rx, G4B_SPIM_FRAME)) {
        stats->rx_amount = NRF_SPIM3->RXD.AMOUNT;
        stats->result = G4B_SPIM_RX_TIMEOUT;
        return G4B_SPIM_RX_TIMEOUT;
    }

    /* TXD.AMOUNT is hardware-written per transaction. The RX phase sets
     * TXD.MAXCNT = 0, so reading TXD.AMOUNT after it always yields 0 - which is
     * exactly what the stock snapshot recorded after a successful exchange.
     * Latch it per phase, above, or a good run reports "TASKS_START never took".
     */
    stats->rx_amount = NRF_SPIM3->RXD.AMOUNT;
    stats->events_end = NRF_SPIM3->EVENTS_END;
    stats->result = G4B_SPIM_OK;
    return G4B_SPIM_OK;
}

/* The public exchange takes the shared external-bus lock so the whole two-phase
 * transaction is indivisible against any SPIM0 NVS op. When the external-NVS
 * feature is off, g4b_extbus_lock/unlock are empty inlines and this is a plain
 * call into the unchanged inner routine. */
enum g4b_spim_result g4b_spim_exchange(const uint8_t *tx, uint8_t *rx,
                                       struct g4b_exchange_stats *stats)
{
    enum g4b_spim_result r;

    g4b_extbus_lock();
    r = spim_exchange_inner(tx, rx, stats);
    g4b_extbus_unlock();
    return r;
}
