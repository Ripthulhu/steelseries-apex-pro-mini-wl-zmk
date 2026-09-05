/* SPDX-License-Identifier: MIT
 *
 * Legacy TWI1, in the same direct-register style as the SPIM link and the RGB
 * driver. See twi_g4b.h for why the write path is four registers wide and has
 * no general write(reg, val).
 *
 * Legacy TWI, not TWIM, because that is what stock uses and it is what the
 * register map at this base actually is: RXD at +0x518 and TXD at +0x51C exist
 * only on the non-DMA TWI. TWIM would need EasyDMA buffers for a single byte.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <nrfx.h>

#include "pins_g4b.h"
#include "twi_g4b.h"

#define G4B_TWI1_BASE 0x40004000u

#define G4B_TWI_TASKS_STARTRX (G4B_TWI1_BASE + 0x000u)
#define G4B_TWI_TASKS_STARTTX (G4B_TWI1_BASE + 0x008u)
#define G4B_TWI_TASKS_STOP    (G4B_TWI1_BASE + 0x014u)
#define G4B_TWI_EVENTS_STOPPED   (G4B_TWI1_BASE + 0x104u)
#define G4B_TWI_EVENTS_RXDREADY  (G4B_TWI1_BASE + 0x108u)
#define G4B_TWI_EVENTS_TXDSENT   (G4B_TWI1_BASE + 0x11Cu)
#define G4B_TWI_EVENTS_ERROR     (G4B_TWI1_BASE + 0x124u)
#define G4B_TWI_SHORTS    (G4B_TWI1_BASE + 0x200u)
#define G4B_TWI_ERRORSRC  (G4B_TWI1_BASE + 0x4C4u)
#define G4B_TWI_ENABLE    (G4B_TWI1_BASE + 0x500u)
#define G4B_TWI_PSEL_SCL  (G4B_TWI1_BASE + 0x508u)
#define G4B_TWI_PSEL_SDA  (G4B_TWI1_BASE + 0x50Cu)
#define G4B_TWI_RXD       (G4B_TWI1_BASE + 0x518u)
#define G4B_TWI_TXD       (G4B_TWI1_BASE + 0x51Cu)
#define G4B_TWI_FREQUENCY (G4B_TWI1_BASE + 0x524u)
#define G4B_TWI_ADDRESS   (G4B_TWI1_BASE + 0x588u)

#define G4B_TWI_ENABLE_ON  5u /* 5 = TWI. TWIM would be 6 and SPIM 7. */
#define G4B_TWI_ENABLE_OFF 0u
#define G4B_TWI_FREQ_K100  0x01980000u
#define G4B_TWI_SHORT_BB_STOP (1u << 1)

#define G4B_BQ_ADDR 0x6Au /* 7-bit; the peripheral adds the R/W bit */

#define G4B_BQ_REG_ADC        0x02u
#define G4B_BQ_REG_CHGCTL     0x03u /* REG03: BAT_LOADEN, WD_RST, OTG/CHG_CONFIG, SYS_MIN */
#define G4B_BQ_REG_ICHG       0x04u
#define G4B_BQ_REG_VREG       0x06u
#define G4B_BQ_REG_TIMER      0x07u
#define G4B_BQ_REG_ICHGR      0x12u /* charge current readback, ADC */
#define G4B_BQ_CHG_CONFIG     0x10u /* REG03 bit 4: 1 = charge enable, 0 = passthrough */
#define G4B_BQ_PG_STAT        0x04u /* REG0B bit 2: 1 = input power good (VBUS present) */
#define G4B_BQ_CONV_START     0x80u /* bit 7. Bit 6 is CONV_RATE - not that. */
#define G4B_BQ_WATCHDOG_MASK  0x30u /* REG07 bits 5:4 */
#define G4B_BQ_BATV_OFFSET_MV 2304u
#define G4B_BQ_BATV_STEP_MV   20u
#define G4B_BQ_ICHGR_STEP_MA  50u   /* REG12 readback, 50 mA per count */

/* REG06 bits 7:2 are VREG, offset 3840 mV in 16 mV steps; bit 1 BATLOWV (1 =
 * 3.0 V), bit 0 VRECHG (0 = 100 mV). 0x42 therefore decodes as:
 *     (0x42 >> 2) = 0x10 = 16  ->  3840 + 16*16 = 4096 mV
 * which is a deliberately low charge ceiling for cell longevity, NOT the pack's
 * rated 4.400 V maximum. The cell is soldered into a keyboard that spends much
 * of its life plugged in, and a lithium cell held near its rated top voltage is
 * the classic cause of the swelling this pack is known for. 4.096 V (the nearest
 * 16 mV step to a 4.10 V target) keeps it well off that top rail. The low two
 * bits are the same as stock's: BATLOWV = 1, VRECHG = 0. Asserted rather than
 * commented, because a wrong constant here would overcharge the cell.
 */
#define G4B_BQ_VREG_4096MV 0x42u
BUILD_ASSERT(3840u + ((G4B_BQ_VREG_4096MV >> 2) * 16u) == 4096u,
             "REG06 constant must decode to 4096 mV, the longevity ceiling");

/* REG04 bits 6:0 are ICHG in 64 mA steps; bit 7 EN_PUMPX must stay clear. */
#define G4B_BQ_ICHG_1472MA 0x17u
BUILD_ASSERT((G4B_BQ_ICHG_1472MA & 0x7Fu) * 64u == 1472u,
             "REG04 constant must decode to 1472 mA");
BUILD_ASSERT((G4B_BQ_ICHG_1472MA & 0x80u) == 0u, "EN_PUMPX must stay clear");

#if IS_ENABLED(CONFIG_APEX_G4B_CHARGE_STORAGE)
BUILD_ASSERT(CONFIG_APEX_G4B_CHARGE_RESUME_PCT < CONFIG_APEX_G4B_CHARGE_STOP_PCT,
             "charge resume threshold must be below the stop threshold");
#endif

/* A conversion takes tens of milliseconds. Poll for it rather than sleeping a
 * fixed guess, and give up rather than spin: this runs on the key-scan thread.
 */
#define G4B_BQ_CONV_TRIES 40u

/* Every wait is bounded. An absent or wedged device must return an error, not
 * stall the scan loop - this runs on the same thread that reads keys.
 */
#define G4B_TWI_SPIN 200000u

#define REG32(a) (*(volatile uint32_t *)(a))

/* TWI1 has two callers on different threads: the diagnostic probe on the g4b
 * scan thread, and ZMK battery reporting on the low-priority work queue. One
 * peripheral, one set of PSEL registers, one register pointer inside the
 * charger - interleaving them would produce readings from the wrong register
 * and, far worse, could split a read-modify-write on a charge-control register.
 *
 * Zephyr mutexes are recursive for the owning thread, so g4b_bq_sample_mv() can
 * hold this across its start-conversion and its poll while the inner
 * g4b_bq_read() calls take it again without deadlocking. That is the point: a
 * conversion and the read of its result are ONE transaction.
 */
static K_MUTEX_DEFINE(bq_lock);

static bool twi_wait(uint32_t event_addr, uint32_t *errorsrc)
{
    uint32_t spin = G4B_TWI_SPIN;

    while (spin-- != 0u) {
        if (REG32(event_addr) != 0u) {
            return true;
        }
        if (REG32(G4B_TWI_EVENTS_ERROR) != 0u) {
            *errorsrc = REG32(G4B_TWI_ERRORSRC);
            return false;
        }
    }
    return false;
}

struct g4b_twi_result g4b_bq_read(uint8_t reg)
{
    struct g4b_twi_result res = { 0u, 0u, false };
    uint32_t errorsrc = 0u;

    k_mutex_lock(&bq_lock, K_FOREVER);
    g4b_twi_pins_claim();

    REG32(G4B_TWI_PSEL_SCL) = (uint32_t)G4B_P0_TWI_SCL;
    REG32(G4B_TWI_PSEL_SDA) = (uint32_t)G4B_P0_TWI_SDA;
    REG32(G4B_TWI_FREQUENCY) = G4B_TWI_FREQ_K100;
    REG32(G4B_TWI_ADDRESS) = G4B_BQ_ADDR;
    REG32(G4B_TWI_SHORTS) = 0u;
    REG32(G4B_TWI_ENABLE) = G4B_TWI_ENABLE_ON;
    __DSB();

    REG32(G4B_TWI_EVENTS_TXDSENT) = 0u;
    REG32(G4B_TWI_EVENTS_STOPPED) = 0u;
    REG32(G4B_TWI_EVENTS_RXDREADY) = 0u;
    REG32(G4B_TWI_EVENTS_ERROR) = 0u;
    REG32(G4B_TWI_ERRORSRC) = REG32(G4B_TWI_ERRORSRC); /* write-1-to-clear */

    /* Phase 1: set the register pointer. This is addressing, not a write to a
     * charger register - no register's value changes, and the device stays in
     * autonomous mode.
     */
    REG32(G4B_TWI_TASKS_STARTTX) = 1u;
    REG32(G4B_TWI_TXD) = reg;
    if (!twi_wait(G4B_TWI_EVENTS_TXDSENT, &errorsrc)) {
        goto out;
    }

    REG32(G4B_TWI_EVENTS_STOPPED) = 0u;
    REG32(G4B_TWI_TASKS_STOP) = 1u;
    if (!twi_wait(G4B_TWI_EVENTS_STOPPED, &errorsrc)) {
        goto out;
    }

    /* Phase 2: one byte back. BB_STOP is armed BEFORE STARTRX so the STOP is
     * issued off the byte-boundary event; arming it afterwards is the classic
     * way to read an unrequested second byte.
     */
    REG32(G4B_TWI_SHORTS) = G4B_TWI_SHORT_BB_STOP;
    REG32(G4B_TWI_EVENTS_RXDREADY) = 0u;
    REG32(G4B_TWI_EVENTS_STOPPED) = 0u;
    REG32(G4B_TWI_EVENTS_ERROR) = 0u;
    __DSB();

    REG32(G4B_TWI_TASKS_STARTRX) = 1u;
    if (!twi_wait(G4B_TWI_EVENTS_RXDREADY, &errorsrc)) {
        goto out;
    }

    res.value = (uint8_t)REG32(G4B_TWI_RXD);
    res.ok = true;
    (void)twi_wait(G4B_TWI_EVENTS_STOPPED, &errorsrc);

out:
    res.errorsrc = (uint8_t)(errorsrc | REG32(G4B_TWI_ERRORSRC));

    /* Leave the bus as it was found: peripheral off, pins released. A pin left
     * driven here would sit across a bus the charger shares.
     */
    REG32(G4B_TWI_SHORTS) = 0u;
    REG32(G4B_TWI_ENABLE) = G4B_TWI_ENABLE_OFF;
    REG32(G4B_TWI_PSEL_SCL) = 0xFFFFFFFFu;
    REG32(G4B_TWI_PSEL_SDA) = 0xFFFFFFFFu;
    __DSB();

    g4b_twi_pins_release();
    k_mutex_unlock(&bq_lock);

    return res;
}

/* Writes are private and gated on an allowlist checked here rather than at the
 * call sites. Only the registers below can be written, and REG00
 * (EN_HIZ isolates the input and stops charging until reset) stays unreachable.
 *
 * REG03 is on the list, but reachable ONLY through g4b_bq_set_charging(), which
 * is a masked read-modify-write of the single CHG_CONFIG bit - it never disturbs
 * SYS_MIN or OTG_CONFIG. That is what lets the storage controller toggle charge
 * on/off for passthrough; a blind full-byte write to REG03 is still not exposed.
 *
 * Every caller below is a named, single-purpose function with its value either
 * baked in as a constant or built by read-modify-write. There is no exported
 * write(reg, val).
 */
static bool bq_reg_writable(uint8_t reg)
{
    return reg == G4B_BQ_REG_ADC ||    /* 0x02 conversion control */
           reg == G4B_BQ_REG_CHGCTL || /* 0x03 CHG_CONFIG only, via set_charging */
           reg == G4B_BQ_REG_ICHG ||   /* 0x04 charge current */
           reg == G4B_BQ_REG_VREG ||   /* 0x06 charge voltage limit */
           reg == G4B_BQ_REG_TIMER;    /* 0x07 termination / watchdog */
}

static bool bq_write(uint8_t reg, uint8_t value)
{
    uint32_t errorsrc = 0u;
    bool ok = false;

    if (!bq_reg_writable(reg)) {
        return false;
    }

    k_mutex_lock(&bq_lock, K_FOREVER);
    g4b_twi_pins_claim();
    REG32(G4B_TWI_PSEL_SCL) = (uint32_t)G4B_P0_TWI_SCL;
    REG32(G4B_TWI_PSEL_SDA) = (uint32_t)G4B_P0_TWI_SDA;
    REG32(G4B_TWI_FREQUENCY) = G4B_TWI_FREQ_K100;
    REG32(G4B_TWI_ADDRESS) = G4B_BQ_ADDR;
    REG32(G4B_TWI_SHORTS) = 0u;
    REG32(G4B_TWI_ENABLE) = G4B_TWI_ENABLE_ON;
    __DSB();

    REG32(G4B_TWI_EVENTS_TXDSENT) = 0u;
    REG32(G4B_TWI_EVENTS_STOPPED) = 0u;
    REG32(G4B_TWI_EVENTS_ERROR) = 0u;
    REG32(G4B_TWI_ERRORSRC) = REG32(G4B_TWI_ERRORSRC);

    REG32(G4B_TWI_TASKS_STARTTX) = 1u;
    REG32(G4B_TWI_TXD) = reg;
    if (twi_wait(G4B_TWI_EVENTS_TXDSENT, &errorsrc)) {
        REG32(G4B_TWI_EVENTS_TXDSENT) = 0u;
        REG32(G4B_TWI_TXD) = value;
        ok = twi_wait(G4B_TWI_EVENTS_TXDSENT, &errorsrc);
    }

    REG32(G4B_TWI_EVENTS_STOPPED) = 0u;
    REG32(G4B_TWI_TASKS_STOP) = 1u;
    (void)twi_wait(G4B_TWI_EVENTS_STOPPED, &errorsrc);

    REG32(G4B_TWI_ENABLE) = G4B_TWI_ENABLE_OFF;
    REG32(G4B_TWI_PSEL_SCL) = 0xFFFFFFFFu;
    REG32(G4B_TWI_PSEL_SDA) = 0xFFFFFFFFu;
    __DSB();
    g4b_twi_pins_release();
    k_mutex_unlock(&bq_lock);

    return ok;
}

bool g4b_bq_configure_charge(void)
{
    struct g4b_twi_result timer;

    /* ORDER MATTERS. The watchdog goes first.
     *
     * Any write starts the 40 s I2C watchdog, and when it lapses REG00-REG07
     * revert to power-on defaults. Setting the charge voltage first and the
     * watchdog second would work for 40 seconds and then quietly undo itself -
     * which is exactly the failure that would look like "it charges to 4.4 V
     * sometimes".
     */
    timer = g4b_bq_read(G4B_BQ_REG_TIMER);
    if (!timer.ok) {
        return false;
    }
    /* Read-modify-write, clearing only the WATCHDOG field. Writing stock's
     * literal 0x8B would also assert its termination and safety-timer policy;
     * this leaves every other bit at whatever the charger chose.
     */
    if (!bq_write(G4B_BQ_REG_TIMER,
                  (uint8_t)(timer.value & (uint8_t)~G4B_BQ_WATCHDOG_MASK))) {
        return false;
    }

    /* 4.096 V, a longevity ceiling well below the pack's 4.400 V rated maximum
     * and below even the 4.208 V power-on default. Fuji 4867A0, 5870 mAh,
     * 3.85 V nominal - a high-voltage cell, and high-voltage cells swell fastest
     * when parked near their top voltage, which an always-plugged-in keyboard
     * does continuously. Charging to 4.096 V instead trades roughly a quarter of
     * the runtime for a cell that stays flat. See G4B_BQ_VREG_4096MV.
     */
    if (!bq_write(G4B_BQ_REG_VREG, G4B_BQ_VREG_4096MV)) {
        return false;
    }

    /* 1472 mA, stock's choice for a USB source: 0.25C on a 5870 mAh pack. The
     * 2048 mA default is 0.35C, still within normal limits, but there is no
     * reason to charge harder than the vendor decided to.
     */
    return bq_write(G4B_BQ_REG_ICHG, G4B_BQ_ICHG_1472MA);
}

/* --- Runtime charge configuration --------------------------------------------
 *
 * Change the charge ceiling/current after boot. g4b_bq_configure_charge() has
 * already disarmed the 40 s I2C watchdog, so a single register write sticks.
 * Named-op bq_write() only (REG06/REG04); REG06 is hard-clamped so VREG cannot
 * exceed the pack's 4.400 V rated max.
 */
#define G4B_BQ_VREG_MIN_MV 3840u
#define G4B_BQ_VREG_MAX_MV 4400u /* Fuji 4867A0 rated max - never exceed */
#define G4B_BQ_ICHG_MAX_MA 2048u /* 0.35C ceiling; heat/longevity guard */

bool g4b_bq_set_vreg_mv(uint16_t mv)
{
    if (mv < G4B_BQ_VREG_MIN_MV) {
        mv = G4B_BQ_VREG_MIN_MV;
    }
    if (mv > G4B_BQ_VREG_MAX_MV) {
        mv = G4B_BQ_VREG_MAX_MV;
    }
    uint8_t steps = (uint8_t)((mv - G4B_BQ_VREG_MIN_MV) / 16u); /* 6-bit field */
    uint8_t reg = (uint8_t)((steps << 2) | 0x02u); /* BATLOWV=1, VRECHG=0 */
    return bq_write(G4B_BQ_REG_VREG, reg);
}

uint16_t g4b_bq_get_vreg_mv(void)
{
    struct g4b_twi_result r = g4b_bq_read(G4B_BQ_REG_VREG);
    if (!r.ok) {
        return 0u;
    }
    return (uint16_t)(G4B_BQ_VREG_MIN_MV + ((r.value >> 2) * 16u));
}

bool g4b_bq_set_ichg_ma(uint16_t ma)
{
    if (ma > G4B_BQ_ICHG_MAX_MA) {
        ma = G4B_BQ_ICHG_MAX_MA;
    }
    uint8_t reg = (uint8_t)((ma / 64u) & 0x7Fu); /* EN_PUMPX (bit7) stays clear */
    return bq_write(G4B_BQ_REG_ICHG, reg);
}

uint16_t g4b_bq_get_ichg_ma(void)
{
    struct g4b_twi_result r = g4b_bq_read(G4B_BQ_REG_ICHG);
    if (!r.ok) {
        return 0u;
    }
    return (uint16_t)((r.value & 0x7Fu) * 64u);
}

/* Battery temperature from the BQ25895 TS pin (a real pack NTC - confirmed: the
 * TS% swings monotonically as the pack is warmed). REG10 holds TSPCT as bits 6:0
 * (21% base, weighted); this table maps each of the 128 codes to degC using the
 * part's reference 103AT network (RT1 5.24k / RT2 30.31k, B 3435). Accurate to a
 * couple degC; a fixed offset would calibrate it. REG10 is only fresh after an
 * ADC conversion, so callers must run g4b_bq_sample_mv() first. */
#define G4B_BQ_REG_TS 0x10u
static const int8_t g4b_bq_ts_temp_tbl[128] = {
      85,   84,   83,   82,   81,   80,   79,   78,   77,   76,   75,   74,   73,   72,   71,   70,
      70,   69,   68,   67,   66,   65,   65,   64,   63,   62,   62,   61,   60,   59,   59,   58,
      57,   57,   56,   55,   54,   54,   53,   52,   52,   51,   50,   50,   49,   48,   48,   47,
      46,   46,   45,   45,   44,   43,   43,   42,   41,   41,   40,   39,   39,   38,   38,   37,
      36,   36,   35,   34,   34,   33,   32,   32,   31,   31,   30,   29,   29,   28,   27,   27,
      26,   25,   25,   24,   23,   23,   22,   21,   21,   20,   19,   19,   18,   17,   16,   16,
      15,   14,   13,   13,   12,   11,   10,   10,    9,    8,    7,    6,    5,    4,    3,    2,
       1,    1,   -1,   -2,   -3,   -4,   -5,   -6,   -7,   -9,  -10,  -11,  -13,  -14,  -16,  -18,
};

int16_t g4b_bq_ts_temp_c(void)
{
    struct g4b_twi_result r = g4b_bq_read(G4B_BQ_REG_TS);
    if (!r.ok) {
        return G4B_BQ_TEMP_INVALID;
    }
    int8_t c = g4b_bq_ts_temp_tbl[r.value & 0x7Fu];
    if (c <= -40) {
        return G4B_BQ_TEMP_INVALID; /* TS open / out of the pack's usable range */
    }
    return (int16_t)c;
}

static bool bq_start_conversion(void)
{
    struct g4b_twi_result cur = g4b_bq_read(G4B_BQ_REG_ADC);
    uint32_t errorsrc = 0u;
    bool ok = false;

    if (!cur.ok) {
        return false;
    }
    if ((cur.value & G4B_BQ_CONV_START) != 0u) {
        return true; /* already converting - do not write at all */
    }

    /* Taken here rather than at the top of the function so it stays balanced
     * against the single unlock below: both early returns above happen before
     * this point. Recursive, since the caller already holds it.
     */
    k_mutex_lock(&bq_lock, K_FOREVER);
    g4b_twi_pins_claim();
    REG32(G4B_TWI_PSEL_SCL) = (uint32_t)G4B_P0_TWI_SCL;
    REG32(G4B_TWI_PSEL_SDA) = (uint32_t)G4B_P0_TWI_SDA;
    REG32(G4B_TWI_FREQUENCY) = G4B_TWI_FREQ_K100;
    REG32(G4B_TWI_ADDRESS) = G4B_BQ_ADDR;
    REG32(G4B_TWI_SHORTS) = 0u;
    REG32(G4B_TWI_ENABLE) = G4B_TWI_ENABLE_ON;
    __DSB();

    REG32(G4B_TWI_EVENTS_TXDSENT) = 0u;
    REG32(G4B_TWI_EVENTS_STOPPED) = 0u;
    REG32(G4B_TWI_EVENTS_ERROR) = 0u;
    REG32(G4B_TWI_ERRORSRC) = REG32(G4B_TWI_ERRORSRC);

    REG32(G4B_TWI_TASKS_STARTTX) = 1u;
    REG32(G4B_TWI_TXD) = G4B_BQ_REG_ADC;
    if (twi_wait(G4B_TWI_EVENTS_TXDSENT, &errorsrc)) {
        REG32(G4B_TWI_EVENTS_TXDSENT) = 0u;
        /* Only bit 7 is added. Every other bit is what the read returned. */
        REG32(G4B_TWI_TXD) = (uint32_t)(cur.value | G4B_BQ_CONV_START);
        ok = twi_wait(G4B_TWI_EVENTS_TXDSENT, &errorsrc);
    }

    REG32(G4B_TWI_EVENTS_STOPPED) = 0u;
    REG32(G4B_TWI_TASKS_STOP) = 1u;
    (void)twi_wait(G4B_TWI_EVENTS_STOPPED, &errorsrc);

    REG32(G4B_TWI_ENABLE) = G4B_TWI_ENABLE_OFF;
    REG32(G4B_TWI_PSEL_SCL) = 0xFFFFFFFFu;
    REG32(G4B_TWI_PSEL_SDA) = 0xFFFFFFFFu;
    __DSB();
    g4b_twi_pins_release();
    k_mutex_unlock(&bq_lock);

    return ok;
}

/* Stock's discharge curve, read out of flash at vaddr 0x00044964: 21 entries,
 * 6-byte stride, the threshold at +2. Entry i covers i*5 percent, so 3399 mV is
 * 0 % and the final 0xFFFF entry is 100 %.
 *
 * This is the vendor's own characterisation of THIS pack - a Fuji 4867A0,
 * 5870 mAh, 3.85 V nominal - so it is worth far more than a generic lithium
 * curve. Kept verbatim; the ceiling is handled by scaling below rather than by
 * editing the vendor's numbers.
 */
static const uint16_t bq_curve_mv[21] = {
    3399u, 3620u, 3670u, 3710u, 3730u, 3740u, 3750u, 3780u, 3795u, 3815u,
    3825u, 3880u, 3925u, 3980u, 4045u, 4070u, 4135u, 4200u, 4225u, 4285u,
    0xFFFFu,
};

static uint32_t bq_curve_percent(uint32_t mv)
{
    for (uint32_t i = 0u; i < ARRAY_SIZE(bq_curve_mv); i++) {
        if (mv <= (uint32_t)bq_curve_mv[i]) {
            return i * 5u;
        }
    }
    return 100u;
}

uint32_t g4b_bq_percent(uint32_t mv)
{
    uint32_t raw;
    uint32_t full;

    if (mv == 0u) {
        return 0u;
    }

    /* Normalise to the ceiling this board actually charges to.
     *
     * The curve above is stock's, characterised against its 4.400 V charge. We
     * cap REG06 at 4.096 V for longevity, so a full charge terminates near there
     * - only about 80 % on the vendor's curve, which would otherwise mean the
     * gauge never read full.
     *
     * Scaling by the percent the configured ceiling maps to makes the reading
     * correct for whatever ceiling is in force: with FULL_MV at 4096 a full
     * charge reads 100 %, and setting it to 4370 for stock's 4.4 V would give a
     * scale factor of one and the vendor curve unmodified.
     */
    raw = bq_curve_percent(mv);
    full = bq_curve_percent((uint32_t)CONFIG_APEX_G4B_BATT_FULL_MV);
    if (full == 0u) {
        return raw;
    }

    raw = raw * 100u / full;
    return (raw > 100u) ? 100u : raw;
}

uint32_t g4b_bq_sample_mv(void)
{
    uint32_t mv = 0u;

    /* Held across the whole sequence, not just the individual transfers: a
     * conversion started here must be the one whose result is read below.
     */
    k_mutex_lock(&bq_lock, K_FOREVER);

    if (bq_start_conversion()) {
        for (uint32_t i = 0u; i < G4B_BQ_CONV_TRIES; i++) {
            struct g4b_twi_result r;

            k_msleep(2);
            r = g4b_bq_read(G4B_BQ_REG_BATV);
            if (r.ok && (r.value & 0x7Fu) != 0u) {
                mv = G4B_BQ_BATV_OFFSET_MV +
                     G4B_BQ_BATV_STEP_MV * (uint32_t)(r.value & 0x7Fu);
                break;
            }
        }
    }

    k_mutex_unlock(&bq_lock);
    return mv;
}

uint32_t g4b_bq_charge_ma(void)
{
    struct g4b_twi_result r = g4b_bq_read(G4B_BQ_REG_ICHGR);

    /* Only meaningful while actually charging. Per Table 8-6 REG12 reads NA in
     * boost, charge-disabled and battery-only modes, so a caller that wants to
     * show this on a display should gate it on REG0B CHRG_STAT rather than
     * treat 0 as "no current" - the two are indistinguishable here.
     *
     * Reads the last conversion; call g4b_bq_sample_mv() first to trigger one.
     */
    if (!r.ok) {
        return 0u;
    }
    return G4B_BQ_ICHGR_STEP_MA * (uint32_t)(r.value & 0x7Fu);
}

bool g4b_bq_is_charging(void)
{
    struct g4b_twi_result r = g4b_bq_read(G4B_BQ_REG_STATUS);

    /* CHRG_STAT is bits 4:3: 0 not charging, 1 pre-charge, 2 fast charge,
     * 3 termination done. "Charging" means current is flowing, so 1 or 2.
     */
    if (!r.ok) {
        return false;
    }
    return ((r.value >> 3) & 0x03u) == 1u || ((r.value >> 3) & 0x03u) == 2u;
}

/* --- Charge on/off control (REG03 CHG_CONFIG) + storage-band controller -------
 *
 * The BQ25895's power path always runs the system from VBUS when the input is
 * present - the keyboard runs with the battery physically removed - so clearing
 * CHG_CONFIG is a true PASSTHROUGH: the system stays up on USB and the cell is
 * left idle, neither charged nor discharged. That is the correct longevity lever,
 * unlike EN_HIZ (cuts the input) or BATFET ship-mode (drops the battery, which
 * would kill the keyboard the moment it is unplugged). Paired with the 4.096 V
 * VREG cap it holds the pack in a healthy band instead of pinning it at the top
 * and nibble-recharging forever. The I2C watchdog is disarmed in
 * g4b_bq_configure_charge(), so a CHG_CONFIG write sticks with no periodic kick.
 */
bool g4b_bq_set_charging(bool enable)
{
    struct g4b_twi_result r = g4b_bq_read(G4B_BQ_REG_CHGCTL);
    uint8_t v;

    if (!r.ok) {
        return false;
    }
    /* Masked RMW: flip only CHG_CONFIG, leaving SYS_MIN / OTG_CONFIG untouched. */
    v = enable ? (uint8_t)(r.value | G4B_BQ_CHG_CONFIG)
               : (uint8_t)(r.value & (uint8_t)~G4B_BQ_CHG_CONFIG);
    if (v == r.value) {
        return true; /* already in the requested state - no bus write */
    }
    return bq_write(G4B_BQ_REG_CHGCTL, v);
}

bool g4b_bq_charging_enabled(void)
{
    struct g4b_twi_result r = g4b_bq_read(G4B_BQ_REG_CHGCTL);

    return r.ok && (r.value & G4B_BQ_CHG_CONFIG) != 0u;
}

bool g4b_bq_charge_terminated(void)
{
    struct g4b_twi_result r = g4b_bq_read(G4B_BQ_REG_STATUS);

    /* CHRG_STAT == 3: charge done (topped to the VREG cap). */
    return r.ok && ((r.value >> 3) & 0x03u) == 3u;
}

bool g4b_bq_power_good(void)
{
    struct g4b_twi_result r = g4b_bq_read(G4B_BQ_REG_STATUS);

    return r.ok && (r.value & G4B_BQ_PG_STAT) != 0u;
}

#if IS_ENABLED(CONFIG_APEX_G4B_CHARGE_STORAGE)
void g4b_bq_storage_tick(uint32_t mv)
{
    /* Only manage charging on external power. With no VBUS the charger cannot
     * charge and CHG_CONFIG is moot; leave whatever state it is in so a replug
     * resumes correctly on the next tick. mv==0 means the read failed - skip. */
    if (mv == 0u || !g4b_bq_power_good()) {
        return;
    }

    /* Thresholds are on the VENDOR curve (percent of the pack's 4.4 V-referenced
     * capacity), matching how the cell is specced. STOP_PCT ~ the 4.096 V cap
     * (~80 %); RESUME_PCT is kept a few points lower so the ~5 % resting-voltage
     * settle right after a charge does not immediately re-trigger one. */
    if (g4b_bq_charging_enabled()) {
        if (g4b_bq_charge_terminated() ||
            bq_curve_percent(mv) >= (uint32_t)CONFIG_APEX_G4B_CHARGE_STOP_PCT) {
            (void)g4b_bq_set_charging(false);
        }
    } else {
        /* In passthrough the reading is the resting voltage (no charge current),
         * so the curve SoC is trustworthy for the resume decision. */
        if (bq_curve_percent(mv) <= (uint32_t)CONFIG_APEX_G4B_CHARGE_RESUME_PCT) {
            (void)g4b_bq_set_charging(true);
        }
    }
}
#else
void g4b_bq_storage_tick(uint32_t mv) { (void)mv; }
#endif /* CONFIG_APEX_G4B_CHARGE_STORAGE */
