/* SPDX-License-Identifier: MIT
 *
 * The only file in the G4B payload permitted to touch GPIO registers.
 * See pins_g4b.h for why.
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/sys/util.h>

#include <nrfx.h>

#include "pins_g4b.h"

/* GPIOTE CONFIG[0] as captured from stock: MODE=Event(1), PSEL=5, PORT=0,
 * POLARITY=LoToHi(1). 0x00010501.
 */
#define G4B_GPIOTE_CONFIG_READY 0x00010501u

/* GPIOTE CONFIG[1] for the STM32 attention line P0.24: MODE=Event(1), PSEL=24,
 * PORT=0, POLARITY=LoToHi(1) -> 0x00011801. The STM32 raises ATTN when it queues
 * a key-state report and drops it once the 0xA1 is read. Interrupt-driving that
 * edge wakes the scan thread instead of waiting for the active or idle poll
 * deadline (which can
 * be hundreds of milliseconds in the battery-saving tiers). Channel 0 (READY)
 * stays polled by wait_ready() and is left with
 * its interrupt masked, so this and the READY path never contend for the ISR.
 */
#define G4B_GPIOTE_CONFIG_ATTN 0x00011801u
#define G4B_GPIOTE_ATTN_CH     1u
#define G4B_GPIOTE_IRQ_PRIO    1u

static K_SEM_DEFINE(g4b_attn_sem, 0, 1);

/* Counts queued-report interrupts so the interrupt path can be distinguished
 * from the timeout fallback. Read via g4b_attn_isr_fires(). */
static volatile uint32_t g4b_attn_isr_fires_ct;

static void g4b_gpiote_attn_isr(const void *arg)
{
    ARG_UNUSED(arg);

    if (NRF_GPIOTE->EVENTS_IN[G4B_GPIOTE_ATTN_CH] != 0u) {
        NRF_GPIOTE->EVENTS_IN[G4B_GPIOTE_ATTN_CH] = 0u;
        (void)NRF_GPIOTE->EVENTS_IN[G4B_GPIOTE_ATTN_CH]; /* flush posted write */
        g4b_attn_isr_fires_ct++;
        k_sem_give(&g4b_attn_sem);
    }
}

uint32_t g4b_attn_isr_fires(void)
{
    return g4b_attn_isr_fires_ct;
}

void g4b_gpiote_attn_configure(void)
{
    NRF_GPIOTE->CONFIG[G4B_GPIOTE_ATTN_CH] = G4B_GPIOTE_CONFIG_ATTN;
    NRF_GPIOTE->EVENTS_IN[G4B_GPIOTE_ATTN_CH] = 0u;
    IRQ_CONNECT(GPIOTE_IRQn, G4B_GPIOTE_IRQ_PRIO, g4b_gpiote_attn_isr, NULL, 0);
    irq_enable(GPIOTE_IRQn);
    NRF_GPIOTE->INTENSET = BIT(G4B_GPIOTE_ATTN_CH);
    __DSB();
}

int g4b_attn_wait(uint32_t timeout_ms)
{
    return k_sem_take(&g4b_attn_sem, K_MSEC(timeout_ms));
}

#if (CONFIG_APEX_G4B_STM32_STOP1_IDLE_MS > 0) || \
    (CONFIG_APEX_G4B_STM32_IDLE_SCAN_PERIOD_MS > 1)
void g4b_attn_drain(void)
{
    /* The semaphore has a limit of one, but use a loop so the invariant stays
     * true if that limit is ever raised. An edge after this drain is retained
     * and wakes the subsequent bounded wait. */
    while (k_sem_take(&g4b_attn_sem, K_NO_WAIT) == 0) {
        /* drain */
    }
}
#endif

static NRF_GPIO_Type *port_regs(enum g4b_port port)
{
    return (port == G4B_PORT1) ? NRF_P1 : NRF_P0;
}

void g4b_pin_cfg(enum g4b_port port, enum g4b_pin pin, uint32_t cnf)
{
    port_regs(port)->PIN_CNF[(uint32_t)pin] = cnf;
}

void g4b_pin_set(enum g4b_port port, enum g4b_pin pin)
{
    port_regs(port)->OUTSET = BIT((uint32_t)pin);
}

void g4b_pin_clr(enum g4b_port port, enum g4b_pin pin)
{
    port_regs(port)->OUTCLR = BIT((uint32_t)pin);
}

bool g4b_pin_read(enum g4b_port port, enum g4b_pin pin)
{
    return (port_regs(port)->IN & BIT((uint32_t)pin)) != 0u;
}

uint32_t g4b_port_in(enum g4b_port port)
{
    return port_regs(port)->IN;
}

uint32_t g4b_port_dir(enum g4b_port port)
{
    return port_regs(port)->DIR;
}

uint32_t g4b_pin_cnf_read(enum g4b_port port, enum g4b_pin pin)
{
    return port_regs(port)->PIN_CNF[(uint32_t)pin];
}

void g4b_gpiote_ready_configure(void)
{
    NRF_GPIOTE->CONFIG[0] = G4B_GPIOTE_CONFIG_READY;
    NRF_GPIOTE->EVENTS_IN[0] = 0u;
}

void g4b_gpiote_ready_clear(void)
{
    NRF_GPIOTE->EVENTS_IN[0] = 0u;
    /* Read back to make sure the clear has landed before the next poll; the
     * event register is on the peripheral bus and a write is posted.
     */
    (void)NRF_GPIOTE->EVENTS_IN[0];
}

bool g4b_gpiote_ready_event(void)
{
    return NRF_GPIOTE->EVENTS_IN[0] != 0u;
}

uint32_t g4b_gpiote_config0(void)
{
    return NRF_GPIOTE->CONFIG[0];
}

/* Rails raised by the vendor loader before USB enumeration.
 *
 * Lifted from the loader at 0x6E388, which runs on its normal boot path:
 *
 *     cfg_output(0x19); set(0x19)   -> P0.25
 *     cfg_output(0x17); set(0x17)   -> P0.23
 *     cfg_output(0x13); set(0x13)   -> P0.19
 *
 * where 0x6E2C8 writes PIN_CNF[pin] = 3 and 0x6E36C writes OUTSET = 1 << pin.
 * PIN_CNF = 3 is output with the input buffer disconnected, i.e. G4B_CNF_EN_OUT.
 *
 * Live rail measurements determine whether P0.19/P0.23 also affect the USB
 * path; the package markings alone do not establish that connection.
 *
 * Safety comes from provenance, not from guessing: the loader configures these
 * three as outputs on every boot of this exact board, so none of them can be a
 * line the STM32 drives. Kept as file-local constants rather than added to
 * enum g4b_pin, so the header's property that the enum cannot express
 * P0.11/19/23 still holds and this file stays the only GPIO writer.
 *
 * PRE_KERNEL_1 because the loader raises them before USB comes up, and every
 * SYS_INIT runs before the USB workqueue starts.
 */
static int g4b_vendor_rails_init(void)
{
    /* Which lines to raise is a build-time mask so leave-one-out variants need
     * no source edit. Default covers P0.19/P0.23/P0.25, the loader's set.
     */
    uint32_t mask = (uint32_t)CONFIG_APEX_G4B_RAIL_MASK;

    for (uint32_t pin = 0U; pin < 32U; pin++) {
        if (mask & BIT(pin)) {
            NRF_P0->PIN_CNF[pin] = G4B_CNF_EN_OUT;
            NRF_P0->OUTSET = BIT(pin);
        }
    }

    return 0;
}

SYS_INIT(g4b_vendor_rails_init, PRE_KERNEL_1, 0);

/* Reset the USB path the Adafruit_nRF52_Bootloader leaves active.
 *
 * The bootloader runs USB in DFU mode (USBD + the POWER USB-regulator events).
 * When it branches to us, USBD stays enabled and a USBDETECTED event is left
 * pending. Zephyr's USB stack (nrfx_power/nrfx_usbd) then inits at POST_KERNEL
 * and enables the POWER IRQ - at which point that stale pending event fires
 * into an nrfx_power handler whose state is not fully set up yet, and faults
 * (observed as a precise bus fault during initial bootloader integration).
 *
 * Clearing USBD + the POWER USB events here, early, gives the USB stack a clean
 * peripheral to initialise from; it re-reads the live VBUS state (USBREGSTATUS)
 * and attaches normally. PRE_KERNEL_1 runs after VTOR is relocated to the app
 * and after INIT_ARCH_HW_AT_BOOT has masked the NVIC, and before the POST_KERNEL
 * USB init - the correct window. Only meaningful on the Adafruit-bootloader
 * build; on the stock loader (no USB) these registers are already at reset.
 */
static int g4b_usb_quiesce(void)
{
	NRF_USBD->INTENCLR = 0xFFFFFFFFu;
	NRF_USBD->ENABLE = 0u;
	NRF_POWER->INTENCLR = 0xFFFFFFFFu;
	NRF_POWER->EVENTS_USBDETECTED = 0u;
	NRF_POWER->EVENTS_USBPWRRDY = 0u;
	NRF_POWER->EVENTS_USBREMOVED = 0u;
	NVIC_ClearPendingIRQ(USBD_IRQn);
	NVIC_ClearPendingIRQ(POWER_CLOCK_IRQn);
	__DSB();
	return 0;
}

SYS_INIT(g4b_usb_quiesce, PRE_KERNEL_1, 1);

/* P1.07 boot strap, sampled once during startup.
 *
 * Stock reads this pin about a millisecond after configuring it as a pulled-up
 * input, and enters a serial recovery loop if it is pulled low. Legacy
 * vendor-loader test images use the same signal to stop feeding their inherited
 * watchdog. Production firmware uses the Adafruit bootloader and ignores it.
 *
 * Read ONCE and cached, deliberately. The strap is a boot-time decision, as it
 * is in stock, and a latched value cannot flap. Removing the strap resumes
 * normal operation at the next launch - and there is a launch every 60 s while
 * it is asserted, because that is what withholding the feed does.
 *
 * The delay is real. A pull-up has to charge the net before the level means
 * anything, and stock waits about a millisecond for the same reason.
 *
 * Fail-safe direction: the internal pull-up means an unconnected or floating
 * pad reads HIGH. A missing strap cannot affect a legacy test image; only a
 * deliberate short can.
 */
#define G4B_STRAP_PIN 7u /* P1.07 */

static bool strap_asserted;

static int g4b_strap_init(void)
{
    NRF_P1->PIN_CNF[G4B_STRAP_PIN] = G4B_PINCNF_DIR_INPUT |
                                     G4B_PINCNF_INBUF_CONN |
                                     G4B_PINCNF_PULL_UP;
    __DSB();
    k_busy_wait(1000);

    strap_asserted = (NRF_P1->IN & BIT(G4B_STRAP_PIN)) == 0u;

    /* Buffer back off once the answer is latched, as stock does. An input
     * buffer left connected on a pin nothing reads is a small constant cost.
     */
    NRF_P1->PIN_CNF[G4B_STRAP_PIN] = G4B_PINCNF_DIR_INPUT |
                                     G4B_PINCNF_INBUF_DISC |
                                     G4B_PINCNF_PULL_UP;
    __DSB();
    return 0;
}

/* Before g4b_mode_init at APPLICATION 90 and the watchdog at 98, so the very
 * first feed decision already knows.
 */
SYS_INIT(g4b_strap_init, APPLICATION, 89);

bool g4b_strap_asserted(void)
{
    return strap_asserted;
}

/* LED rails, raised and lowered independently in the vendor sequence:
 * P0.23 first on power-up, P0.19 first on power-down. Both are lowered for
 * light sleep and terminal power-off.
 *
 * The order is a real constraint; a settle delay is not. Stock has no delay
 * between P0.23 rising and P0.19 rising, and none between P0.19 rising and the
 * first SPI write to the LED controller - the delays elsewhere in that code sit
 * next to the STM32 resync, not next to these pins.
 *
 * P0.02, P1.05 and P0.25 are deliberately NOT touched here. Stock keeps all
 * three high through light sleep and drops them only on terminal power-off,
 * which is correct: the wake source is P0.24, the STM32 attention line, so the
 * scanner has to stay powered to produce it.
 */
#define G4B_RAIL_LED_DRIVER 19u /* raised last, lowered first */
#define G4B_RAIL_LED_ARRAY  23u /* raised first, lowered last */

void g4b_rgb_rail_up(void)
{
    NRF_P0->OUTSET = BIT(G4B_RAIL_LED_ARRAY);
    NRF_P0->OUTSET = BIT(G4B_RAIL_LED_DRIVER);
    __DSB();
}

void g4b_rgb_rail_down(void)
{
    NRF_P0->OUTCLR = BIT(G4B_RAIL_LED_DRIVER);
    NRF_P0->OUTCLR = BIT(G4B_RAIL_LED_ARRAY);
    __DSB();
}

/* RGB chip-select on P0.11.
 *
 * The IS31FL3743B hangs off SPIM2 (SCK P1.09, MOSI P1.08) with its hardware
 * CSN disconnected; the stock driver bit-bangs CS on P0.11 as a plain GPIO.
 * Confirmed from the stock SPIM2 device init at 0x296C8, which writes
 * PIN_CNF[11] = 3 (output). Kept here, with a file-local constant, so this file
 * stays the only writer of GPIO registers and enum g4b_pin need not learn a
 * pin it exists to exclude.
 *
 * Idle HIGH: the part latches on the rising edge of CS, so it must rest high
 * between frames.
 */
#define G4B_RGB_CS_PIN 11u

void g4b_rgb_cs_init(void)
{
    NRF_P0->OUTSET = BIT(G4B_RGB_CS_PIN);
    NRF_P0->PIN_CNF[G4B_RGB_CS_PIN] = G4B_CNF_EN_OUT;
    __DSB();
}

void g4b_rgb_cs_park(void)
{
    /* The controller rail is already down when this is called. Leaving CS as
     * an output cannot select anything useful, and can leak through an
     * unpowered input. Return the pad to reset-like high impedance until the
     * next rail-up sequence. OUT remains latched high, so cs_init() cannot
     * produce a low glitch when it restores output mode. */
    NRF_P0->PIN_CNF[G4B_RGB_CS_PIN] = G4B_CNF_IN_NOPULL;
    __DSB();
}

void g4b_rgb_cs_low(void)
{
    NRF_P0->OUTCLR = BIT(G4B_RGB_CS_PIN);
}

void g4b_rgb_cs_high(void)
{
    NRF_P0->OUTSET = BIT(G4B_RGB_CS_PIN);
}

/* --- Read-only pin survey ------------------------------------------------
 * Configure each candidate as an input, sample with pull-up and pull-down, then
 * leave it as an input without a pull. The survey does not write DIR or OUT.
 *
 * Reading the result:
 *   follows the pull (high with pull-up, low with pull-down) -> nothing is
 *       driving it and no external resistor dominates: FREE, and a candidate
 *       for I2C.
 *   stuck high or stuck low regardless -> something external wins. Either a
 *       driven signal or a strong pull. NOT free.
 * The internal pull is ~13 kOhm, so a weak external pull could still lose to
 * it; a pin that looks free should still be checked with a meter before
 * anything is soldered to it.
 */
/* Input, buffer connected, pull-up, drive S0D1 (open drain). Same value stock's
 * TWI driver writes for these two pins.
 */
#define G4B_CNF_TWI_BUS 0x0000060Cu

void g4b_twi_pins_claim(void)
{
    g4b_pin_cfg(G4B_PORT0, G4B_P0_TWI_SCL, G4B_CNF_TWI_BUS);
    g4b_pin_cfg(G4B_PORT0, G4B_P0_TWI_SDA, G4B_CNF_TWI_BUS);
}

void g4b_twi_pins_release(void)
{
    g4b_pin_cfg(G4B_PORT0, G4B_P0_TWI_SCL, G4B_CNF_IN_NOPULL);
    g4b_pin_cfg(G4B_PORT0, G4B_P0_TWI_SDA, G4B_CNF_IN_NOPULL);
}

void g4b_pin_survey(uint32_t p0_mask, uint32_t p1_mask,
                    uint32_t *p0_up, uint32_t *p1_up,
                    uint32_t *p0_down, uint32_t *p1_down)
{
    /* P0.18 is nRESET and must not be reconfigured by the survey. */
    p0_mask &= ~BIT(18);

    for (uint32_t i = 0u; i < 32u; i++) {
        if (p0_mask & BIT(i)) {
            NRF_P0->PIN_CNF[i] = G4B_PINCNF_DIR_INPUT | G4B_PINCNF_PULL_UP;
        }
        if (p1_mask & BIT(i)) {
            NRF_P1->PIN_CNF[i] = G4B_PINCNF_DIR_INPUT | G4B_PINCNF_PULL_UP;
        }
    }
    __DSB();
    k_busy_wait(1000); /* let the pull win against any pin capacitance */
    *p0_up = NRF_P0->IN & p0_mask;
    *p1_up = NRF_P1->IN & p1_mask;

    for (uint32_t i = 0u; i < 32u; i++) {
        if (p0_mask & BIT(i)) {
            NRF_P0->PIN_CNF[i] = G4B_PINCNF_DIR_INPUT | G4B_PINCNF_PULL_DOWN;
        }
        if (p1_mask & BIT(i)) {
            NRF_P1->PIN_CNF[i] = G4B_PINCNF_DIR_INPUT | G4B_PINCNF_PULL_DOWN;
        }
    }
    __DSB();
    k_busy_wait(1000);
    *p0_down = NRF_P0->IN & p0_mask;
    *p1_down = NRF_P1->IN & p1_mask;

    /* Leave every probed pin exactly as it was found: input, no pull. */
    for (uint32_t i = 0u; i < 32u; i++) {
        if (p0_mask & BIT(i)) {
            NRF_P0->PIN_CNF[i] = G4B_CNF_IN_NOPULL;
        }
        if (p1_mask & BIT(i)) {
            NRF_P1->PIN_CNF[i] = G4B_CNF_IN_NOPULL;
        }
    }
    __DSB();
}

/* --- Identify a pin with a multimeter -----------------------------------
 *
 * Finding which pad on the interposer is which pin, by measurement rather than
 * by counting package corners.
 *
 * Each candidate is driven with a DIFFERENT DUTY CYCLE. A multimeter on DC
 * averages the square wave, so every pin reads as its own steady voltage and a
 * pad is identified by one reading against a table - no blink-counting, no
 * timing, no ambiguity about which pin is active. 17 steps gives 194 mV
 * between neighbours, which any meter resolves.
 *
 * THIS DRIVES PINS, which the survey deliberately did not. It is only safe
 * because the survey already established that these sixteen follow an internal
 * pull in both directions, so nothing else is driving them. The six that did
 * not - P0.08, P0.16, P0.17, P0.20, P0.28, P0.31 - are NOT in this list and
 * must not be added to it: something external holds them, and driving against
 * that is how a board gets damaged.
 *
 * Standard drive strength, and P0.18 (nRESET) is absent by construction.
 */
/* P0.01, P0.09 and P1.07 follow an internal pull in both directions, but they
 * are not safe survey candidates:
 *
 *   P0.01  SPIM0 MISO, the SPI-NOR read line. The flash holds SO high-Z while
 *          its chip select (P0.26) idles high, so at survey time nothing was
 *          driving it. Driving it while a read is in flight is contention.
 *   P0.09  the recovery UART RXD. An unconnected receive line follows a pull
 *          perfectly, which is exactly what made it look free.
 *   P1.07  a boot strap, active low with an internal pull-up. "Follows the
 *          pull" is the signature of an UNASSERTED strap, not of a free pad.
 *
 * The electrical survey only proves that a pin is not driven at the time of the
 * test; it does not prove that the pin is unclaimed. Anything added
 * here must also be checked against the pin map in docs/HARDWARE.md.
 *
 * P0.08, P0.16, P0.17, P0.20, P0.28 and P0.31 are also excluded because they
 * failed the survey. Four have known functions:
 * P0.08 is SPIM2 MISO, P0.16/P0.17 are TWI1 to the charger, and P0.28 is the
 * charger's /CE. P0.18 (nRESET) is absent by construction.
 */
static const uint8_t beacon_p0[] = { 12, 13, 14, 15, 21, 22, 29, 30 };
static const uint8_t beacon_p1[] = { 1, 2, 3, 4, 6 };

/* One step per pin plus an all-off marker. 14 steps is 236 mV apart, which any
 * meter resolves comfortably.
 */
#define G4B_BEACON_STEPS 14u

void g4b_pin_beacon_init(void)
{
    for (uint32_t i = 0u; i < ARRAY_SIZE(beacon_p0); i++) {
        NRF_P0->OUTCLR = BIT(beacon_p0[i]);
        NRF_P0->PIN_CNF[beacon_p0[i]] = G4B_CNF_EN_OUT;
    }
    for (uint32_t i = 0u; i < ARRAY_SIZE(beacon_p1); i++) {
        NRF_P1->OUTCLR = BIT(beacon_p1[i]);
        NRF_P1->PIN_CNF[beacon_p1[i]] = G4B_CNF_EN_OUT;
    }
    __DSB();
}

/* Call once per millisecond. One full cycle is 17 ms, about 59 Hz, which every
 * meter averages without showing a wandering reading.
 */
void g4b_pin_beacon_tick(void)
{
    static uint32_t step;
    uint32_t p0_set = 0u, p0_clr = 0u, p1_set = 0u, p1_clr = 0u;

    for (uint32_t i = 0u; i < ARRAY_SIZE(beacon_p0); i++) {
        if (step < (i + 1u)) {
            p0_set |= BIT(beacon_p0[i]);
        } else {
            p0_clr |= BIT(beacon_p0[i]);
        }
    }
    for (uint32_t i = 0u; i < ARRAY_SIZE(beacon_p1); i++) {
        uint32_t duty = ARRAY_SIZE(beacon_p0) + i + 1u;

        if (step < duty) {
            p1_set |= BIT(beacon_p1[i]);
        } else {
            p1_clr |= BIT(beacon_p1[i]);
        }
    }

    NRF_P0->OUTSET = p0_set;
    NRF_P0->OUTCLR = p0_clr;
    NRF_P1->OUTSET = p1_set;
    NRF_P1->OUTCLR = p1_clr;

    step++;
    if (step >= G4B_BEACON_STEPS) {
        step = 0u;
    }
}
