/* SPDX-License-Identifier: MIT AND Apache-2.0
 * SPDX-FileCopyrightText: 2016-2023 Nordic Semiconductor ASA (USB errata helpers)
 *
 * STM32 scanner link and legacy diagnostic-stage runner. Production builds
 * bring up the link, replay the scanner configuration, and remain in the key
 * scan loop. The Nordic scan thread publishes health through
 * g4b_wdt_keyboard_heartbeat();
 * wdt_g4b.c is the only code that writes the watchdog reload register.
 */

#include <string.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
/* Also included further down for the flash-map work. Both are needed: the
 * switch-settings handler is near the top of this file and the include guard
 * makes the duplicate free.
 */
#include <zephyr/settings/settings.h>

#include <nrfx.h>

#include "evidence_g4b.h"
#include "mode_g4b.h"
#include "sleep_g4b.h"
#include "actuation_g4b.h"
#include "twi_g4b.h"
#include "pins_g4b.h"
#include "saadc_g4b.h"
#include "spim_g4b.h"
#include "wdt_g4b.h"
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
#include "rgb_g4b.h"
#include "rgb_fx_g4b.h"
#include "rgb_map_g4b.h"
#endif
#if IS_ENABLED(CONFIG_APEX_G4B_GAMEPAD)
#include "gamepad_g4b.h"
#endif
#if IS_ENABLED(CONFIG_APEX_G4B_SPINOR_DUMP) || \
    IS_ENABLED(CONFIG_APEX_G4B_SPINOR_WRITETEST)
#include "spinor_g4b.h"
#endif
#if IS_ENABLED(CONFIG_APEX_G4B_AB_ROLLBACK)
#include "ab_rollback_g4b.h"
#endif
#if IS_ENABLED(CONFIG_APEX_G4B_DONGLE_LINK)
#include "dongle_link_g4b.h"
#endif

BUILD_ASSERT(CONFIG_APEX_G4B_STAGE >= 0 && CONFIG_APEX_G4B_STAGE <= 7,
             "stages 0 through 7 are design-frozen");

/* One predicate controls scanner shutdown and entry into the permanent key
 * loop. Keeping those decisions together prevents the scanner from being
 * powered down immediately before normal input starts. */
#if CONFIG_APEX_G4B_STAGE == 3 && IS_ENABLED(CONFIG_APEX_G4B_KSCAN_INGEST)
#define G4B_KEYBOARD_BUILD 1
#else
#define G4B_KEYBOARD_BUILD 0
#endif

/* Lines that must never be outputs when the link is brought up.
 *
 * P0.18 is nRESET on the nRF52833. Driving it low is an instant reset loop and
 * nothing else in this design prevents it, so it is checked explicitly even
 * though pins_g4b.h makes it inexpressible.
 */
#define G4B_MUST_BE_INPUT_MASK (BIT(5) | BIT(7) | BIT(18) | BIT(24))

/* Pins neither this payload nor the vendor loader drives, so the ones worth
 * asking about. Derived from the loader's own P0.DIR = 0x0C000855 and
 * P1.DIR = 0x00000321 measured at boot, minus everything we use ourselves.
 *   P0: 1, 8, 9, 12-17, 20-22, 28-31
 *   P1: 1-4, 6, 7
 * P0.09 is an NFC pin by default, but P0.10 already works as the diagnostic
 * UART, which proves NFC is disabled in UICR - so it is ordinary GPIO and we
 * need not (and must not) write UICR to make it so.
 */
#define G4B_SURVEY_P0_MASK 0xF073F302u
#define G4B_SURVEY_P1_MASK 0x000000DEu

#define G4B_CPU_HZ 64000000u
#define G4B_READY_POLL_LIMIT (G4B_CPU_HZ / 2u) /* ~0.5 s of cycles */

static struct g4b_record record;

#if CONFIG_APEX_G4B_STAGE == 1
/* EasyDMA cannot read from flash, so the TX frames must live in RAM. nrfx has
 * an explicit guard for this; the direct-register path does not.
 */
static uint8_t s1_tx[G4B_SPIM_FRAME];
static uint8_t s1_rx[G4B_S1_EXCHANGES][G4B_SPIM_FRAME];
static struct g4b_record_s1 record_s1;

/* 0x90 first, exactly as stock does. Its response is ASCII "3.24.1", which
 * simultaneously proves clock polarity and phase, MSB-first bit order, byte
 * alignment, the two-phase framing, the ready handshake and the pin
 * assignment. Nothing else in the protocol is that diagnostic. 0xA0 follows
 * because its rx[1] reports whether the STM32 is already configured.
 */
static const uint8_t s1_opcodes[G4B_S1_EXCHANGES] = { 0x90u, 0xA0u };
#endif

#if CONFIG_APEX_G4B_STAGE == 2 || CONFIG_APEX_G4B_STAGE == 3
#include "apex_boot_prefix.h"

BUILD_ASSERT(APEX_BOOT_PREFIX_FRAMES == G4B_S2_PREFIX_FRAMES,
             "the frozen prefix and the record layout disagree on frame count");
BUILD_ASSERT(APEX_SPIM_FRAME_BYTES == G4B_SPIM_FRAME,
             "the frozen prefix and the SPIM driver disagree on frame size");

/* The boot beacon was a fixed low-RAM handoff from the stock loader's launcher
 * stage to the payload. The Adafruit bootloader has no launcher stage, so nothing
 * writes it and classify_launch*() always sees "no beacon" (the live reset reason
 * comes from the RESETREAS register instead). Backing it with an uninitialised
 * (.noinit) variable rather than a fixed 0x20002FE0 lets the linker place it
 * inside the app's RAM, so the app uses the full 128 KiB with no low-RAM
 * reservation for the (now-removed) s140. The read stays defensive: an
 * uninitialised magic practically never matches, so it reads as "no beacon". */
static volatile struct g4b_boot_beacon g4b_boot_beacon_ram __noinit;

/* Stock's pace. The link is far faster than this - measured per-phase ready
 * waits are 1 us and 33 us - and going faster than stock on the first stage
 * that writes to the slave buys nothing.
 */
#define G4B_S2_FRAME_PACE_US 8000u

/* G4B_S2_PREFIX_FRAMES (59) x G4B_S2_FRAME_PACE_US (8 ms) = 472 ms of actual
 * work. This is the ceiling the replay is allowed, measured from its own first
 * frame rather than from the start of the stage.
 */
#define G4B_S2_REPLAY_BUDGET_MS 3000u
#define G4B_S2_REPLAY_BUDGET_CYCLES (G4B_S2_REPLAY_BUDGET_MS * (G4B_CPU_HZ / 1000u))

/* The oracle plus the three scanner-state frames. */
static const uint8_t s2_verbatim_index[G4B_S2_VERBATIM_FRAMES] = { 0u, 29u, 57u, 58u };

static const uint8_t s2_confirm_opcodes[G4B_S2_CONFIRM_EXCHANGES] = { 0x90u, 0xA0u };

/* EasyDMA cannot read from flash, and apex_boot_prefix[] is const. Each frame
 * must be copied into RAM before it is transmitted - a pointer into the table
 * would produce a silent zero-length transfer.
 */
static uint8_t s2_tx[G4B_SPIM_FRAME];
static uint8_t s2_rx[G4B_SPIM_FRAME];
static struct g4b_record_s2_replay record_s2r;
static struct g4b_record_s2_confirm record_s2c;

/* Set on the run path, read on the emit path. Initialised to the read-only
 * kind so that an abort before classification emits the harmless record rather
 * than a replay record full of zeroes.
 */
static uint32_t s2_launch_kind = G4B_LAUNCH_CONFIRM_NO_BEACON;
#endif

#if CONFIG_APEX_G4B_STAGE == 3
#include <zephyr/device.h>

#include "kscan_g4b.h"

#if IS_ENABLED(CONFIG_APEX_G4B_KSCAN_INGEST)
#include <zmk/ble.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_USB) || IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/endpoints.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#include <hal/nrf_power.h>
#include <zephyr/usb/usb_device.h>
#include <nrf_erratas.h>
#endif

/* Minimum spacing between A1 exchanges, and a hard cap on how many run.
 *
 * These apply ONLY to the two diagnostic busy-wait loops - s3_chord_window and
 * s3_run_poll - neither of which has a k_msleep anywhere. Without the gap they
 * would clock the STM32 bus continuously for the whole window, and s3_run_poll
 * would burn its exchange budget in under a second and then go blind for the
 * rest of the deadline.
 *
 * The permanent keyboard loop deliberately does NOT use them. It is ATTN-gated
 * and sleeps 1 ms every iteration, which is the same one-read-per-scheduler-tick
 * cadence stock runs. Do not "restore symmetry" by adding the gap back there,
 * and do not delete this constant as dead - it is live in the two loops above.
 */
#define G4B_S3_A1_MIN_GAP_US 2000u
#define G4B_S3_A1_MAX 2000u

/* Minimum interval between scanner liveness polls. */
#define G4B_S3_A0_GAP_US 50000u

static struct g4b_record_s3 record_s3;
static uint8_t s3_prev_bitmap[APEX_G4B_KEY_BITMAP_SIZE];
static bool s3_have_prev;

#if IS_ENABLED(CONFIG_APEX_G4B_KSCAN_INGEST)
static const struct device *s3_kscan =
    DEVICE_DT_GET_ANY(steelseries_apex_g4b_spim_kscan);
#endif

/* Use the Nordic's physical VBUS detector for power policy. usb_device_next can
 * leave its software status at USB_DC_SUSPEND after cable removal; treating
 * that stale state as powered prevents every Bluetooth/battery-only saving.
 * USBREGSTATUS.VBUSDETECT changes with the cable and has no logging side effect.
 */
static bool s3_usb_is_powered(void)
{
#if IS_ENABLED(CONFIG_ZMK_USB)
    return (*(volatile uint32_t *)G4B_USBREGSTATUS & BIT(0)) != 0u;
#else
    return false;
#endif
}
#endif

static void settle(void)
{
    /* A few microseconds for an internal pull to charge the line. The pull is
     * roughly 13 kOhm; with a few pF of trace this settles in well under a
     * microsecond, so this is generous.
     */
    k_busy_wait(50);
}

/* Returns true if the line reads high. Used with pullup then pulldown: a line
 * that follows the pull in both directions is floating; a line that ignores
 * the pull is being driven by the STM32.
 */
static bool sample_with_pull(enum g4b_pin pin, uint32_t pull)
{
    bool level;

    g4b_pin_cfg(G4B_PORT0, pin, G4B_PINCNF_DIR_INPUT | pull);
    settle();
    level = g4b_pin_read(G4B_PORT0, pin);
    g4b_pin_cfg(G4B_PORT0, pin, G4B_CNF_IN_NOPULL);

    return level;
}

static void pull_test(uint8_t *ready_up, uint8_t *ready_down,
                      uint8_t *attn_up, uint8_t *attn_down)
{
    *ready_up = sample_with_pull(G4B_P0_READY, G4B_PINCNF_PULL_UP) ? 1u : 0u;
    *ready_down = sample_with_pull(G4B_P0_READY, G4B_PINCNF_PULL_DOWN) ? 1u : 0u;
    *attn_up = sample_with_pull(G4B_P0_ATTN, G4B_PINCNF_PULL_UP) ? 1u : 0u;
    *attn_down = sample_with_pull(G4B_P0_ATTN, G4B_PINCNF_PULL_DOWN) ? 1u : 0u;
}

/* Bring the pins to the state captured from stock, in stock's order. Set both
 * enable output latches low before configuring the pins, then assert them high.
 */
static void bringup_pins(void)
{
    /* Inputs first, so nothing is briefly driven against the STM32. */
    g4b_pin_cfg(G4B_PORT0, G4B_P0_READY, G4B_CNF_IN_NOPULL);
    g4b_pin_cfg(G4B_PORT0, G4B_P0_ATTN, G4B_CNF_IN_NOPULL);
    g4b_pin_cfg(G4B_PORT0, G4B_P0_MISO, G4B_CNF_MISO_IN);

    /* Clock and data idle low, as outputs, before the enables rise. */
    g4b_pin_clr(G4B_PORT0, G4B_P0_SCK);
    g4b_pin_clr(G4B_PORT0, G4B_P0_MOSI);
    g4b_pin_cfg(G4B_PORT0, G4B_P0_SCK, G4B_CNF_SCK_OUT);
    g4b_pin_cfg(G4B_PORT0, G4B_P0_MOSI, G4B_CNF_MOSI_OUT);

    /* Enables: drive low, configure as outputs, then raise. */
    g4b_pin_clr(G4B_PORT0, G4B_P0_EN);
    g4b_pin_clr(G4B_PORT1, G4B_P1_EN);
    g4b_pin_cfg(G4B_PORT0, G4B_P0_EN, G4B_CNF_EN_OUT);
    g4b_pin_cfg(G4B_PORT1, G4B_P1_EN, G4B_CNF_EN_OUT);

    g4b_gpiote_ready_configure();
}

static void enable_stm32(void)
{
    g4b_pin_set(G4B_PORT0, G4B_P0_EN);
    g4b_pin_set(G4B_PORT1, G4B_P1_EN);
    __DSB();
}

/* Stock's shutdown path: enables back low, clock and data released. Leaves the
 * board close to how it was found, so a failed stage does not hand the vendor
 * loader an unusual pin state on the next boot.
 */
#if !G4B_KEYBOARD_BUILD
static void shutdown_pins(void)
{
    g4b_pin_clr(G4B_PORT0, G4B_P0_EN);
    g4b_pin_clr(G4B_PORT1, G4B_P1_EN);
    g4b_pin_cfg(G4B_PORT0, G4B_P0_SCK, G4B_CNF_IN_NOPULL);
    g4b_pin_cfg(G4B_PORT0, G4B_P0_MOSI, G4B_CNF_IN_NOPULL);
    g4b_pin_cfg(G4B_PORT0, G4B_P0_MISO, G4B_CNF_IN_NOPULL);
    g4b_pin_cfg(G4B_PORT0, G4B_P0_EN, G4B_CNF_IN_NOPULL);
    g4b_pin_cfg(G4B_PORT1, G4B_P1_EN, G4B_CNF_IN_NOPULL);
    __DSB();
}
#else
/* Restore the STM32 enable pins before entering the keyboard loop.
 *
 * g4b_main normally leaves these pins high. Reapplying the state here also
 * covers error paths that return with the scanner disabled; SPI then completes
 * without an error but reports no keys.
 *
 * Two details matter:
 *
 *  - The enables go HIGH in OUT before PIN_CNF makes them outputs, so a pin
 *    already driving high stays high and a floated pin comes up without a low
 *    edge in between. bringup_pins() deliberately does the opposite (clr, cfg,
 *    then set) because at boot the STM32 must be taken through a reset. Here a
 *    low pulse would RESET it and discard the configuration s2_run_replay
 *    wrote; :1051 is the runtime test for it (rx[1] == 1 means configured).
 *  - SCK/MOSI/MISO are left alone. g4b_spim_enable() already sets their levels
 *    and then their PIN_CNF before it touches the peripheral (spim_g4b.c:78-82),
 *    in nrfx's order. Duplicating that here would only create a second place
 *    for it to be wrong.
 */
static void keyboard_link_reassert(void)
{
    uint32_t t0;

    /* The two inputs, restated. Cheap, and makes this function true on its own
     * terms rather than true because of what ran before it.
     */
    g4b_pin_cfg(G4B_PORT0, G4B_P0_READY, G4B_CNF_IN_NOPULL);
    g4b_pin_cfg(G4B_PORT0, G4B_P0_ATTN, G4B_CNF_IN_NOPULL);

    /* Level first, direction second. See the note above: the order is the
     * point, not an accident of style.
     */
    g4b_pin_set(G4B_PORT0, G4B_P0_EN);
    g4b_pin_set(G4B_PORT1, G4B_P1_EN);
    g4b_pin_cfg(G4B_PORT0, G4B_P0_EN, G4B_CNF_EN_OUT);
    g4b_pin_cfg(G4B_PORT1, G4B_P1_EN, G4B_CNF_EN_OUT);
    __DSB();

    /* GPIOTE ch0 survives on its own - nothing on this path rewrites CONFIG[0]
     * or P0.05's PIN_CNF - but re-issuing it is two register writes and it
     * clears EVENTS_IN[0] (pins_g4b.c), so an edge latched while the enables
     * were being restored cannot be read as the first frame's ready.
     */
    g4b_gpiote_ready_configure();

    /* Bounded wait for READY, the same ~0.5 s window bring-up uses. Ready is a
     * LEVEL and holds once raised (spim_g4b.c:37-52), so on the normal path -
     * enables never dropped, STM32 already up - this exits on the first read and
     * costs one GPIO read. If the enables really had been dropped, this is where
     * the ~80 ms enable-to-ready measured at bring-up gets spent once, instead
     * of being paid as a failed exchange every iteration. A timeout is
     * deliberately not fatal: every exchange has its own 250 ms ready wait and
     * the loop simply retries.
     */
    t0 = g4b_cyccnt();
    while (!g4b_pin_read(G4B_PORT0, G4B_P0_READY) &&
           (g4b_cyccnt() - t0) < G4B_READY_POLL_LIMIT) {
        /* spin */
    }
    g4b_gpiote_ready_clear();
}
#endif

#if CONFIG_APEX_G4B_STAGE == 2 || CONFIG_APEX_G4B_STAGE == 3
/* SREQ means the boot followed the vendor updater's reset command, so this is
 * the first launch after a flash and the STM32 is cold - the only conditions
 * under which the frozen prefix's recorded responses apply.
 *
 * Both misclassifications fail safely: a false REPLAY resends frames stock
 * sends on every boot, and a false CONFIRM writes nothing at all. An absent
 * magic defaults to read-only, because for the first stage that writes to the
 * slave, failing closed means failing toward not writing.
 */
static uint32_t classify_launch(uint32_t *resetreas)
{
    const volatile struct g4b_boot_beacon *beacon = &g4b_boot_beacon_ram;

    if (beacon->magic != G4B_BOOT_BEACON_MAGIC) {
        *resetreas = 0u;
        return G4B_LAUNCH_CONFIRM_NO_BEACON;
    }

    *resetreas = beacon->resetreas;

    return (*resetreas & G4B_RESETREAS_SREQ) ? G4B_LAUNCH_REPLAY
                                             : G4B_LAUNCH_CONFIRM;
}

/* Hold the frame cadence measured from the start of the frame, so the pace does
 * not drift with however long the exchange itself took.
 */
static void pace_frame(uint32_t frame_start)
{
    while (((g4b_cyccnt() - frame_start) / (G4B_CPU_HZ / 1000000u))
           < G4B_S2_FRAME_PACE_US) {
        /* spin */
    }
}

static void s2_fill_head(struct g4b_s2_head *head, uint16_t version,
                         uint32_t resetreas, uint32_t launch_kind)
{
    head->magic = G4B_EVIDENCE_MAGIC;
    head->version = version;
    head->stage = CONFIG_APEX_G4B_STAGE;
    head->launch_ms = record.launch_ms;
    head->last_error = record.last_error;
    head->bringup_ok = record.bringup_ok;
    head->dwt_enable_to_ready = record.dwt_enable_to_ready;
    head->resetreas = resetreas;
    head->launch_kind = launch_kind;
}

/* Replay the frozen prefix byte for byte, comparing all 64 bytes of every
 * response. Abort on the first mismatch: continuing to push configuration into
 * a slave that is not answering as recorded is precisely the case where we no
 * longer know what we are doing.
 */
/* Adjustable actuation point.
 *
 * Six frames of the stock prefix carry per-key press/release points: opcodes
 * 0x30 and 0x33, each sent three times to cover all 70 key positions in runs of
 * 30, 30 and 10, and the whole set sent twice (bank 0x00 then 0x20). The frame
 * is { opcode, count, start, bank } followed by two bytes per key, press then
 * release, which is why a 30-key frame is exactly 4 + 60 = 64 bytes.
 *
 * Stock fills every pair with 50/46. Those are not field levels: the scanner
 * resolves them against each key's own factory calibration as
 * LO + value * (HI - LO) / 255, so one number means the same fraction of travel
 * on every key and every unit. That is what makes rewriting them globally
 * reasonable, and it is the whole feature - the scanner already does the
 * comparison, so changing where a key actuates needs no analog readout at all.
 *
 * Patched here rather than in apex_boot_prefix.h on purpose. That header is a
 * frozen capture whose sha256 verify_g4b.py re-derives, and it doubles as the
 * replay's reference response. Editing it would break both. Patching the TX
 * copy is safe because the reply does NOT echo the data: a threshold frame
 * answers { opcode, count, start } and then zeros, so expect_rx is unaffected
 * and the byte-compare still proves the link is healthy.
 *
 * Defaults are 50/46, so an unconfigured build puts identical bytes on the wire
 * to stock. Both handlers touch STM32 RAM only - nothing here can reach its
 * flash - so a bad value is undone by a power cycle.
 */
BUILD_ASSERT(CONFIG_APEX_G4B_ACTUATION_PRESS > CONFIG_APEX_G4B_ACTUATION_RELEASE,
             "actuation press point must be deeper than the release point, or a "
             "key resting on the threshold will chatter");

#define G4B_THRESH_HEADER 4u

/* Actuation and rapid trigger are RUNTIME state now, not build-time constants.
 *
 * The Kconfigs still exist and still set the boot values - what changed is that
 * the keymap can move them afterwards, which is what the keycaps have always
 * promised. The legends on I and O say actuation up and down, and on T rapid
 * trigger; those keys were left inert because we could only do this at build
 * time.
 *
 * THE LADDER IS A TABLE, NOT ARITHMETIC. The scanner does not take millimetres.
 * These bytes are indices into a 256-entry lookup table in the STM32's own
 * flash at 0x0800D3E0, and the mapping is not linear: 16 is 1.0 mm, 29 is
 * 1.5 mm, 46 is 2.0 mm, 50 is 2.1 mm (stock), 70 is 2.5 mm, 110 is 3.0 mm.
 * Stepping by arithmetic would be inventing depths we have never measured, so
 * the ladder contains only indices whose depth we actually read out of that
 * table.
 */
static const uint8_t g4b_act_steps[] = { 16u, 29u, 46u, 50u, 70u, 110u };
#define G4B_ACT_STEP_DEFAULT 3u  /* index 3 */
#define G4B_ACT_PRESS_DEFAULT 50u /* ...which is 50, stock's 2.1 mm */

/* Indexing the array here would not be a constant expression in C, so the
 * default is spelled out and checked against the Kconfig instead. If they ever
 * disagree, the first press of the actuation control jumps to somewhere the
 * board was not already at.
 */
BUILD_ASSERT(G4B_ACT_PRESS_DEFAULT == CONFIG_APEX_G4B_ACTUATION_PRESS,
             "the default ladder step must equal the configured press point");

/* The reset point, as a gap below the press point.
 *
 * Stock ships 50 press / 46 release, so a gap of 4. Expressed as a gap rather
 * than an absolute index deliberately: the pair has to keep press > release at
 * every actuation setting or a key resting exactly on the threshold chatters,
 * and a gap cannot violate that no matter where the press point moves.
 *
 * The ladder is index space, not depth space - near stock 4 counts is roughly
 * 0.1 mm - but the lookup table is monotonic, so a smaller number is always a
 * shorter reset travel and a larger one is always more margin against chatter.
 * 2 is twitchy, 8 is deliberate; stock's 4 is the default and the middle.
 */
static const uint8_t g4b_reset_gaps[] = { 2u, 4u, 6u, 8u };
#define G4B_RESET_GAP_DEFAULT 1u /* index 1 = 4 = stock */

static uint8_t s3_reset_gap = G4B_RESET_GAP_DEFAULT;

static uint8_t s3_act_step = G4B_ACT_STEP_DEFAULT;
static uint8_t s3_rt_tenths = CONFIG_APEX_G4B_RAPID_TRIGGER;
static uint8_t s3_rt_last_on =
    (CONFIG_APEX_G4B_RAPID_TRIGGER > 0) ? CONFIG_APEX_G4B_RAPID_TRIGGER : 3;

/* Set from ZMK's thread, consumed by the g4b thread. Single byte, and the only
 * transition that matters is 0 -> 1, so this needs no more than volatile.
 */
static volatile uint8_t s3_cfg_dirty;

/* Shell-requested component resets / power, serviced from the ATTN-low branch of
 * the keyboard loop (the single-writer-safe point where nothing is queued). The
 * shell thread only sets these flags; the g4b thread owns the pins and SPIM. */
static volatile uint8_t s3_req_stm32_reset;
static volatile uint8_t s3_req_rgb_reset;
static volatile uint8_t s3_req_usb_reset;
static volatile int8_t  s3_req_rgb_rail = -1; /* -1 none, 0 = power down, 1 = up */

/* Shell-requested RAW STM32 frame exchange (a debug tool for the scanner
 * protocol). The shell fills s3_raw_tx and sets pending; the g4b thread runs one
 * 64-byte exchange at the same single-writer-safe point and returns the reply.
 * The shell guards the dangerous opcodes (0x01/0x02/0x32) before requesting. */
static volatile uint8_t s3_req_raw_pending;
static volatile uint8_t s3_req_raw_done;
static volatile uint8_t s3_req_raw_ok;
static uint8_t s3_raw_tx[G4B_SPIM_FRAME];
static uint8_t s3_raw_rx[G4B_SPIM_FRAME];

void g4b_request_stm32_reset(void)
{
    s3_req_stm32_reset = 1u;
}
void g4b_request_usb_rail_reset(void)
{
    s3_req_usb_reset = 1u;
}
void g4b_request_rgb_reset(void)
{
    s3_req_rgb_reset = 1u;
}
void g4b_request_rgb_rail(bool on)
{
    s3_req_rgb_rail = on ? 1 : 0;
}

/* Send one raw 64-byte frame to the STM32 and return its reply. Called from the
 * shell thread; the actual exchange runs on the g4b thread (single-writer). tx is
 * zero-padded to a full frame. Returns 0 on a clean exchange, negative on error. */
int g4b_scan_raw(const uint8_t *tx, uint32_t len, uint8_t *rx, uint32_t timeout_ms)
{
    if (tx == NULL || rx == NULL || len == 0u || len > G4B_SPIM_FRAME) {
        return -EINVAL;
    }
    if (s3_req_raw_pending) {
        return -EBUSY;
    }
    memset(s3_raw_tx, 0, sizeof(s3_raw_tx));
    memcpy(s3_raw_tx, tx, len);
    s3_req_raw_done = 0u;
    s3_req_raw_ok = 0u;
    __DMB();
    s3_req_raw_pending = 1u; /* hand off to the g4b thread */

    uint32_t waited = 0u;
    while (!s3_req_raw_done && waited < timeout_ms) {
        k_msleep(2);
        waited += 2u;
    }
    if (!s3_req_raw_done) {
        s3_req_raw_pending = 0u;
        return -ETIMEDOUT;
    }
    memcpy(rx, s3_raw_rx, G4B_SPIM_FRAME);
    return s3_req_raw_ok ? 0 : -EIO;
}

/* Full scanner re-bring-up (the 59-frame boot config replay). Defined below;
 * forward-declared so the shell raw-frame recovery can re-arm the scanner in
 * place without a whole reboot. */
static void s2_run_replay(uint32_t start, uint32_t deadline_cycles);

/* Runs on the g4b thread from the ATTN-low branch. Owns the pins and SPIM here,
 * so component resets are safe. */
static void s3_service_shell_requests(void)
{
    if (s3_req_stm32_reset) {
        s3_req_stm32_reset = 0u;
        /* Drop the STM32 enable lines for a full 250 ms - a real power-off so the
         * scanner actually resets and reboots (a short glitch leaves it in a bad
         * state and the keyboard stops typing). Its RAM actuation/RT/fx config is
         * lost across this, so mark it dirty to re-send once it is back. */
        g4b_pin_clr(G4B_PORT0, G4B_P0_EN);
        g4b_pin_clr(G4B_PORT1, G4B_P1_EN);
        k_msleep(250);
        g4b_pin_set(G4B_PORT0, G4B_P0_EN);
        g4b_pin_set(G4B_PORT1, G4B_P1_EN);
        s3_cfg_dirty = 1u;
    }
    if (s3_req_usb_reset) {
        s3_req_usb_reset = 0u;
        /* Pulse the USB data-path rail (P0.25) low then high to force a USB
         * re-enumeration. It is ALWAYS restored in this same operation - never
         * left low, or USB (and this shell) would be gone until a power cycle. */
        NRF_P0->OUTCLR = BIT(25);
        k_msleep(150);
        NRF_P0->OUTSET = BIT(25);
    }
    if (s3_req_raw_pending) {
        /* One raw 64-byte scanner exchange for the shell. Safe here: ATTN is low
         * so nothing is queued, and we own the SPIM. The shell already refused the
         * dangerous opcodes before setting this. */
        struct g4b_exchange_stats stats = {0};
        g4b_spim_arm_ready();
        (void)g4b_spim_exchange(s3_raw_tx, s3_raw_rx, &stats);
        s3_req_raw_ok = (stats.result == G4B_SPIM_OK) ? 1u : 0u;

        /* A raw frame injected outside the loop's own rhythm (an 0xA2 lowers ATTN,
         * an 0xA1 consumes an event) leaves the scanner's event state stuck, so
         * keys stop reaching us until the next full bring-up. Just re-issuing the
         * enable is not enough - the event state only re-arms on the full config
         * sequence - so re-run the whole 59-frame boot replay here (~0.5 s, the
         * same thing a reboot does), so a probe can never strand the keyboard. */
        s2_run_replay(g4b_cyccnt(), G4B_S2_REPLAY_BUDGET_CYCLES);
        s3_cfg_dirty = 1u;

        s3_req_raw_pending = 0u;
        __DMB();
        s3_req_raw_done = 1u;
    }
#if IS_ENABLED(CONFIG_APEX_G4B_RGB)
    if (s3_req_rgb_reset) {
        s3_req_rgb_reset = 0u;
        g4b_rgb_rail_down();
        k_msleep(10);
        g4b_rgb_rail_up();
        g4b_rgb_bringup();
    }
    if (s3_req_rgb_rail >= 0) {
        int8_t want = s3_req_rgb_rail;
        s3_req_rgb_rail = -1;
        if (want == 0) {
            g4b_rgb_rail_down();
        } else {
            g4b_rgb_rail_up();
            g4b_rgb_bringup();
        }
    }
#endif
}

static void s2_apply_actuation(uint8_t *tx)
{
    uint32_t count;

    if (tx[0] != 0x30u && tx[0] != 0x33u) {
        return;
    }

    count = tx[1];
    if (count == 0u ||
        (G4B_THRESH_HEADER + count * 2u) > G4B_SPIM_FRAME) {
        /* Not the layout this was written against. Leave the frame exactly as
         * captured rather than corrupting a frame we do not understand.
         */
        return;
    }

    for (uint32_t k = 0u; k < count; k++) {
        uint8_t press = g4b_act_steps[s3_act_step];

        tx[G4B_THRESH_HEADER + k * 2u] = press;
        tx[G4B_THRESH_HEADER + k * 2u + 1u] =
            (uint8_t)(press - g4b_reset_gaps[s3_reset_gap]);
    }
}

/* Rapid trigger.
 *
 * The scanner implements it natively, so this is configuration and nothing
 * else - no analog readout, no direction state machine here, no extra bus
 * traffic. Opcode 0x35 carries { key_id, sensitivity } pairs after a one-byte
 * count, and sensitivity is a re-trigger distance in tenths of a millimetre:
 * 1..40 turns rapid trigger on for that key, 0 turns it off.
 *
 * Once on, a key re-fires as soon as it moves back down by that distance from
 * wherever it stopped, instead of having to come back up past a fixed release
 * point. The scanner tracks the turning point per key in its own travel array.
 *
 * Stock sends 0 for all 63 real keys, so stock has rapid trigger OFF. The seven
 * positions that carry 2 are unpopulated sensors echoing the scanner's own init
 * default. That gives us a clean rule that needs no hardcoded key list: rewrite
 * a record only where stock left it at 0, which is exactly the set of real keys.
 *
 * Interaction worth knowing, because it changes what the actuation point means:
 * with rapid trigger on, the 0x30 press code is only the ARMING depth, used for
 * the first press from full rest, and the 0x30 release code is not consulted at
 * all. Release becomes "moved up by the sensitivity", with a hard override that
 * a key at the top always releases.
 */
BUILD_ASSERT(CONFIG_APEX_G4B_RAPID_TRIGGER >= 0 &&
                 CONFIG_APEX_G4B_RAPID_TRIGGER <= 40,
             "rapid trigger sensitivity is tenths of a millimetre: 0 disables, "
             "1 to 40 enables; the scanner clears the enable bit above 40");

#define G4B_RT_HEADER 2u

static void s2_apply_rapid_trigger(uint8_t *tx)
{
    uint32_t n;

    if (tx[0] != 0x35u) {
        return;
    }

    n = tx[1];
    if (n == 0u || (G4B_RT_HEADER + n * 2u) > G4B_SPIM_FRAME) {
        return;
    }

    for (uint32_t i = 0u; i < n; i++) {
        uint8_t *rec = &tx[G4B_RT_HEADER + i * 2u];

        /* Only where stock disabled it. The positions stock leaves at the
         * scanner's default of 2 are unpopulated sensors, and writing a real
         * sensitivity to a sensor that does not exist has no purpose.
         */
        if (rec[1] == 0u) {
            rec[1] = s3_rt_tenths;
        }
    }
}

static void s2_run_replay(uint32_t start, uint32_t deadline_cycles)
{
    record_s2r.first_mismatch_index = G4B_S2_NO_INDEX;
    record_s2r.first_mismatch_offset = G4B_S2_NO_INDEX;

    for (uint32_t i = 0u; i < G4B_S2_PREFIX_FRAMES; i++) {
        struct g4b_exchange_stats stats = {0};
        uint32_t frame_start = g4b_cyccnt();
        enum g4b_spim_result result;

        memcpy(s2_tx, apex_boot_prefix[i].tx, G4B_SPIM_FRAME);
        s2_apply_actuation(s2_tx);
        s2_apply_rapid_trigger(s2_tx);
        memset(s2_rx, 0, sizeof(s2_rx));

        g4b_spim_arm_ready();
        result = g4b_spim_exchange(s2_tx, s2_rx, &stats);
        record_s2r.frames_run = i + 1u;

        if (result != G4B_SPIM_OK) {
            record_s2r.frame_result[i] = G4B_FRAME_SPIM_ERROR;
            record_s2r.first_mismatch_index = i;
            memcpy(record_s2r.mismatch_expected, apex_boot_prefix[i].expect_rx,
                   G4B_SPIM_FRAME);
            memcpy(record_s2r.mismatch_actual, s2_rx, G4B_SPIM_FRAME);
            record.last_error = G4B_ERR_REPLAY;
            return;
        }

        if (memcmp(s2_rx, apex_boot_prefix[i].expect_rx, G4B_SPIM_FRAME) != 0) {
            record_s2r.frame_result[i] = G4B_FRAME_MISMATCH;
            record_s2r.first_mismatch_index = i;
            for (uint32_t off = 0u; off < G4B_SPIM_FRAME; off++) {
                if (s2_rx[off] != apex_boot_prefix[i].expect_rx[off]) {
                    record_s2r.first_mismatch_offset = off;
                    break;
                }
            }
            memcpy(record_s2r.mismatch_expected, apex_boot_prefix[i].expect_rx,
                   G4B_SPIM_FRAME);
            memcpy(record_s2r.mismatch_actual, s2_rx, G4B_SPIM_FRAME);
            record.last_error = G4B_ERR_REPLAY;
            return;
        }

        record_s2r.frame_result[i] = G4B_FRAME_MATCH;
        record_s2r.frames_matched++;

        for (uint32_t v = 0u; v < G4B_S2_VERBATIM_FRAMES; v++) {
            if (s2_verbatim_index[v] == i) {
                memcpy(record_s2r.verbatim[v], s2_rx, G4B_SPIM_FRAME);
            }
        }

        if ((g4b_cyccnt() - start) >= deadline_cycles) {
            record.last_error = G4B_ERR_DEADLINE;
            return;
        }

        pace_frame(frame_start);
    }
}

/* The positive control for rx[1].
 *
 * Sent on the replay launch itself, after frame 58, before anything resets.
 * Deliberately sent whether or not the replay completed: 0xA0 is read-only, and
 * knowing the reported state after a replay that aborted is worth as much as
 * knowing it after one that finished. frames_matched in the same record says
 * which case this was.
 */
static void s2_post_replay_probe(void)
{
    struct g4b_exchange_stats stats = {0};

    memset(s2_tx, 0, sizeof(s2_tx));
    s2_tx[0] = 0xA0u;
    memset(s2_rx, 0, sizeof(s2_rx));

    g4b_spim_arm_ready();
    (void)g4b_spim_exchange(s2_tx, s2_rx, &stats);

    memcpy(record_s2r.post_replay_a0.rx, s2_rx, G4B_SPIM_FRAME);
    record_s2r.post_replay_a0.tx_amount = stats.tx_amount;
    record_s2r.post_replay_a0.rx_amount = stats.rx_amount;
    record_s2r.post_replay_a0.events_end = stats.events_end;
    record_s2r.post_replay_a0.result = stats.result;
    record_s2r.post_replay_a0.wait_tx_us = stats.wait_tx_us;
    record_s2r.post_replay_a0.wait_rx_us = stats.wait_rx_us;
}

/* Each diagnostic relaunch samples 0xA0 to determine whether the STM32 retained
 * its configuration across the Nordic reset.
 *
 * __maybe_unused is used instead of a narrower conditional-compilation guard.
 * This function is compiled for stages 2 and 3 and called only by stage 2
 * (the #else arm at the s2_run_replay/s2_run_confirm branch), so a stage-3
 * build warns "defined but not used". The obvious fix - move it and its data
 * inside a stage-2-only guard - means adding conditional-compilation
 * boundaries beside the stage-3 keyboard call. Keeping that call outside a
 * fragile nested guard lets -Wunused-function continue to catch its accidental
 * removal; CMakeLists.txt promotes that warning to an error for keyboard builds.
 */
__maybe_unused static void s2_run_confirm(uint32_t start,
                                          uint32_t deadline_cycles)
{
    for (int i = 0; i < G4B_S2_CONFIRM_EXCHANGES; i++) {
        struct g4b_exchange_stats stats = {0};

        memset(s2_tx, 0, sizeof(s2_tx));
        s2_tx[0] = s2_confirm_opcodes[i];
        record_s2c.opcode[i] = s2_confirm_opcodes[i];

        memset(s2_rx, 0, sizeof(s2_rx));
        g4b_spim_arm_ready();
        (void)g4b_spim_exchange(s2_tx, s2_rx, &stats);

        memcpy(record_s2c.exchange[i].rx, s2_rx, G4B_SPIM_FRAME);
        record_s2c.exchange[i].tx_amount = stats.tx_amount;
        record_s2c.exchange[i].rx_amount = stats.rx_amount;
        record_s2c.exchange[i].events_end = stats.events_end;
        record_s2c.exchange[i].result = stats.result;
        record_s2c.exchange[i].wait_tx_us = stats.wait_tx_us;
        record_s2c.exchange[i].wait_rx_us = stats.wait_rx_us;

        if ((g4b_cyccnt() - start) >= deadline_cycles) {
            record.last_error = G4B_ERR_DEADLINE;
            return;
        }
    }
}
#endif

#if CONFIG_APEX_G4B_STAGE == 3
static uint32_t elapsed_us_since(uint32_t mark)
{
    return (g4b_cyccnt() - mark) / (G4B_CPU_HZ / 1000000u);
}

/* Poll for key events until the deadline.
 *
 * P0.24 high means the STM32 has an A1 event queued, matching stock's gate.
 * When it is low, a rate-limited 0xA0
 * confirms the STM32 is still alive and still configured, so that an absence of
 * key events can be distinguished from a slave that quietly dropped its
 * configuration halfway through the window.
 */
#if IS_ENABLED(CONFIG_ZMK_USB)
/* Record USBD->ENABLE before the USB stack runs. A zero value confirms that the
 * loader handed over a disabled peripheral.
 */
static uint32_t usbd_preinit_enable;

static const uint32_t pwr_snap_addr[G4B_PWR_SNAP_WORDS] = {
	G4B_POWER_RESETREAS, G4B_CLOCK_HFCLKSTAT, G4B_CLOCK_LFCLKSTAT,
	G4B_CLOCK_LFCLKSRC, G4B_POWER_DCDCEN, G4B_POWER_DCDCEN0,
	G4B_USBREGSTATUS,
};

static void pwr_snapshot(uint32_t index)
{
	for (uint32_t i = 0; i < G4B_PWR_SNAP_WORDS; i++) {
		record_s3.pwr_snap[index][i] =
			*(volatile uint32_t *)pwr_snap_addr[i];
	}
}

static int g4b_usbd_preinit_probe(void)
{
	usbd_preinit_enable = *(volatile uint32_t *)G4B_USBD_ENABLE;
	pwr_snapshot(0);
	return 0;
}

/* Before anything in the USB stack, which is APPLICATION priority. */
SYS_INIT(g4b_usbd_preinit_probe, PRE_KERNEL_1, 0);

/* Snapshot SPIM2 exactly as the vendor loader left it.
 *
 * SPIM2 drives the IS31FL3743B. The loader configures it to light the recovery
 * red, then jumps to us with no reset in between, so PSEL still holds the pin
 * numbers we need and cannot get from the stock image - which contains only
 * SPIM3's nrfx pin quad.
 *
 * PRE_KERNEL_1 so nothing in Zephyr has had a chance to rebind the peripheral.
 * Reads only; no write touches SPIM2 anywhere in this payload.
 */
static int g4b_spim2_snapshot(void)
{
    record_s3.spim2_psel_sck = NRF_SPIM2->PSEL.SCK;
    record_s3.spim2_psel_mosi = NRF_SPIM2->PSEL.MOSI;
    record_s3.spim2_psel_miso = NRF_SPIM2->PSEL.MISO;
    record_s3.spim2_psel_csn = NRF_SPIM2->PSEL.CSN;
    record_s3.spim2_enable = NRF_SPIM2->ENABLE;
    record_s3.spim2_frequency = NRF_SPIM2->FREQUENCY;
    record_s3.spim2_config = NRF_SPIM2->CONFIG;

    return 0;
}

SYS_INIT(g4b_spim2_snapshot, PRE_KERNEL_1, 0);

/* Re-post the USB regulator-ready event that the vendor loader consumed.
 *
 * Zephyr's usb_dc_attach() already handles half of this hazard and says so in a
 * comment: when a USB-enabled bootloader has run, the cable-attach edge is gone
 * by the time the application starts, so it synthesises DETECTED by hand. It
 * does not do the same for READY, which rides POWER->EVENTS_USBPWRRDY and fires
 * only on the regulator output's 0->1 transition. Our vendor loader enumerates
 * over USB to receive the flash, so that edge is spent too. The driver enables
 * USBD and then waits for a READY that can never arrive, so it never reaches
 * USBD_POWERED and never calls nrf_usbd_common_start() - the call that raises
 * USBPULLUP. Measured on hardware exactly so: ENABLE=1, USBPULLUP=0, the bus
 * idle, and the last status callback SUSPEND rather than CONFIGURED.
 *
 * Re-posting the event is preferred over patching the driver or poking USBPULLUP
 * directly: nrfx has already armed the USBPWRRDY interrupt, and nRF EVENTS_
 * registers are software-writable, so this hands the driver its own missing
 * event and lets it run its own sequence - endpoints enabled, state advanced,
 * status callback raised - instead of leaving it convinced USB is still down.
 *
 * Every precondition is checked and the outcome recorded, so a launch where the
 * driver got there by itself is distinguishable from one where we intervened.
 */
#if IS_ENABLED(CONFIG_APEX_G4B_USB_KICK)
static const uint32_t usb_snap_addr[G4B_USB_SNAP_WORDS] = {
	G4B_USBD_ENABLE, G4B_USBD_USBPULLUP, G4B_USBD_INTEN, G4B_USBD_EVENTCAUSE,
	G4B_USBD_LOWPOWER, G4B_USBD_EPINEN, G4B_USBD_EPOUTEN, G4B_USBD_FRAMECNTR,
	G4B_USBD_DPDMVALUE, G4B_USBREGSTATUS, G4B_CLOCK_HFCLKSTAT,
	G4B_CLOCK_HFCLKRUN,
};

static void usb_snapshot(uint32_t index)
{
	for (uint32_t i = 0; i < G4B_USB_SNAP_WORDS; i++) {
		record_s3.usb_snap[index][i] =
			*(volatile uint32_t *)usb_snap_addr[i];
	}
}
#endif /* APEX_G4B_USB_KICK */

/* Sample USBD while its register window is still open. See evidence_g4b.h.
 *
 * Runs at APPLICATION priority 97, immediately after ZMK's usb_enable() at 96,
 * and sleeps 1 ms per iteration so the USB workqueue (cooperative, priority -1)
 * can actually make progress between samples.
 */
#if IS_ENABLED(CONFIG_APEX_G4B_USB_KICK)
static void g4b_usb_early_watch(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	record_s3.early_start_ms = k_uptime_get_32();

	pwr_snapshot(1);

	record_s3.early_pullup_first_ms = 0xFFFFFFFFu;
	record_s3.early_lowpower_first_ms = 0xFFFFFFFFu;

	/* Hold D+ up for 5 s and watch for the host.
	 *
	 * The driver suspends into LOWPOWER 3 ms after attach, which is well
	 * inside a host's ~100-200 ms connect debounce. Forcing ForceNormal every
	 * millisecond keeps the peripheral out of low power for the whole window,
	 * so the question becomes clean: given five seconds of asserted D+, does
	 * the host ever respond?
	 *
	 * USBRESET or EP0SETUP appearing means it does, and the fault is the early
	 * suspend. Neither appearing, with USBPULLUP reading 1 throughout, means
	 * D+ is not reaching the host at all and no amount of firmware will fix it.
	 */
	for (uint32_t ms = 0; ms < 5000U; ms++) {
		*(volatile uint32_t *)G4B_USBD_LOWPOWER = 0U;
		record_s3.hold_usbreset_or |= *(volatile uint32_t *)G4B_USBD_EVENTS_USBRESET;
		record_s3.hold_ep0setup_or |= *(volatile uint32_t *)G4B_USBD_EVENTS_EP0SETUP;
		record_s3.hold_usbevent_or |= *(volatile uint32_t *)G4B_USBD_EVENTS_USBEVENT;
		uint32_t pullup = *(volatile uint32_t *)G4B_USBD_USBPULLUP;
		uint32_t epin = *(volatile uint32_t *)G4B_USBD_EPINEN;
		uint32_t epout = *(volatile uint32_t *)G4B_USBD_EPOUTEN;
		uint32_t lowpower = *(volatile uint32_t *)G4B_USBD_LOWPOWER;

		record_s3.early_eventcause_or |=
			*(volatile uint32_t *)G4B_USBD_EVENTCAUSE;
		record_s3.early_samples++;

		if (pullup != 0U) {
			record_s3.early_pullup_seen = 1U;
			if (record_s3.early_pullup_first_ms == 0xFFFFFFFFu) {
				record_s3.early_pullup_first_ms = ms;
			}
		}
		if (epin > record_s3.early_epinen_max) {
			record_s3.early_epinen_max = epin;
		}
		if (epout > record_s3.early_epouten_max) {
			record_s3.early_epouten_max = epout;
		}
		if (lowpower != 0U
		    && record_s3.early_lowpower_first_ms == 0xFFFFFFFFu) {
			record_s3.early_lowpower_first_ms = ms;
		}

		k_msleep(1);
	}

	/* Retired re-attach fields remain zero for record-layout compatibility. */

	return;
}

/* A thread, not a SYS_INIT.
 *
 * SYS_INIT entries all run before z_init_static_threads(), so anything sampled
 * there is sampled before the USB workqueue exists - i.e. before the driver
 * attaches at all. The 100 ms start delay puts this after the workqueues are
 * running, so it observes the attach it is meant to measure. Priority 9 keeps
 * it below the cooperative USB workqueue at -1, so sampling cannot starve the
 * very thing being sampled.
 */
K_THREAD_DEFINE(g4b_usb_watch_thread, 1024, g4b_usb_early_watch,
		NULL, NULL, NULL, K_PRIO_PREEMPT(9), 0, 100);
#endif /* CONFIG_APEX_G4B_USB_KICK */

/* Apply the errata 187/211 trim window from nrf_usbd_common.c without its
 * runtime revision gate. Erratum 171 is nRF52840-only; do not write 0x4006EC14
 * on this nRF52833.
 */
#if IS_ENABLED(CONFIG_APEX_G4B_USB_KICK)
static void usb_errata_begin(void)
{
	unsigned int key = irq_lock();

	if (*((volatile uint32_t *)(0x4006EC00)) == 0x00000000) {
		*((volatile uint32_t *)(0x4006EC00)) = 0x00009375;
		*((volatile uint32_t *)(0x4006ED14)) = 0x00000003;
		*((volatile uint32_t *)(0x4006EC00)) = 0x00009375;
	} else {
		*((volatile uint32_t *)(0x4006ED14)) = 0x00000003;
	}

	irq_unlock(key);
}

static void usb_errata_end(void)
{
	unsigned int key = irq_lock();

	if (*((volatile uint32_t *)(0x4006EC00)) == 0x00000000) {
		*((volatile uint32_t *)(0x4006EC00)) = 0x00009375;
		*((volatile uint32_t *)(0x4006ED14)) = 0x00000000;
		*((volatile uint32_t *)(0x4006EC00)) = 0x00009375;
	} else {
		*((volatile uint32_t *)(0x4006ED14)) = 0x00000000;
	}

	irq_unlock(key);
}

static uint32_t usb_pwrrdy_kick(void)
{
	usb_snapshot(0);

	if (*(volatile uint32_t *)G4B_USBD_ENABLE == 0U) {
		return G4B_USB_KICK_USBD_OFF;
	}
	if (*(volatile uint32_t *)G4B_USBD_USBPULLUP != 0U) {
		return G4B_USB_KICK_NOT_NEEDED;
	}
	if ((*(volatile uint32_t *)G4B_USBREGSTATUS & 0x2U) == 0U) {
		return G4B_USB_KICK_NO_OUTPUTRDY;
	}
	if (nrf_power_int_enable_check(NRF_POWER,
				       NRF_POWER_INT_USBPWRRDY_MASK) == 0U) {
		return G4B_USB_KICK_INT_DISABLED;
	}

	record_s3.usb_power_inten =
		nrf_power_int_enable_check(NRF_POWER, 0xFFFFFFFFu);
	record_s3.usb_nvic_enabled = NVIC_GetEnableIRQ(POWER_CLOCK_IRQn);
	record_s3.usb_evt_before =
		*(volatile uint32_t *)((uint8_t *)NRF_POWER + NRF_POWER_EVENT_USBPWRRDY);

	*(volatile uint32_t *)((uint8_t *)NRF_POWER + NRF_POWER_EVENT_USBPWRRDY) = 1U;

	/* Long enough for the ISR and the USB workqueue (cooperative, priority -1,
	 * so it preempts this thread) to run if they are going to.
	 */
	k_msleep(20);
	record_s3.usb_evt_after =
		*(volatile uint32_t *)((uint8_t *)NRF_POWER + NRF_POWER_EVENT_USBPWRRDY);
	record_s3.usb_pullup_after = *(volatile uint32_t *)G4B_USBD_USBPULLUP;
	record_s3.usbd_inten = *(volatile uint32_t *)G4B_USBD_INTEN;

	/* Ask the stack what it thinks its own state is. With the USBPWRRDY patch
	 * in place a second attach would also synthesise READY, so if this returns
	 * 0 - meaning USB was not actually enabled - the pullup should come up
	 * right here, and the fault was never in the power events at all.
	 */
	record_s3.usb_enable_rc = usb_enable(NULL);
	k_msleep(50);
	record_s3.usb_pullup_final = *(volatile uint32_t *)G4B_USBD_USBPULLUP;

	/* Force the pullup and read it straight back. See evidence_g4b.h. */
	record_s3.usbd_lowpower = *(volatile uint32_t *)G4B_USBD_LOWPOWER;
	*(volatile uint32_t *)G4B_USBD_USBPULLUP = 1U;
	record_s3.usb_pullup_forced = *(volatile uint32_t *)G4B_USBD_USBPULLUP;
	k_msleep(1000);
	record_s3.usb_pullup_forced_1s = *(volatile uint32_t *)G4B_USBD_USBPULLUP;
	record_s3.usb_status_forced = (uint32_t)zmk_usb_get_status();

	/* Take USBD out of low power, where the register interface is inert, and
	 * read the pullup's real state. See evidence_g4b.h.
	 */
	/* Force HFXO on directly. USBD's config block needs the crystal, and BLE
	 * only holds it up around radio events, so it can be off exactly when the
	 * driver tries to raise the pullup.
	 */
	*(volatile uint32_t *)G4B_CLOCK_HFCLKSTART = 1U;
	for (uint32_t i = 0; i < 10000U; i++) {
		if (*(volatile uint32_t *)G4B_CLOCK_HFCLKSTAT & BIT(16)) {
			break;
		}
		k_busy_wait(10);
	}

	*(volatile uint32_t *)G4B_USBD_LOWPOWER = 0U;
	k_busy_wait(2000);
	usb_snapshot(1);
	record_s3.usbd_lowpower_after = *(volatile uint32_t *)G4B_USBD_LOWPOWER;
	record_s3.usb_pullup_normal = *(volatile uint32_t *)G4B_USBD_USBPULLUP;

	*(volatile uint32_t *)G4B_USBD_USBPULLUP = 1U;
	record_s3.usb_pullup_forced_normal = *(volatile uint32_t *)G4B_USBD_USBPULLUP;
	usb_snapshot(2);

	/* Cycle USBD->ENABLE and wait for the peripheral to report ready. */
	*(volatile uint32_t *)G4B_USBD_ENABLE = 0U;
	k_busy_wait(1000);
	*(volatile uint32_t *)G4B_USBD_EVENTCAUSE = 0U;
	*(volatile uint32_t *)G4B_USBD_ENABLE = 1U;
	for (uint32_t i = 0; i < 20000U; i++) {
		record_s3.usbd_reenable_spins = i;
		if (*(volatile uint32_t *)G4B_USBD_EVENTCAUSE
		    & G4B_USBD_EVENTCAUSE_READY) {
			record_s3.usbd_reenable_ready = 1U;
			break;
		}
		k_busy_wait(10);
	}
	/* Clear READY the way the driver does before using the peripheral. */
	*(volatile uint32_t *)G4B_USBD_EVENTCAUSE = G4B_USBD_EVENTCAUSE_READY;

	*(volatile uint32_t *)G4B_USBD_USBPULLUP = 1U;
	record_s3.usbd_reenable_pullup =
		*(volatile uint32_t *)G4B_USBD_USBPULLUP;
	usb_snapshot(3);

	/* Now the same cycle with erratum 171 + 187/211 applied by hand. */
	record_s3.ficr_errata_var1 = *(volatile uint32_t *)G4B_FICR_ERRATA_VAR1;
	record_s3.ficr_errata_var2 = *(volatile uint32_t *)G4B_FICR_ERRATA_VAR2;
	record_s3.errata_187_flag = nrf52_errata_187() ? 1U : 0U;
	record_s3.errata_171_flag = nrf52_errata_171() ? 1U : 0U;

	*(volatile uint32_t *)G4B_USBD_ENABLE = 0U;
	k_busy_wait(1000);
	usb_errata_begin();
	*(volatile uint32_t *)G4B_USBD_EVENTCAUSE = 0U;
	*(volatile uint32_t *)G4B_USBD_ENABLE = 1U;
	for (uint32_t i = 0; i < 20000U; i++) {
		record_s3.forced_errata_spins = i;
		if (*(volatile uint32_t *)G4B_USBD_EVENTCAUSE
		    & G4B_USBD_EVENTCAUSE_READY) {
			record_s3.forced_errata_ready = 1U;
			break;
		}
		k_busy_wait(10);
	}
	usb_errata_end();
	*(volatile uint32_t *)G4B_USBD_EVENTCAUSE = G4B_USBD_EVENTCAUSE_READY;

	*(volatile uint32_t *)G4B_USBD_USBPULLUP = 1U;
	record_s3.forced_errata_pullup =
		*(volatile uint32_t *)G4B_USBD_USBPULLUP;

	k_msleep(2000);
	record_s3.usb_status_normal = (uint32_t)zmk_usb_get_status();

	return G4B_USB_KICK_APPLIED;
}
#endif /* APEX_G4B_USB_KICK */
#endif /* CONFIG_ZMK_USB */

/* Escape-hatch detection. See evidence_g4b.h.
 *
 * Deliberately does NOT deduplicate against the previous bitmap the way
 * s3_run_poll does: a key HELD from before boot may produce one event and then
 * nothing, so dedup would hide exactly the case this exists to detect.
 */
BUILD_ASSERT(G4B_CHORD_BITMAP_BYTES == APEX_G4B_KEY_BITMAP_SIZE,
             "chord bitmap width must match the kscan bitmap");

/* Refresh time-varying watchdog and mode fields together before each stage-3
 * record. wdt_stopped reports the most recent feed decision, so it may lag the
 * adjacent mode sample by one feed period.
 *
 * Six plain reads of statics owned by two work items on the system workqueue.
 * No lock: on this single core an aligned 8/16/32-bit load cannot tear, and the
 * values are only ever compared against each other loosely.
 */
static void s3_fill_live(void)
{
    record_s3.wdt_feeds = g4b_wdt.feeds;
    record_s3.wdt_uptime_ms = k_uptime_get_32();
    record_s3.wdt_stopped = g4b_wdt.stopped_reason;

    record_s3.mode_mv = g4b_mode_mv();
    record_s3.mode_class = (uint8_t)(G4B_MODE_CLASS_PRESENT |
                                     g4b_mode_flags() |
                                     ((uint8_t)g4b_mode_get() &
                                      G4B_MODE_CLASS_MASK));
    record_s3.mode_sample_ms = g4b_mode_last_sample_ms();
}

#if G4B_KEYBOARD_BUILD && IS_ENABLED(CONFIG_APEX_G4B_BOOT_DIAG)
static void s3_chord_window(void)
{
	uint32_t start = g4b_cyccnt();
	uint32_t last_a1 = start;

	record_s3.chord_first_ms = 0xFFFFFFFFu;
	record_s3.a0_bit7_first_ms = 0xFFFFFFFFu;

	while (elapsed_us_since(start) < (G4B_CHORD_WINDOW_MS * 1000u)) {
		struct g4b_exchange_stats stats = {0};

		if (!g4b_pin_read(G4B_PORT0, G4B_P0_ATTN)) {
			continue;
		}
		if (elapsed_us_since(last_a1) < G4B_S3_A1_MIN_GAP_US) {
			continue;
		}
		last_a1 = g4b_cyccnt();

		memset(s2_tx, 0, sizeof(s2_tx));
		s2_tx[0] = 0xA1u;
		memset(s2_rx, 0, sizeof(s2_rx));

		g4b_spim_arm_ready();
		(void)g4b_spim_exchange(s2_tx, s2_rx, &stats);
		if (stats.result != G4B_SPIM_OK) {
			continue;
		}
		record_s3.chord_a1_frames++;

		for (uint32_t i = 0; i < APEX_G4B_KEY_BITMAP_SIZE; i++) {
			record_s3.chord_bitmap_or[i] |= s2_rx[i];
		}

		if (s2_rx[G4B_CHORD_BYTE] & G4B_CHORD_MASK) {
			record_s3.chord_seen = 1u;
			if (record_s3.chord_first_ms == 0xFFFFFFFFu) {
				record_s3.chord_first_ms =
					elapsed_us_since(start) / 1000u;
			}
		}
	}

	record_s3.chord_window_ms = elapsed_us_since(start) / 1000u;
}

static void s3_run_poll(uint32_t start, uint32_t deadline_cycles)
{
    /* Use uptime milliseconds because the 64 MHz DWT counter wraps every
     * 67.1 seconds. */
    uint32_t poll_start_ms = k_uptime_get_32();
    uint32_t deadline_ms = (uint32_t)CONFIG_APEX_G4B_DEADLINE_MS;
    uint32_t poll_start = g4b_cyccnt();
    uint32_t last_a0 = poll_start;
    uint32_t last_a1 = poll_start;
    bool first_a0 = true;
    uint32_t last_ain_ms = 0U;

    for (uint32_t ch = 0U; ch < G4B_SAADC_CHANNELS; ch++) {
        record_s3.ain_min[ch] = 0xFFFFu;
        record_s3.ain_max[ch] = 0U;
    }

    while ((k_uptime_get_32() - poll_start_ms) < deadline_ms) {
        struct g4b_exchange_stats stats = {0};
        uint32_t now_ms = k_uptime_get_32();

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
        /* Pump staged RGB frames during the diagnostic window. */
        g4b_rgb_flush();
#endif

        /* Sweep the candidate analog pins twice a second.
         *
         * Deliberately NOT every iteration: the A0 poll runs at ~20 Hz and a
         * conversion plus its bounded waits would sit inside the SPIM frame
         * gap. Twice a second is far denser than a human can move a slide
         * switch, and leaves the link timing alone.
         */
        if (last_ain_ms == 0U || (now_ms - last_ain_ms) >= 500U) {
            last_ain_ms = now_ms;

            for (uint32_t ch = 0U; ch < G4B_SAADC_CHANNELS; ch++) {
                int16_t raw = g4b_saadc_read(g4b_saadc_pselp[ch]);
                uint16_t v = (raw < 0) ? 0u : (uint16_t)raw;

                record_s3.ain_last[ch] = v;
                if (v < record_s3.ain_min[ch]) {
                    record_s3.ain_min[ch] = v;
                }
                if (v > record_s3.ain_max[ch]) {
                    record_s3.ain_max[ch] = v;
                }
            }

            if (record_s3.ain_samples < 0xFFFFu) {
                record_s3.ain_samples++;
            }
        }

        if (g4b_pin_read(G4B_PORT0, G4B_P0_ATTN)) {
            record_s3.attn_high_seen++;

            if (record_s3.polls_a1 >= G4B_S3_A1_MAX ||
                elapsed_us_since(last_a1) < G4B_S3_A1_MIN_GAP_US) {
                continue;
            }
            last_a1 = g4b_cyccnt();

            memset(s2_tx, 0, sizeof(s2_tx));
            s2_tx[0] = 0xA1u;
            memset(s2_rx, 0, sizeof(s2_rx));

            g4b_spim_arm_ready();
            (void)g4b_spim_exchange(s2_tx, s2_rx, &stats);
            record_s3.polls_a1++;

            if (stats.result != G4B_SPIM_OK) {
                continue;
            }

            /* Deduplicate against the previous bitmap so holding a key down
             * does not fill the buffer with copies of the same state.
             */
            if (s3_have_prev &&
                memcmp(s3_prev_bitmap, s2_rx, APEX_G4B_KEY_BITMAP_SIZE) == 0) {
                continue;
            }
            /* Recorded unconditionally below, but the dedup shadow only
             * advances once ZMK has accepted the frame - see the same
             * reasoning in s3_run_keyboard. s3_prev_bitmap is shared between
             * the two loops, so a divergence opened here would be inherited by
             * the permanent loop at the handover.
             */
            if (record_s3.events_total < G4B_S3_EVENTS) {
                struct g4b_key_event *ev =
                    &record_s3.event[record_s3.events_total];

                ev->t_us = elapsed_us_since(poll_start);
                ev->result = stats.result;
                memcpy(ev->rx, s2_rx, G4B_S3_EVENT_RX);
            }
            record_s3.events_total++;

#if IS_ENABLED(CONFIG_APEX_G4B_KSCAN_INGEST)
            /* The return code is recorded, not discarded. -EACCES here means
             * ZMK never configured or enabled the kscan device, which looks
             * identical from outside to "the keys never arrived" - and the two
             * need entirely different fixes.
             */
            record_s3.ingest_last_rc = apex_g4b_kscan_ingest_bitmap(
                s3_kscan, s2_rx, APEX_G4B_KEY_BITMAP_SIZE);
            record_s3.ingest_calls++;
            if (record_s3.ingest_last_rc == 0) {
                record_s3.ingest_ok++;
                memcpy(s3_prev_bitmap, s2_rx, APEX_G4B_KEY_BITMAP_SIZE);
                s3_have_prev = true;
            }
#else
            memcpy(s3_prev_bitmap, s2_rx, APEX_G4B_KEY_BITMAP_SIZE);
            s3_have_prev = true;
#endif
            continue;
        }

        /* Deliberately does NOT adopt the keyboard loop's A1->A0 stamp. This
         * block is the travel-scalar instrument (a0_bit7_first_ms, a0_low6_max,
         * a0_plateau40, a0_lost_config) and those fields only have anything to
         * report WHILE a key is down. Suppressing A0 for 50 ms after every A1
         * read would blind them exactly then, and would break comparability
         * with every stored a0= baseline.
         */
        if (!first_a0 && elapsed_us_since(last_a0) < G4B_S3_A0_GAP_US) {
            continue;
        }
        first_a0 = false;
        last_a0 = g4b_cyccnt();

        memset(s2_tx, 0, sizeof(s2_tx));
        s2_tx[0] = 0xA0u;
        memset(s2_rx, 0, sizeof(s2_rx));

        g4b_spim_arm_ready();
        (void)g4b_spim_exchange(s2_tx, s2_rx, &stats);
        record_s3.polls_a0++;

        if (stats.result == G4B_SPIM_OK && s2_rx[0] == 0xA0u && s2_rx[1] == 1u) {
            /* byte[2] is the matrix-wide travel scalar: bit 7 = a key is down,
             * bits 0..5 = 0..63 magnitude. See evidence_g4b.h.
             */
            uint32_t b2 = s2_rx[2];

            record_s3.a0_polls++;
            record_s3.a0_b2_or |= b2;
            record_s3.a0_b2_last = b2;
            if ((b2 & 0x3Fu) > record_s3.a0_low6_max) {
                record_s3.a0_low6_max = b2 & 0x3Fu;
            }
            if (b2 == 0x40u) {
                record_s3.a0_plateau40++;
            }
            if ((b2 & 0x80u) &&
                record_s3.a0_bit7_first_ms == 0xFFFFFFFFu) {
                record_s3.a0_bit7_first_ms =
                    k_uptime_get_32() - poll_start_ms;
            }
        }

        if (stats.result != G4B_SPIM_OK || s2_rx[0] != 0xA0u || s2_rx[1] != 1u) {
            record_s3.a0_lost_config++;
        }
    }

    /* Report from the same uptime clock used by the polling deadline. */
    record_s3.poll_window_us =
        (k_uptime_get_32() - poll_start_ms) * 1000u;
}
#endif /* G4B_KEYBOARD_BUILD && APEX_G4B_BOOT_DIAG */

#if IS_ENABLED(CONFIG_APEX_G4B_KSCAN_INGEST)
/* Permanent keyboard runtime. It waits for ATTN, reads an absolute 0xA1 bitmap,
 * deduplicates it, and passes changes to ZMK. Each iteration publishes a scanner
 * heartbeat; wdt_g4b.c owns watchdog feeding. The 1 ms sleep yields to BLE, USB,
 * underglow, and watchdog work.
 */
/* A key report with more bits set than any hand can hold is a corrupt or
 * racing read, not real input. Reject it rather than let it become a storm of
 * spurious press/release events. Generous enough for full n-key rollover.
 */
#define G4B_KEYBOARD_MAX_KEYS 10u

static uint32_t s3_ingest_events;
static uint32_t s3_a0_polls;
static uint32_t s3_last_activity_ms;
#if IS_ENABLED(CONFIG_APEX_G4B_BAG_GUARD)
/* Bag/pocket guard state. s3_bag_active is true while the loop is held in its
 * slow-poll state; s3_bag_timing/s3_bag_since time how long the key COUNT has
 * stayed high. A count this obviously large is never a real chord, so it
 * engages after only a short settle instead of the full idle window.
 */
static bool s3_bag_active;
static bool s3_bag_timing;
static uint32_t s3_bag_since;
#define G4B_BAG_OBVIOUS_KEYS   8u
#define G4B_BAG_OBVIOUS_IDLE_MS 1000u
#endif

#if IS_ENABLED(CONFIG_APEX_G4B_UART_EVIDENCE)
/* Tiny decimal/string formatters. Defined here rather than next to the boot
 * timing they were written for, because the sleep diagnostics below use them
 * too and sit earlier in the file.
 */
static uint32_t txt_u32(uint8_t *dst, uint32_t v)
{
    uint8_t tmp[10];
    uint32_t n = 0u;
    uint32_t i = 0u;

    do {
        tmp[n++] = (uint8_t)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u);

    while (n-- != 0u) {
        dst[i++] = tmp[n];
    }
    return i;
}

static uint32_t txt_str(uint8_t *dst, const char *src)
{
    uint32_t i = 0u;

    while (src[i] != 0) {
        dst[i] = (uint8_t)src[i];
        i++;
    }
    return i;
}

#endif


#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW) && CONFIG_APEX_G4B_RGB_IDLE_MS > 0
/* Blank the LEDs after a spell of no key activity while Bluetooth is the
 * physical mode and USB is not the selected report endpoint.
 *
 * Called from the keyboard loop immediately before g4b_rgb_flush(), so the
 * decision and the transmit are on the same thread and the SPIM2 single-writer
 * invariant holds.
 *
 * Activity is our own ingest, not ZMK's activity state: this needs its own
 * timeout (30 seconds) without disturbing ZMK_IDLE_TIMEOUT, which is 30 s and
 * shared with other subsystems. A key held down without moving does not
 * re-ingest and so reads as idle - correct for a keyboard nobody is at, and
 * the frame comes straight back on the release.
 */
static void s3_rgb_idle_update(void)
{
    bool usb_selected = false;

#if IS_ENABLED(CONFIG_ZMK_USB)
    /* A configured USB host keeps the lighting awake. A charge-only battery
     * bank does not select USB, so Bluetooth's 30-second timeout still applies.
     */
    usb_selected = s3_usb_is_powered() &&
                   zmk_endpoint_get_selected().transport == ZMK_TRANSPORT_USB;
#endif

    uint32_t now = k_uptime_get_32();
    /* Only Bluetooth mode owns this timeout. A configured USB endpoint and the
     * other physical switch modes keep their prior always-on behavior. In BT,
     * the lights ease out once no key has moved for the idle window and ease
     * back in on activity. The fade and rail cycling live in rgb_idle_tick.
     */
    bool want_on = g4b_mode_get() != G4B_MODE_BT || usb_selected ||
                   (now - s3_last_activity_ms) < (uint32_t)CONFIG_APEX_G4B_RGB_IDLE_MS;

#if IS_ENABLED(CONFIG_APEX_G4B_BAG_GUARD)
    /* A bag squeeze keeps the activity clock fresh through key chatter, so the
     * normal RGB idle would never blank. Force the LEDs off while bagged - they
     * are the largest single draw on the board, so leaving them lit would
     * undo most of the guard's saving. */
    if (s3_bag_active) {
        want_on = false;
    }
#endif

    g4b_rgb_idle_tick(want_on, now);
}
#else
static inline void s3_rgb_idle_update(void) {}
#endif

/* --- Persistence ----------------------------------------------------------
 *
 * Switch tuning, BLE bonds, and RGB state use the ZMK NVS partition in external
 * NOR at 0x60000..0x68000. Application and UF2 updates do not overwrite it.
 *
 * One record, not four keys. These are read together at boot and written
 * together. A torn or version-mismatched record is rejected as a unit.
 */
#if IS_ENABLED(CONFIG_SETTINGS)

struct g4b_switch_settings {
    uint8_t act_step;
    uint8_t reset_gap;
    uint8_t rt_tenths;
    uint8_t rt_last_on;
    uint8_t gamepad_on;
    uint8_t fx;
};

static void g4b_settings_save_work(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g4b_settings_dwork, g4b_settings_save_work);

static void g4b_settings_save_work(struct k_work *work)
{
    struct g4b_switch_settings st = {
        .act_step = s3_act_step,
        .reset_gap = s3_reset_gap,
        .rt_tenths = s3_rt_tenths,
        .rt_last_on = s3_rt_last_on,
#if IS_ENABLED(CONFIG_APEX_G4B_GAMEPAD)
        .gamepad_on = g4b_gamepad_is_enabled() ? 1u : 0u,
#else
        .gamepad_on = 0u,
#endif
        .fx = (uint8_t)g4b_fx_current(),
    };

    ARG_UNUSED(work);
    (void)settings_save_one("apex/switch", &st, sizeof(st));
}

/* Debounced, deliberately. Every one of these controls is a key someone will
 * press repeatedly while dialling a feel in, and a flash write per press is
 * both NVS wear and a radio-synced erase that blocks its caller. Thirty seconds
 * after the last change, one write.
 */
void g4b_settings_mark_dirty(void)
{
    (void)k_work_reschedule(&g4b_settings_dwork, K_SECONDS(30));
}

static int g4b_settings_set(const char *name, size_t len,
                            settings_read_cb read_cb, void *cb_arg)
{
    struct g4b_switch_settings st;
    const char *next;

    if (!settings_name_steq(name, "switch", &next) || next != NULL) {
        return -ENOENT;
    }
    if (len != sizeof(st)) {
        return -EINVAL;
    }
    if (read_cb(cb_arg, &st, sizeof(st)) < 0) {
        return -EIO;
    }

    /* EVERY field is range-checked before it is used, and silently dropped if
     * it is wrong rather than clamped. Two of these are array indices: a
     * corrupt record reaching g4b_act_steps[] unchecked is an out-of-bounds
     * read on a lookup that decides how hard the keyboard presses its own keys.
     * A rejected field simply leaves the compiled-in default in place.
     */
    if (st.act_step < ARRAY_SIZE(g4b_act_steps)) {
        s3_act_step = st.act_step;
    }
    if (st.reset_gap < ARRAY_SIZE(g4b_reset_gaps)) {
        s3_reset_gap = st.reset_gap;
    }
    if (st.rt_tenths <= 40u) {
        s3_rt_tenths = st.rt_tenths;
    }
    if (st.rt_last_on >= 1u && st.rt_last_on <= 40u) {
        s3_rt_last_on = st.rt_last_on;
    }
#if IS_ENABLED(CONFIG_APEX_G4B_GAMEPAD)
    g4b_gamepad_set_enabled(st.gamepad_on != 0u);
#endif
    /* g4b_fx_set() range-checks for itself, so a corrupt byte simply leaves
     * the effect at passthrough rather than indexing off the end of a switch.
     */
    g4b_fx_set((enum g4b_fx)st.fx);

    /* Ask for a resend even though the boot replay has not run yet.
     *
     * Settings load during application init and the g4b thread does not start
     * for another 250 ms, so the replay should already pick these up. "Should"
     * is doing work in that sentence - it depends on init ordering we do not
     * control - and one extra config resend costs a few frames, where getting
     * it wrong means the board silently runs at defaults while the saved values
     * sit in flash.
     */
    s3_cfg_dirty = 1u;
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(apex_switch, "apex", NULL, g4b_settings_set,
                               NULL, NULL);
#else
void g4b_settings_mark_dirty(void) {}
#endif /* CONFIG_SETTINGS */

/* --- Runtime actuation and rapid-trigger controls -------------------------
 *
 * Called from ZMK's thread when a key is pressed. They only move a byte and
 * raise a flag; the g4b thread does the talking, because SPIM3 has exactly one
 * writer and that is not negotiable - the whole payload runs without a bus lock
 * on the strength of it.
 */
void g4b_actuation_step(int delta)
{
    int step = (int)s3_act_step + delta;

    if (step < 0) {
        step = 0;
    } else if (step >= (int)ARRAY_SIZE(g4b_act_steps)) {
        step = (int)ARRAY_SIZE(g4b_act_steps) - 1;
    }
    if ((uint8_t)step == s3_act_step) {
        return; /* already at the end of the ladder - do not resend for nothing */
    }

    s3_act_step = (uint8_t)step;
    s3_cfg_dirty = 1u;
    g4b_settings_mark_dirty();
}

void g4b_rapid_trigger_step(int delta)
{
    int rt = (int)s3_rt_tenths + delta;

    /* 1..40 tenths of a millimetre is the scanner's range; 0 is off, and is
     * reached through the toggle rather than by stepping down into it, so that
     * holding the decrement key cannot silently disable the feature.
     */
    if (rt < 1) {
        rt = 1;
    } else if (rt > 40) {
        rt = 40;
    }
    if ((uint8_t)rt == s3_rt_tenths) {
        return;
    }

    s3_rt_tenths = (uint8_t)rt;
    s3_rt_last_on = s3_rt_tenths;
    s3_cfg_dirty = 1u;
    g4b_settings_mark_dirty();
}

void g4b_rapid_trigger_toggle(void)
{
    s3_rt_tenths = (s3_rt_tenths != 0u) ? 0u : s3_rt_last_on;
    s3_cfg_dirty = 1u;
    g4b_settings_mark_dirty();
}

void g4b_reset_point_cycle(void)
{
    /* One key, cycling, rather than two. The gap has four sensible values and
     * no free labelled keycap to spend on a second binding, and wrapping round
     * is easy to explore by feel - which is how anyone actually tunes this.
     */
    s3_reset_gap = (uint8_t)((s3_reset_gap + 1u) % ARRAY_SIZE(g4b_reset_gaps));
    s3_cfg_dirty = 1u;
    g4b_settings_mark_dirty();
}

uint8_t g4b_reset_gap_counts(void)
{
    return g4b_reset_gaps[s3_reset_gap];
}

uint8_t g4b_actuation_tenths(void)
{
    /* Depth of the current step, for anything that wants to show it. Same six
     * measured points as the ladder: 1.0, 1.5, 2.0, 2.1, 2.5, 3.0 mm.
     */
    static const uint8_t tenths[] = { 10u, 15u, 20u, 21u, 25u, 30u };

    BUILD_ASSERT(ARRAY_SIZE(tenths) == ARRAY_SIZE(g4b_act_steps),
                 "the depth table and the index ladder must stay in step");
    return tenths[s3_act_step];
}

uint8_t g4b_rapid_trigger_tenths(void)
{
    return s3_rt_tenths;
}

/* Re-send only the frames that carry these settings.
 *
 * NOT the whole 59-frame boot prefix. That would take 472 ms with the scanner
 * ignoring keys throughout, and it would re-assert every other setting as a
 * side effect of changing one. Only 0x30, 0x33 and 0x35 carry thresholds, so
 * only those go back out - a handful of frames, paced exactly as at boot.
 *
 * Runs on the g4b thread, from the ATTN-low branch, so the scanner has nothing
 * queued and no key report can be racing this.
 */
static void s3_resend_config(void)
{
    for (uint32_t i = 0u; i < G4B_S2_PREFIX_FRAMES; i++) {
        struct g4b_exchange_stats stats = {0};
        uint32_t frame_start;
        uint8_t op = apex_boot_prefix[i].tx[0];

        if (op != 0x30u && op != 0x33u && op != 0x35u) {
            continue;
        }

        frame_start = g4b_cyccnt();
        memcpy(s2_tx, apex_boot_prefix[i].tx, G4B_SPIM_FRAME);
        s2_apply_actuation(s2_tx);
        s2_apply_rapid_trigger(s2_tx);
        memset(s2_rx, 0, sizeof(s2_rx));

        g4b_spim_arm_ready();
        (void)g4b_spim_exchange(s2_tx, s2_rx, &stats);

        /* Same pacing as the boot replay. The scanner was characterised at this
         * rate and there is no reason to discover how it behaves at another.
         */
        pace_frame(frame_start);
    }
}

/* STM32 scanner-link health, exposed to the apex shell (`apex link`). */
void g4b_link_stats_get(struct g4b_link_stats *out)
{
    if (out == NULL) {
        return;
    }
    out->frames_run = record_s3.frames_run;
    out->frames_matched = record_s3.frames_matched;
    out->ingest_calls = record_s3.ingest_calls;
    out->ingest_ok = record_s3.ingest_ok;
    /* Live counters from the permanent keyboard loop - these move as keys are
     * pressed, unlike the boot snapshots above (which the loop assigns once at
     * handover and never touches again). */
    out->live_key_events = s3_ingest_events;
    out->live_keepalives = s3_a0_polls;
}

#if IS_ENABLED(CONFIG_APEX_G4B_RGB)
/* Per-key depth for the analog lighting effect.
 *
 * Only runs when that effect is selected - g4b_fx_wants_analog() gates it, so
 * an ordinary build spends zero bus time here. Sweeps ONE 32-key window per
 * call, cycling through the 70 keys over three calls, which matches the cadence
 * and the safety of the existing gamepad sampler: it issues 0xA2 only from the
 * ATTN-low branch, where the scanner has nothing queued, because 0xA2 can drop
 * the attention line and a swallowed ATTN is a lost keypress.
 *
 * Depth is 0xA2's raw ADC, valid roughly 337..3959, larger meaning deeper.
 * Normalised to 0..255 per key against that span - not a physical millimetre,
 * which the travel table could give, but proportional to travel, which is all
 * a brightness needs.
 */
/* Shared temporal filter for the raw 0xA2 depth, one slot per key position.
 *
 * The scanner hands back a fresh raw ADC count every read, and both consumers -
 * the RGB analog effects and the gamepad axis - used to map that single sample
 * straight to output. So every count of ADC/finger tremor reached the axis and
 * the lighting on every sample, which is the jitter: worst on a slow press,
 * where the finger dwells mid-travel and small movements wiggle the reading.
 *
 * A one-pole EMA with alpha = 1/4 (time constant ~4 samples) cuts that RMS
 * noise roughly 2.6x while lagging a deliberate press by only ~4 samples, which
 * is imperceptible. Filtering HERE, keyed by absolute scan slot, means the axis
 * and the RGB read one coherent smoothed signal and cannot disagree. The
 * digital key path (0xA1) is untouched, so nothing about actuation changes.
 *
 * Raw is always in [337,3959] when this is called, so 0 is a safe "unseen"
 * sentinel that seeds the filter on the first sample instead of ramping from 0.
 */
static uint16_t g4b_depth_ema[G4B_SCAN_SLOTS];

static uint16_t g4b_depth_ingest(uint8_t slot, uint16_t raw)
{
    uint16_t f;

    if (slot >= G4B_SCAN_SLOTS) {
        return raw;
    }
    f = g4b_depth_ema[slot];
    if (f == 0u) {
        f = raw;
    } else {
        f = (uint16_t)((int32_t)f + ((int32_t)raw - (int32_t)f) / 4);
    }
    g4b_depth_ema[slot] = f;
    return f;
}

/* Set by the apex shell (`apex depth`) to force one/again 0xA2 sampling even
 * when no analog RGB effect is active, so per-key travel can be read on demand. */
static volatile bool g4b_depth_force_flag;

void g4b_depth_force(bool on)
{
    g4b_depth_force_flag = on;
}

/* Copy the filtered per-key depth (raw ADC ~337 released .. ~3959 pressed;
 * 0 = not yet sampled). Returns the true slot count regardless of @max. */
size_t g4b_depth_read(uint16_t *out, size_t max)
{
    size_t n = (max < (size_t)G4B_SCAN_SLOTS) ? max : (size_t)G4B_SCAN_SLOTS;
    for (size_t i = 0; i < n; i++) {
        out[i] = g4b_depth_ema[i];
    }
    return (size_t)G4B_SCAN_SLOTS;
}

static void s3_analog_light(void)
{
    static uint8_t win;
    struct g4b_exchange_stats stats = {0};
    uint8_t start = (uint8_t)(win * 32u);
    uint8_t count = (start + 32u <= G4B_SCAN_SLOTS)
                        ? 32u
                        : (uint8_t)(G4B_SCAN_SLOTS - start);

    if (!g4b_fx_wants_analog() && !g4b_depth_force_flag) {
        return;
    }

    memset(s2_tx, 0, sizeof(s2_tx));
    s2_tx[0] = 0xA2u;
    s2_tx[1] = count;
    s2_tx[2] = start;
    memset(s2_rx, 0, sizeof(s2_rx));

    g4b_spim_arm_ready();
    if (g4b_spim_exchange(s2_tx, s2_rx, &stats) == G4B_SPIM_OK &&
        stats.result == G4B_SPIM_OK) {
        for (uint8_t k = 0u; k < count; k++) {
            uint32_t o = (uint32_t)k * 2u;
            uint16_t v = (uint16_t)(s2_rx[o] | ((uint16_t)s2_rx[o + 1u] << 8));

            if (v < 337u || v > 3959u) {
                continue; /* scanner skipped this key; not a reading */
            }
            v = g4b_depth_ingest((uint8_t)(start + k), v); /* shared smoothing */
            g4b_fx_depth((uint8_t)(start + k),
                         (uint8_t)(((uint32_t)(v - 337u) * 255u) / (3959u - 337u)));
        }
    }

    win = (uint8_t)((win + 1u) % 3u);
}
#endif

#if CONFIG_APEX_G4B_SLEEP_MS > 0
/* Enter System OFF after the keyboard has remained idle. Call this only from
 * the ATTN-low scan path. DETECT is a level OR over the wake pins, so entering
 * System OFF while ATTN is high would wake immediately and cause a reset loop.
 * sleep_g4b.h documents the remaining preconditions and wake self-test.
 */
static bool s3_sleep_failed;

static void s3_sleep_maybe(void)
{
    bool powered = false;

    if (s3_sleep_failed) {
        return;
    }

#if IS_ENABLED(CONFIG_ZMK_USB)
    /* On USB there is nothing to save and the host expects a live device. */
    powered = s3_usb_is_powered();
#endif
    if (powered) {
        return;
    }

    /* Bluetooth position only - the USB position parks P0.03 at mid-rail,
     * where SENSE is not reliably either state.
     */
    if (g4b_mode_get() != G4B_MODE_BT) {
        return;
    }

    /* No keypress has been seen setting the ATTN latch since boot, so the wake
     * path is unproven on this hardware and sleeping could be a one-way trip.
     */
    if (!g4b_sleep_wake_proven()) {
        return;
    }

    if ((k_uptime_get_32() - s3_last_activity_ms) <
        (uint32_t)CONFIG_APEX_G4B_SLEEP_MS) {
        return;
    }

#if IS_ENABLED(CONFIG_APEX_G4B_UART_EVIDENCE)
    {
        /* Capture state immediately before System OFF. latch and attn identify
         * an already-active DETECT source; a post-write record distinguishes a
         * failed System OFF entry from a reset before the write.
         */
        static uint8_t line[160];
        uint32_t n = 0u;

        n += txt_str(&line[n], "APXS pre latch=");
        n += txt_u32(&line[n], g4b_sleep_latch_raw());
        n += txt_str(&line[n], " attn=");
        n += txt_u32(&line[n], g4b_pin_read(G4B_PORT0, G4B_P0_ATTN) ? 1u : 0u);
        n += txt_str(&line[n], " mv=");
        n += txt_u32(&line[n], g4b_mode_mv());
        n += txt_str(&line[n], " up=");
        n += txt_u32(&line[n], k_uptime_get_32());
        line[n++] = (uint8_t)13;
        line[n++] = (uint8_t)10;
        g4b_evidence_emit_text(line, n);
    }
#endif

#if CONFIG_APEX_G4B_SLEEP_SCAN_PERIOD > 0
    /* Throttle the scanner before we stop the CPU.
     *
     * In System OFF the nRF draws ~2 uA, but the STM32 keeps scanning at full
     * cadence (~3-5 mA) so it can raise ATTN - our only wake source - on a key.
     * The scanner therefore dominates the sleep budget. SET_MODE mode 0x03
     * selects the alternate (slower) scan interval: it still detects keys and
     * still raises ATTN, just up to <period> later, and that latency only ever
     * hits the single key that wakes the board (already a reset + reconnect).
     *
     * No restore is issued here on purpose: waking from System OFF is a full
     * reset, and CONFIG_APEX_G4B_STM32_RESET_MS (enforced non-zero by the
     * Kconfig dependency) power-cycles the scanner at the next boot back to full
     * cadence. This is the LAST scanner exchange before the CPU stops - ATTN is
     * known low here (see the s3_sleep_maybe() contract), so it cannot race a
     * key report. If the SYSTEMOFF write below is ignored (s3_sleep_failed), the
     * scanner is left at the slow cadence until the next reboot: it still works,
     * just with higher latency, and that path is already abnormal. */
    {
        struct g4b_exchange_stats st = {0};

        memset(s2_tx, 0, sizeof(s2_tx));
        s2_tx[0] = 0x20u; /* SET_MODE */
        s2_tx[1] = 0x03u; /* alternate (slower) scan cadence, still wakes on key */
        s2_tx[2] = (uint8_t)CONFIG_APEX_G4B_SLEEP_SCAN_PERIOD;
        memset(s2_rx, 0, sizeof(s2_rx));
        g4b_spim_arm_ready();
        (void)g4b_spim_exchange(s2_tx, s2_rx, &st);
    }
#endif

    g4b_sleep_enter();

    /* Reached only if the SYSTEMOFF write did nothing. Say so, once, and never
     * ask again - the keyboard carries on scanning instead of retrying every
     * pass or parking somewhere it cannot be recovered from.
     */
    s3_sleep_failed = true;
#if IS_ENABLED(CONFIG_APEX_G4B_UART_EVIDENCE)
    {
        static uint8_t line[64];
        uint32_t n = 0u;

        n += txt_str(&line[n], "APXS post-systemoff-ignored up=");
        n += txt_u32(&line[n], k_uptime_get_32());
        line[n++] = (uint8_t)13;
        line[n++] = (uint8_t)10;
        g4b_evidence_emit_text(line, n);
    }
#endif
}
#else
static inline void s3_sleep_maybe(void) {}
#endif

#if (CONFIG_APEX_G4B_STM32_IDLE_SCAN_PERIOD_MS > 1) || \
    (CONFIG_APEX_G4B_STM32_STOP1_IDLE_MS > 0)
/* Last-resort, in-place link recovery shared by the two opt-in scanner
 * canaries. This is the same safe sequence used at boot: hold both scanner
 * enables low long enough to drain its rail, wait for READY, and replay all 59
 * stock configuration frames. DFU and the Nordic remain alive even if this
 * also fails. */
static bool s3_recover_scanner(bool defer_if_attn, bool *deferred)
{
    uint32_t start;
    bool ready;
    bool replay_ok = false;

    if (deferred != NULL) {
        *deferred = false;
    }
    g4b_extbus_replay_begin();
    g4b_extbus_lock();

    /* An external-NVS operation may have held the mutex after the caller's
     * ATTN-low check. For mode-3 recovery, never power-cycle across a key that
     * arrived while we slept acquiring that lock: release everything and let
     * A1 run first. The known-failing STOP1 harness requests its historical
     * unconditional recovery behavior instead. */
    if (defer_if_attn && g4b_pin_read(G4B_PORT0, G4B_P0_ATTN)) {
        g4b_extbus_unlock();
        g4b_extbus_replay_end();
        if (deferred != NULL) {
            *deferred = true;
        }
        return false;
    }

    g4b_spim_disable();
    bringup_pins();
    k_msleep(CONFIG_APEX_G4B_STM32_RESET_MS);
    enable_stm32();

    start = g4b_cyccnt();
    while (!g4b_pin_read(G4B_PORT0, G4B_P0_READY) &&
           (g4b_cyccnt() - start) < G4B_READY_POLL_LIMIT) {
        /* bounded boot-equivalent READY wait */
    }
    ready = g4b_pin_read(G4B_PORT0, G4B_P0_READY);
    g4b_gpiote_ready_clear();
    /* Restore the normal link pins even when READY missed this bounded window.
     * A scanner that finishes booting late must not be stranded behind a
     * disabled peripheral. */
    g4b_spim_enable();
    g4b_extbus_unlock();

    if (ready) {
        memset(&record_s2r, 0, sizeof(record_s2r));
        s2_run_replay(g4b_cyccnt(), G4B_S2_REPLAY_BUDGET_CYCLES);
        s2_post_replay_probe();
        replay_ok = record_s2r.frames_run == G4B_S2_PREFIX_FRAMES &&
                    record_s2r.frames_matched == G4B_S2_PREFIX_FRAMES &&
                    record_s2r.post_replay_a0.result == G4B_SPIM_OK &&
                    record_s2r.post_replay_a0.rx[1] == 1u;
    }

    g4b_extbus_replay_end();
    if (replay_ok) {
        s3_have_prev = false;
    }
    return replay_ok;
}
#endif

#if CONFIG_APEX_G4B_STM32_IDLE_SCAN_PERIOD_MS > 1
/* Bluetooth-only, reversible scan-cadence control.
 *
 * Handler RE matters here: 20 01 01 is a boot-time RUN/setup frame, not an
 * in-place restore. While scanner state 1 is active, that command is rejected
 * as 20 02 02. A detected matrix change calls the mode-1 callback, which clears
 * the alternate selector and immediately returns scanning to the primary
 * interval. Once ATTN is low again we also normalize the alternate interval to
 * 1 with a mode-3 write, so either selector is full speed:
 *
 *     idle: 20 03 CONFIG_APEX_G4B_STM32_IDLE_SCAN_PERIOD_MS
 *     long: 20 03 CONFIG_APEX_G4B_STM32_LONG_IDLE_SCAN_PERIOD_MS
 *     fast: 20 03 01
 *
 * Both mode-3 paths return 20 00 00 in ordinary RUN state. The fast write is a
 * defensive cadence override, not a semantic exit from mode 3. Its selected
 * alternate interval of 1 is cadence-equivalent to the primary interval of 1;
 * the next idle spell can rewrite it slow again without a state change.
 */
volatile uint32_t g4b_mode3_entry_attempts;
volatile uint32_t g4b_mode3_entries;
volatile uint32_t g4b_mode3_attn_resumes;
volatile uint32_t g4b_mode3_cancellations;
volatile uint32_t g4b_mode3_fast_overrides;
volatile uint32_t g4b_mode3_failures;
volatile uint32_t g4b_mode3_recoveries;
volatile uint32_t g4b_mode3_expected_period = 1u;
volatile uint32_t g4b_mode3_commands;
volatile uint32_t g4b_mode3_last_result;
volatile uint32_t g4b_mode3_last_reply;
volatile uint32_t g4b_mode3_last_tx_amount;
volatile uint32_t g4b_mode3_last_rx_amount;
volatile uint32_t g4b_mode3_last_events_end;
volatile uint32_t g4b_mode3_last_wait_tx_us;
volatile uint32_t g4b_mode3_last_wait_rx_us;
volatile uint32_t g4b_mode3_last_fault;
volatile uint32_t g4b_mode3_failure_result;
volatile uint32_t g4b_mode3_failure_reply;
volatile uint32_t g4b_mode3_failure_fault;
volatile uint32_t g4b_mode3_last_slow_residency_ms;
volatile uint32_t g4b_mode3_max_slow_residency_ms;
volatile uint32_t g4b_mode3_long_entry_attempts;
volatile uint32_t g4b_mode3_long_entries;

/* Kept true after the scanner's automatic key-change resume until the Nordic
 * has sent its explicit period-1 normalization. */
static bool s3_mode3_slow_requested;
static bool s3_mode3_resume_seen;
static bool s3_mode3_disabled;
static bool s3_mode3_repair_pending;
static bool s3_mode3_recovery_pending;
static bool s3_mode3_keys_down;
static uint32_t s3_mode3_entered_ms;
static uint32_t s3_mode3_last_event_ms;

#define G4B_MODE3_FAULT_RESULT BIT(0)
#define G4B_MODE3_FAULT_STATS BIT(1)
#define G4B_MODE3_FAULT_TX_AMOUNT BIT(2)
#define G4B_MODE3_FAULT_RX_AMOUNT BIT(3)
#define G4B_MODE3_FAULT_END BIT(4)
#define G4B_MODE3_FAULT_REPLY BIT(5)

static bool s3_mode3_set_period(uint8_t period)
{
    struct g4b_exchange_stats stats = {0};
    enum g4b_spim_result result;
    uint32_t reply;
    uint32_t fault = 0u;

    g4b_mode3_commands++;
    memset(s2_tx, 0, sizeof(s2_tx));
    s2_tx[0] = 0x20u;
    s2_tx[1] = 0x03u;
    s2_tx[2] = period;
    memset(s2_rx, 0, sizeof(s2_rx));
    g4b_spim_arm_ready();
    result = g4b_spim_exchange(s2_tx, s2_rx, &stats);

    reply = (uint32_t)s2_rx[0] | ((uint32_t)s2_rx[1] << 8) |
            ((uint32_t)s2_rx[2] << 16) | ((uint32_t)s2_rx[3] << 24);
    if (result != G4B_SPIM_OK) {
        fault |= G4B_MODE3_FAULT_RESULT;
    }
    if (stats.result != G4B_SPIM_OK) {
        fault |= G4B_MODE3_FAULT_STATS;
    }
    if (stats.tx_amount != G4B_SPIM_FRAME) {
        fault |= G4B_MODE3_FAULT_TX_AMOUNT;
    }
    if (stats.rx_amount != G4B_SPIM_FRAME) {
        fault |= G4B_MODE3_FAULT_RX_AMOUNT;
    }
    if (stats.events_end == 0u) {
        fault |= G4B_MODE3_FAULT_END;
    }
    if (s2_rx[0] != 0x20u || s2_rx[1] != 0x00u || s2_rx[2] != 0x00u) {
        fault |= G4B_MODE3_FAULT_REPLY;
    }

    g4b_mode3_last_result = (uint32_t)result;
    g4b_mode3_last_reply = reply;
    g4b_mode3_last_tx_amount = stats.tx_amount;
    g4b_mode3_last_rx_amount = stats.rx_amount;
    g4b_mode3_last_events_end = stats.events_end;
    g4b_mode3_last_wait_tx_us = stats.wait_tx_us;
    g4b_mode3_last_wait_rx_us = stats.wait_rx_us;
    g4b_mode3_last_fault = fault;
    if (fault != 0u && g4b_mode3_failure_fault == 0u) {
        /* Preserve the failed command even if a following period-1 repair
         * succeeds and overwrites the ordinary last-command fields. */
        g4b_mode3_failure_result = (uint32_t)result;
        g4b_mode3_failure_reply = reply;
        g4b_mode3_failure_fault = fault;
    }
    return fault == 0u;
}

static bool s3_mode3_wanted(void)
{
    bool powered = false;
    uint32_t now = k_uptime_get_32();

#if IS_ENABLED(CONFIG_ZMK_USB)
    powered = s3_usb_is_powered();
#endif
    return !powered && g4b_mode_get() == G4B_MODE_BT &&
           s3_cfg_dirty == 0u && !s3_mode3_keys_down &&
           (now - s3_last_activity_ms) >=
               (uint32_t)CONFIG_APEX_G4B_IDLE_AFTER_MS &&
           (now - s3_mode3_last_event_ms) >=
               (uint32_t)CONFIG_APEX_G4B_IDLE_AFTER_MS;
}

static uint8_t s3_mode3_target_period(void)
{
    uint32_t now;

    if (!s3_mode3_wanted()) {
        return 0u;
    }

    now = k_uptime_get_32();
#if CONFIG_APEX_G4B_STM32_LONG_IDLE_SCAN_PERIOD_MS > 1 && \
    CONFIG_APEX_G4B_STM32_LONG_IDLE_AFTER_MS > 0
    if ((now - s3_last_activity_ms) >=
            (uint32_t)CONFIG_APEX_G4B_STM32_LONG_IDLE_AFTER_MS &&
        (now - s3_mode3_last_event_ms) >=
            (uint32_t)CONFIG_APEX_G4B_STM32_LONG_IDLE_AFTER_MS) {
        return (uint8_t)CONFIG_APEX_G4B_STM32_LONG_IDLE_SCAN_PERIOD_MS;
    }
#endif
    return (uint8_t)CONFIG_APEX_G4B_STM32_IDLE_SCAN_PERIOD_MS;
}

/* Called at the start of the ATTN-low branch. Normalize after a key/entry race,
 * or cancel slow cadence when USB power, mode, or config state changed. */
static bool s3_mode3_normalize_if_needed(void)
{
    bool wanted = s3_mode3_wanted();
    bool cancellation = s3_mode3_slow_requested && !s3_mode3_resume_seen &&
                        !wanted;
    bool repair = s3_mode3_repair_pending;

    if (s3_mode3_recovery_pending) {
        bool recovered;
        bool deferred;

        g4b_attn_drain();
        if (g4b_pin_read(G4B_PORT0, G4B_P0_ATTN)) {
            return true;
        }
        if (s3_mode3_keys_down) {
            /* ATTN low only says that no report is queued. A key may still be
             * physically held after its press report was consumed, so wait
             * for the release A1 before power-cycling scanner state. */
            return false;
        }
        recovered = s3_recover_scanner(true, &deferred);
        if (deferred) {
            /* The key owns the bus now. Leave recovery pending; the next
             * confirmed-low pass will retry after A1 has been serviced. */
            return true;
        }
        if (recovered) {
            g4b_mode3_recoveries++;
            g4b_mode3_expected_period = 1u;
        } else {
            g4b_mode3_expected_period = UINT32_MAX;
        }
        s3_mode3_slow_requested = false;
        s3_mode3_resume_seen = false;
        s3_mode3_recovery_pending = false;
        /* Restart after a power-cycle/replay rather than issuing any more idle
         * work against freshly reconstructed scanner state. */
        return true;
    }

    if (repair ||
        (s3_mode3_slow_requested && (s3_mode3_resume_seen || !wanted))) {
        g4b_attn_drain();
        if (g4b_pin_read(G4B_PORT0, G4B_P0_ATTN)) {
            return true;
        }
        if (s3_mode3_set_period(1u)) {
            s3_mode3_slow_requested = false;
            s3_mode3_resume_seen = false;
            s3_mode3_repair_pending = false;
            g4b_mode3_expected_period = 1u;
            g4b_mode3_fast_overrides++;
            if (cancellation) {
                g4b_mode3_cancellations++;
            }
        } else {
            g4b_mode3_failures++;
            g4b_mode3_expected_period = UINT32_MAX;
            s3_mode3_disabled = true;
            if (repair) {
                /* A second period-1 attempt failed. Defer destructive recovery
                 * to another confirmed-low pass so an ATTN raised during this
                 * exchange is serviced first. */
                s3_mode3_repair_pending = false;
                s3_mode3_recovery_pending = true;
            } else {
                /* Retry period 1 once before falling back to power-cycle/replay. */
                s3_mode3_repair_pending = true;
            }
            return true;
        }
        return g4b_pin_read(G4B_PORT0, G4B_P0_ATTN);
    }

    /* The outer loop observed ATTN low, but it may have risen since. Never let
     * housekeeping get ahead of A1 merely because no normalization was due. */
    return g4b_pin_read(G4B_PORT0, G4B_P0_ATTN);
}

/* Called after every other scanner transaction, immediately before the ATTN
 * wait. This ordering makes a successful slow write the final idle command. */
static bool s3_mode3_enter_if_needed(void)
{
    uint8_t target_period = s3_mode3_target_period();
    bool long_tier;

    if (s3_mode3_disabled || target_period <= 1u ||
        (s3_mode3_slow_requested &&
         g4b_mode3_expected_period == (uint32_t)target_period)) {
        return g4b_pin_read(G4B_PORT0, G4B_P0_ATTN);
    }

    /* Close the edge-vs-level race before entry. A new edge remains queued in
     * the semaphore, and the physical level is checked again after the reply. */
    g4b_attn_drain();
    if (g4b_pin_read(G4B_PORT0, G4B_P0_ATTN)) {
        return true;
    }
    /* Power/mode/config state can change after the first wanted snapshot. Do
     * not cross that race with the command that selects slow cadence. */
    target_period = s3_mode3_target_period();
    if (target_period <= 1u) {
        return g4b_pin_read(G4B_PORT0, G4B_P0_ATTN);
    }
    long_tier =
#if CONFIG_APEX_G4B_STM32_LONG_IDLE_SCAN_PERIOD_MS > 1
        target_period ==
            (uint8_t)CONFIG_APEX_G4B_STM32_LONG_IDLE_SCAN_PERIOD_MS;
#else
        false;
#endif
    g4b_mode3_entry_attempts++;
    if (long_tier) {
        g4b_mode3_long_entry_attempts++;
    }
    if (s3_mode3_set_period(target_period)) {
        if (!s3_mode3_slow_requested) {
            s3_mode3_entered_ms = k_uptime_get_32();
        }
        s3_mode3_slow_requested = true;
        s3_mode3_resume_seen = false;
        g4b_mode3_expected_period = (uint32_t)target_period;
        g4b_mode3_entries++;
        if (long_tier) {
            g4b_mode3_long_entries++;
        }
    } else {
        g4b_mode3_failures++;
        /* The failed exchange might still have changed the scanner. Mark the
         * cadence unknown, disable future entries, and repair only on the next
         * confirmed-low loop pass so a key raised during the command wins. */
        g4b_mode3_expected_period = UINT32_MAX;
        s3_mode3_slow_requested = true;
        s3_mode3_resume_seen = false;
        s3_mode3_repair_pending = true;
        s3_mode3_disabled = true;
        return true;
    }
    return g4b_pin_read(G4B_PORT0, G4B_P0_ATTN);
}
#else
static inline bool s3_mode3_normalize_if_needed(void) { return false; }
static inline bool s3_mode3_enter_if_needed(void) { return false; }
#endif

#if CONFIG_APEX_G4B_STM32_STOP1_IDLE_MS > 0
/* Bounded reproduction of the failed scanner stop/resume theory, without
 * Nordic System OFF. Both 2026-09-03 canaries failed; this remains available
 * only as forensic evidence and is compiled out of normal builds.
 *
 * This is deliberately separate from s3_sleep_maybe(): the Nordic stays in
 * System ON and BLE/USB remain alive. The experiment incorrectly combined two
 * stock sequences that belong to different state machines:
 *
 *   hard stop:   TX/RX 20 00 01 02 (STM32 state becomes zero)
 *   link resync: SPIM3 off, MOSI GPIO high, READY low, SPIM3/MOSI-low, READY high
 *
 * A mode-2 SET_MODE write is not used here. In scanner state 2 that handler
 * deliberately omits its response phase, and treating it as a normal exchange
 * strands the link waiting for READY. Stock's actual nRF link-sleep path sends
 * no 0x20 and leaves the scanner running so ATTN can wake the Nordic.
 */
static bool s3_stop1_failed;
static bool s3_stop1_report_pending;
static bool s3_stop1_report_recovered;
static uint32_t s3_stop1_entries;
static uint32_t s3_stop1_wait_low_us;
static uint32_t s3_stop1_wait_ready_us;
static uint32_t s3_stop1_fail_tx_amount;
static uint32_t s3_stop1_fail_rx_amount;
static uint32_t s3_stop1_fail_wait_tx_us;
static uint32_t s3_stop1_fail_wait_rx_us;
static uint8_t s3_stop1_fail_rx[4];

#if IS_ENABLED(CONFIG_APEX_G4B_UART_EVIDENCE)
static void s3_stop1_emit_enter(void)
{
    static uint8_t line[64];
    uint32_t n = 0u;

    if (s3_stop1_entries > 3u && (s3_stop1_entries & 0xFFu) != 0u) {
        return;
    }

    n += txt_str(&line[n], "APXSTOP enter n=");
    n += txt_u32(&line[n], s3_stop1_entries);
    n += txt_str(&line[n], " up=");
    n += txt_u32(&line[n], k_uptime_get_32());
    line[n++] = (uint8_t)13;
    line[n++] = (uint8_t)10;
    g4b_evidence_emit_text(line, n);
}

static void s3_stop1_emit_wake(void)
{
    static uint8_t line[96];
    uint32_t n = 0u;

    if (s3_stop1_entries > 3u && (s3_stop1_entries & 0xFFu) != 0u) {
        return;
    }

    n += txt_str(&line[n], "APXSTOP wake n=");
    n += txt_u32(&line[n], s3_stop1_entries);
    n += txt_str(&line[n], " low_us=");
    n += txt_u32(&line[n], s3_stop1_wait_low_us);
    n += txt_str(&line[n], " ready_us=");
    n += txt_u32(&line[n], s3_stop1_wait_ready_us);
    n += txt_str(&line[n], " recovered=");
    n += txt_u32(&line[n], s3_stop1_report_recovered ? 1u : 0u);
    line[n++] = (uint8_t)13;
    line[n++] = (uint8_t)10;
    g4b_evidence_emit_text(line, n);
}

static void s3_stop1_emit_fail(uint32_t code, bool recovered)
{
    static uint8_t line[160];
    uint32_t n = 0u;

    n += txt_str(&line[n], "APXSTOP fail code=");
    n += txt_u32(&line[n], code);
    n += txt_str(&line[n], " recovered=");
    n += txt_u32(&line[n], recovered ? 1u : 0u);
    n += txt_str(&line[n], " tx=");
    n += txt_u32(&line[n], s3_stop1_fail_tx_amount);
    n += txt_str(&line[n], " rx=");
    n += txt_u32(&line[n], s3_stop1_fail_rx_amount);
    n += txt_str(&line[n], " wait=");
    n += txt_u32(&line[n], s3_stop1_fail_wait_tx_us);
    line[n++] = (uint8_t)'/';
    n += txt_u32(&line[n], s3_stop1_fail_wait_rx_us);
    n += txt_str(&line[n], " bytes=");
    for (uint32_t i = 0u; i < ARRAY_SIZE(s3_stop1_fail_rx); i++) {
        if (i != 0u) {
            line[n++] = (uint8_t)',';
        }
        n += txt_u32(&line[n], s3_stop1_fail_rx[i]);
    }
    line[n++] = (uint8_t)13;
    line[n++] = (uint8_t)10;
    g4b_evidence_emit_text(line, n);
}
#else
static inline void s3_stop1_emit_enter(void) {}
static inline void s3_stop1_emit_wake(void) {}
static inline void s3_stop1_emit_fail(uint32_t code, bool recovered)
{
    ARG_UNUSED(code);
    ARG_UNUSED(recovered);
}
#endif

static void s3_stop1_emit_pending(void)
{
    if (!s3_stop1_report_pending ||
        g4b_pin_read(G4B_PORT0, G4B_P0_ATTN)) {
        return;
    }

    /* Rate-limited by s3_stop1_emit_wake(), so a short host-poll interval does
     * not flood CDC/UART or materially change the measured duty cycle. */
    s3_stop1_report_pending = false;
    s3_stop1_emit_wake();
}

static void s3_stop1_maybe(void)
{
    struct g4b_exchange_stats stats = {0};
    bool powered = false;
    bool restored = true;
    uint32_t fail_code = 0u;
    enum g4b_spim_result result;

    if (s3_stop1_failed || s3_cfg_dirty != 0u ||
        (k_uptime_get_32() - s3_last_activity_ms) <
            (uint32_t)CONFIG_APEX_G4B_STM32_STOP1_IDLE_MS) {
        return;
    }

#if IS_ENABLED(CONFIG_ZMK_USB)
    powered = s3_usb_is_powered();
#endif
    if (powered) {
        if (!IS_ENABLED(CONFIG_APEX_G4B_STM32_STOP1_ALLOW_USB)) {
            return;
        }
    } else if (g4b_mode_get() != G4B_MODE_BT) {
        return;
    }
    if (g4b_mode_get() == G4B_MODE_DONGLE) {
        return;
    }

    /* Close the edge-vs-level race before the command: discard only an edge
     * already consumed by the active loop, then re-read the physical line. A
     * new edge after this point remains queued in the semaphore. */
    g4b_attn_drain();
    if (g4b_pin_read(G4B_PORT0, G4B_P0_ATTN)) {
        return;
    }

    memset(s2_tx, 0, sizeof(s2_tx));
    s2_tx[0] = 0x20u;
    s2_tx[1] = 0x00u;
    s2_tx[2] = 0x01u;
    s2_tx[3] = 0x02u;
    memset(s2_rx, 0, sizeof(s2_rx));
    g4b_spim_arm_ready();
    result = g4b_spim_exchange(s2_tx, s2_rx, &stats);

    /* The active -> stopped transition replies 20 00 00. If state was already
     * zero, the handler makes no transition and replies 20 02 01. Both prove
     * mode 0 was accepted; neither reply alone proves that the asynchronous
     * STOP1 task armed. */
    if (result != G4B_SPIM_OK || s2_rx[0] != 0x20u ||
        !((s2_rx[1] == 0x00u && s2_rx[2] == 0x00u) ||
          (s2_rx[1] == 0x02u && s2_rx[2] == 0x01u))) {
        /* If all 64 TX bytes left EasyDMA, mode 0 may already be active even
         * when the reply failed. Try bounded resync, then power-cycle/replay. */
        fail_code = (result != G4B_SPIM_OK) ? (uint32_t)result : 100u;
        s3_stop1_fail_tx_amount = stats.tx_amount;
        s3_stop1_fail_rx_amount = stats.rx_amount;
        s3_stop1_fail_wait_tx_us = stats.wait_tx_us;
        s3_stop1_fail_wait_rx_us = stats.wait_rx_us;
        memcpy(s3_stop1_fail_rx, s2_rx, sizeof(s3_stop1_fail_rx));
        if (stats.tx_amount == G4B_SPIM_FRAME) {
            /* Preserve the exact bounded negative-test sequence. A mode-0
             * scanner cannot satisfy the link-resync READY-low precondition. */
            k_msleep(50);
            restored = g4b_spim_link_resync(&s3_stop1_wait_low_us,
                                            &s3_stop1_wait_ready_us);
            if (!restored) {
                restored = s3_recover_scanner(false, NULL);
            }
        }
        s3_stop1_failed = true;
        s3_last_activity_ms = k_uptime_get_32();
        s3_stop1_emit_fail(fail_code, restored);
        return;
    }

    s3_stop1_entries++;
    s3_stop1_emit_enter();

    /* Preserve the R4 reproduction exactly: wait briefly, then attempt stock's
     * link resync even though mode 0 has removed its READY-low precondition.
     * This is known to fail and reach the power-cycle/replay fallback. */
    if (!g4b_pin_read(G4B_PORT0, G4B_P0_ATTN)) {
        (void)g4b_attn_wait(CONFIG_APEX_G4B_STM32_STOP1_POLL_MS);
    }

    restored = g4b_spim_link_resync(&s3_stop1_wait_low_us,
                                    &s3_stop1_wait_ready_us);
    s3_stop1_report_recovered = false;
    if (!restored) {
        s3_stop1_report_recovered = s3_recover_scanner(false, NULL);
        s3_stop1_failed = true;
        s3_last_activity_ms = k_uptime_get_32();
        if (!s3_stop1_report_recovered) {
            s3_stop1_emit_fail(101u, false);
        }
    }

    /* On success, deliberately do not stamp activity: if A1 reports a real key
     * the normal input path does that. Otherwise the next idle pass re-enters
     * stopped state, forming a Nordic-timed scan duty cycle. */
    s3_stop1_report_pending = restored || s3_stop1_report_recovered;
}
#endif

#if IS_ENABLED(CONFIG_APEX_G4B_ANALOG_PROBE)
/* --- Analog depth sampling -----------------------------------------------
 *
 * 0xA2 returns raw per-key ADC samples. W, A, S and D are scan indices 16, 29,
 * 30 and 31, which is contiguous, so ONE frame covers all four: count 16 from
 * start 16. The reply has NO opcode echo and no header - rx[0] is the first
 * sample - and only the first 2*count bytes are real, the rest being whatever
 * was on the scanner's stack.
 *
 * Both bounds are compile-time constants and asserted, because the scanner
 * signals NO error for a bad count or start: it returns 64 bytes of stale stack
 * that looks exactly like plausible ADC readings.
 *
 * A standalone probe records these values for diagnostics. When the gamepad is
 * enabled, the same samples feed its USB HID axes.
 */
#define G4B_A2_START  16u
#define G4B_A2_COUNT  16u
BUILD_ASSERT(G4B_A2_COUNT <= 32u, "0xA2 caps count at 32");
BUILD_ASSERT(G4B_A2_START + G4B_A2_COUNT <= 70u, "0xA2 start+count must fit 70 keys");

/* Window offsets of the four keys we care about, as sample indices. */
#define G4B_A2_W  (16u - G4B_A2_START)
#define G4B_A2_A  (29u - G4B_A2_START)
#define G4B_A2_S  (30u - G4B_A2_START)
#define G4B_A2_D  (31u - G4B_A2_START)

/* The scanner ignores any sample outside this window and leaves the key's
 * previous value in place, so a reading here is not data.
 */
#define G4B_A2_VALID_LO 337u
#define G4B_A2_VALID_HI 3959u

#define G4B_A2_KEYS  4u
#define G4B_A2_RING  11u

static uint8_t s3_a2_tx[G4B_SPIM_FRAME];
static uint8_t s3_a2_rx[G4B_SPIM_FRAME];
static uint16_t s3_a2_min[G4B_A2_KEYS];
static uint16_t s3_a2_max[G4B_A2_KEYS];
static uint16_t s3_a2_last[G4B_A2_KEYS];
static uint16_t s3_a2_ring[G4B_A2_KEYS][G4B_A2_RING];
/* Two-sample debounce for growing s3_a2_max: candidate value and a pending
 * flag, so a lone deeper spike cannot rescale a key's analog range.
 */
static uint16_t s3_a2_max_cand[G4B_A2_KEYS];
static uint8_t s3_a2_max_pend[G4B_A2_KEYS];
static uint16_t s3_a2_ring_n;
static uint32_t s3_a2_frames;
static uint32_t s3_a2_oob;

#if IS_ENABLED(CONFIG_APEX_G4B_GAMEPAD)
/* Defined below, next to the mapping constants it depends on; declared here
 * because the sampler calls it as soon as a frame lands.
 */
static void s3_gamepad_update(void);
#endif

static bool s3_analog_sample_wanted(void)
{
#if IS_ENABLED(CONFIG_APEX_G4B_GAMEPAD)
    /* The analog gamepad has no BLE report map. Do not spend a 0xA2 exchange
     * every 4 ms unless its USB interface is both usable and explicitly
     * enabled. A standalone ANALOG_PROBE diagnostic build keeps its original
     * unconditional sampling below because collecting those measurements is
     * the entire purpose of that build.
     */
    return s3_usb_is_powered() && g4b_gamepad_is_enabled();
#else
    return true;
#endif
}

static void s3_analog_sample(void)
{
    static const uint8_t idx[G4B_A2_KEYS] = {
        G4B_A2_W, G4B_A2_A, G4B_A2_S, G4B_A2_D
    };
    struct g4b_exchange_stats stats = {0};

    memset(s3_a2_tx, 0, sizeof(s3_a2_tx));
    s3_a2_tx[0] = 0xA2u;
    s3_a2_tx[1] = (uint8_t)G4B_A2_COUNT;
    s3_a2_tx[2] = (uint8_t)G4B_A2_START;
    memset(s3_a2_rx, 0, sizeof(s3_a2_rx));

    g4b_spim_arm_ready();
    if (g4b_spim_exchange(s3_a2_tx, s3_a2_rx, &stats) != G4B_SPIM_OK ||
        stats.result != G4B_SPIM_OK) {
        return;
    }
    s3_a2_frames++;

    for (uint32_t k = 0u; k < G4B_A2_KEYS; k++) {
        uint32_t o = (uint32_t)idx[k] * 2u;
        uint16_t v = (uint16_t)(s3_a2_rx[o] | ((uint16_t)s3_a2_rx[o + 1u] << 8));

        if (v < G4B_A2_VALID_LO || v > G4B_A2_VALID_HI) {
            s3_a2_oob++;
            continue; /* the scanner skipped this key; not a measurement */
        }

        /* Smooth first, then learn and map from the filtered value, using the
         * SAME shared per-slot filter the RGB sweep uses (idx[k] is window
         * relative, so the absolute slot is G4B_A2_START + idx[k]). This kills
         * the per-sample axis jitter and stops a noise spike from being learned
         * as the key's range.
         */
        uint16_t vf = g4b_depth_ingest((uint8_t)(G4B_A2_START + idx[k]), v);

        s3_a2_last[k] = vf;
        if (s3_a2_min[k] == 0u || vf < s3_a2_min[k]) {
            s3_a2_min[k] = vf;
        }
        /* Grow the learned max only after TWO consecutive filtered samples sit
         * above it, so a single deeper wiggle cannot ratchet the ceiling up and
         * step-rescale the whole curve - which is what made the same physical
         * position read differently after a wiggle.
         */
        if (vf > s3_a2_max[k]) {
            if (s3_a2_max_pend[k] != 0u && vf >= s3_a2_max_cand[k]) {
                s3_a2_max[k] = vf;
                s3_a2_max_pend[k] = 0u;
            } else {
                s3_a2_max_cand[k] = vf;
                s3_a2_max_pend[k] = 1u;
            }
        } else {
            s3_a2_max_pend[k] = 0u;
        }
        if (s3_a2_ring_n < G4B_A2_RING) {
            s3_a2_ring[k][s3_a2_ring_n] = vf;
        }
    }
    if (s3_a2_ring_n < G4B_A2_RING) {
        s3_a2_ring_n++;
    }
#if IS_ENABLED(CONFIG_APEX_G4B_GAMEPAD)
    s3_gamepad_update();
#endif
}

#if IS_ENABLED(CONFIG_APEX_G4B_GAMEPAD)
/* Raw counts -> axis, using values measured on the development keyboard:
 *
 *   rest        ~836-870      bottom-out  3199-3556      span 2363-2701
 *   rest noise  8 counts over 11 samples
 *
 * A 5 percent lower deadzone is about 118 counts on the smallest measured span,
 * comfortably above the 8-count rest noise while retaining most of the travel.
 *
 * The 14 percent spread in bottom-out ACROSS four adjacent keys is why min and
 * max are learned per key and never shared: a common constant would make one
 * key reach full lock early and leave another unable to reach it at all.
 */
#define G4B_GP_DZ_LO_PCT   5u   /* ignore the first 5% of travel  */
#define G4B_GP_DZ_HI_PCT   88u  /* full lock by 88%: a normal bottom-out reaches
                                 * it even if a past harder press learned a
                                 * deeper max, so full press is consistent */
#define G4B_GP_LOCK_HYST_PCT 3u  /* stay locked until the key lifts >3% of span,
                                  * so wiggling at the stop does not un-lock */
#define G4B_GP_MIN_SPAN 1200u   /* below this a key is not trusted analog */

/* 0..1024 fixed point. Returns -1 while the key has not yet been pressed far
 * enough to know its own range.
 */
static uint8_t s3_gp_locked[G4B_A2_KEYS];

static int32_t s3_gp_unit(uint32_t k)
{
    uint32_t lo = s3_a2_min[k];
    uint32_t hi = s3_a2_max[k];
    uint32_t v = s3_a2_last[k];
    uint32_t span, dz_lo, dz_hi, unlock, u;

    if (lo == 0u || hi <= lo) {
        return -1;
    }
    span = hi - lo;
    if (span < G4B_GP_MIN_SPAN) {
        return -1;
    }
    if (v <= lo) {
        s3_gp_locked[k] = 0u;
        return 0;
    }

    dz_lo = lo + (span * G4B_GP_DZ_LO_PCT) / 100u;
    dz_hi = lo + (span * G4B_GP_DZ_HI_PCT) / 100u;
    if (v <= dz_lo) {
        s3_gp_locked[k] = 0u;
        return 0;
    }

    /* Full-lock latch with hysteresis: once the key crosses dz_hi it reads full
     * and STAYS full through bottom-out wiggle, only releasing when it lifts
     * back below dz_hi by more than G4B_GP_LOCK_HYST_PCT of the span. Without
     * this, a wiggle at the stop kept re-modulating the axis around full.
     */
    unlock = dz_hi - (span * G4B_GP_LOCK_HYST_PCT) / 100u;
    if (s3_gp_locked[k]) {
        if (v >= unlock) {
            return 1024;
        }
        s3_gp_locked[k] = 0u;
    }
    if (v >= dz_hi) {
        s3_gp_locked[k] = 1u;
        return 1024;
    }
    u = ((v - dz_lo) * 1024u) / (dz_hi - dz_lo);
    return (int32_t)(u > 1024u ? 1024u : u);
}

/* A key whose range is not yet learned still has to do something sensible, and
 * the something is exactly what the keyboard does today: on or off from the
 * key bitmap. So the worst case for an uncalibrated key is the status quo,
 * never something worse, and it becomes analog by itself the first time it is
 * bottomed out.
 */
static int32_t s3_gp_unit_or_digital(uint32_t k, uint8_t scan_bit)
{
    int32_t u = s3_gp_unit(k);

    if (u >= 0) {
        return u;
    }
    if (s3_have_prev &&
        (s3_prev_bitmap[scan_bit >> 3] & (1u << (scan_bit & 7u))) != 0u) {
        return 1024;
    }
    return 0;
}

static void s3_gamepad_update(void)
{
    /* Order matches s3_analog_sample(): W, A, S, D. */
    int32_t w = s3_gp_unit_or_digital(0u, 16u);
    int32_t a = s3_gp_unit_or_digital(1u, 29u);
    int32_t s = s3_gp_unit_or_digital(2u, 30u);
    int32_t d = s3_gp_unit_or_digital(3u, 31u);

    /* Steering is the DIFFERENCE, so both keys held gives counter-steer with no
     * priority rule to invent. Idea from zmk-feature-hall-effect (MIT).
     */
    int32_t x = G4B_GP_CENTRE + ((d - a) * (G4B_GP_MAX / 2)) / 1024;
    int32_t y = G4B_GP_CENTRE + ((s - w) * (G4B_GP_MAX / 2)) / 1024;
    int32_t z = (w * G4B_GP_MAX) / 1024;
    int32_t rz = (s * G4B_GP_MAX) / 1024;

    x = CLAMP(x, 0, G4B_GP_MAX);
    y = CLAMP(y, 0, G4B_GP_MAX);
    z = CLAMP(z, 0, G4B_GP_MAX);
    rz = CLAMP(rz, 0, G4B_GP_MAX);

    /* Rx carries the same steering value as X, on a second (right-stick) axis,
     * so a host mapper can put on-foot steering on one stick and vehicle
     * steering on the other without having to duplicate an axis it cannot.
     */
    g4b_gamepad_publish((uint16_t)x, (uint16_t)y, (uint16_t)z, (uint16_t)rz,
                        (uint16_t)x, 0u);
}
#endif /* CONFIG_APEX_G4B_GAMEPAD */

/* Pack analog measurements into the kbd_cap_* region. Version 39 tells the
 * decoder how to interpret the unchanged 1280-byte record.
 *
 * __maybe_unused for the same reason as s2_run_confirm: the only call site is
 * inside the KBD_TELEMETRY block, and CONFIG_APEX_G4B_KBD_TELEMETRY is off in
 * the shipping build (g4b_usb.conf), so this warns "defined but not used"
 * there today. Silencing it with an attribute rather than a narrower guard
 * keeps every conditional-compilation boundary in this file untouched.
 */
__maybe_unused static void s3_analog_fill(void)
{
    record_s3.kbd_cap_reads = s3_a2_frames;
    record_s3.kbd_cap_ok = s3_a2_oob;
#if IS_ENABLED(CONFIG_APEX_G4B_GAMEPAD)
    /* Gamepad endpoint-health counters reuse three reserved words without
     * changing the version-39 record size. They distinguish these cases:
     *   writes  climbing            -> reports are reaching the host
     *   busy    climbing, writes 0  -> the previous report never completed
     *   err     climbing, writes 0  -> hid_int_ep_write() refuses; -EAGAIN while
     *                                  unconfigured or suspended is normal
     *   all three 0                 -> g4b_gamepad_publish() is never called, so
     *                                  the fault is upstream in the sampler
     *
     * Plain aligned 32-bit loads of statics written by the system workqueue,
     * with no lock, exactly as s3_fill_live() reads the watchdog and mode
     * statics: on this single core such a load cannot tear and the values are
     * only ever read as magnitudes.
     */
    record_s3.gp_writes = g4b_gp_writes;
    record_s3.gp_busy = g4b_gp_busy;
    record_s3.gp_err = g4b_gp_err;
#endif
    for (uint32_t k = 0u; k < G4B_A2_KEYS; k++) {
        record_s3.kbd_cap_hist[k * 3u + 0u] = s3_a2_min[k];
        record_s3.kbd_cap_hist[k * 3u + 1u] = s3_a2_max[k];
        record_s3.kbd_cap_hist[k * 3u + 2u] = s3_a2_last[k];
    }
    record_s3.kbd_cap_samples = s3_a2_ring_n;
    for (uint32_t k = 0u; k < G4B_A2_KEYS; k++) {
        for (uint32_t n = 0u; n < G4B_A2_RING; n++) {
            uint32_t b = (k * G4B_A2_RING + n) * 2u;

            record_s3.kbd_cap_sample[b / 11u][b % 11u] =
                (uint8_t)(s3_a2_ring[k][n] & 0xFFu);
            record_s3.kbd_cap_sample[(b + 1u) / 11u][(b + 1u) % 11u] =
                (uint8_t)(s3_a2_ring[k][n] >> 8);
        }
    }
}
#endif /* CONFIG_APEX_G4B_ANALOG_PROBE */

#if IS_ENABLED(CONFIG_APEX_G4B_UART_EVIDENCE)
/* Boot timing starts at kernel initialization and therefore excludes time spent
 * in the bootloader. The frame count confirms that all 59 scanner setup
 * exchanges completed within their independent replay deadline. */
static uint32_t boot_t_main;
static uint32_t boot_t_bringup;
static uint32_t boot_t_replay;
static uint32_t boot_resetreas;

static void s3_emit_boot_timing(void)
{
    /* Static, not stack: UARTE EasyDMA needs RAM, and this is 128 bytes. */
    static uint8_t line[160];
    uint32_t n = 0u;

    n += txt_str(&line[n], "APXT main=");
    n += txt_u32(&line[n], boot_t_main);
    n += txt_str(&line[n], " bring=");
    n += txt_u32(&line[n], boot_t_bringup);
    n += txt_str(&line[n], " rep=");
    n += txt_u32(&line[n], boot_t_replay);
    n += txt_str(&line[n], " kbd=");
    n += txt_u32(&line[n], k_uptime_get_32());
    n += txt_str(&line[n], " frames=");
    n += txt_u32(&line[n], record_s2r.frames_matched);
    n += txt_str(&line[n], " rr=");
    n += txt_u32(&line[n], boot_resetreas);
    /* A value of one confirms that CN3 DTM pulled P1.07 low at boot. */
    n += txt_str(&line[n], " strap=");
    n += txt_u32(&line[n], g4b_strap_asserted() ? 1u : 0u);
    line[n++] = (uint8_t)13;  /* CR */
    line[n++] = (uint8_t)10;  /* LF */

    g4b_evidence_emit_text(line, n);
}
#else
static inline void s3_emit_boot_timing(void) {}
#endif

#if IS_ENABLED(CONFIG_APEX_G4B_BATT_PROBE)
/* PSELP values, not pin numbers: AINn is n+1, and the two supply inputs sit
 * outside that range entirely. P0.28/29/30/31 are AIN4..AIN7.
 */
#define G4B_BATT_PSELP_A28    5u
#define G4B_BATT_PSELP_A29    6u
#define G4B_BATT_PSELP_A30    7u
#define G4B_BATT_PSELP_A31    8u
#define G4B_BATT_PSELP_VDD    9u
#define G4B_BATT_PSELP_VDDH5  0x0Du

static uint32_t s3_batt_last_ms;

static uint32_t batt_mv(uint8_t pselp)
{
    int16_t raw = g4b_saadc_read(pselp);

    /* Same scaling as mode_sample(): gain 1/6 against the 0.6 V internal
     * reference is 3.6 V full scale over 12 bits.
     */
    return (raw < 0) ? 0u : ((uint32_t)raw * 3600u / 4096u);
}

static void s3_batt_probe(void)
{
    static uint8_t line[192];
    uint32_t now = k_uptime_get_32();
    uint32_t n = 0u;

    if (s3_batt_last_ms != 0u && (now - s3_batt_last_ms) < 30000u) {
        return;
    }
    s3_batt_last_ms = now;

    n += txt_str(&line[n], "APXV a28=");
    n += txt_u32(&line[n], batt_mv(G4B_BATT_PSELP_A28));
    n += txt_str(&line[n], " a29=");
    n += txt_u32(&line[n], batt_mv(G4B_BATT_PSELP_A29));
    n += txt_str(&line[n], " a30=");
    n += txt_u32(&line[n], batt_mv(G4B_BATT_PSELP_A30));
    n += txt_str(&line[n], " a31=");
    n += txt_u32(&line[n], batt_mv(G4B_BATT_PSELP_A31));
    n += txt_str(&line[n], " vdd=");
    n += txt_u32(&line[n], batt_mv(G4B_BATT_PSELP_VDD));
    /* VDDH/5: multiply by 5 for the real rail, done here so the line reads as
     * millivolts throughout and nothing has to be scaled by hand later.
     */
    n += txt_str(&line[n], " vddh=");
    n += txt_u32(&line[n], batt_mv(G4B_BATT_PSELP_VDDH5) * 5u);
    n += txt_str(&line[n], " up=");
    n += txt_u32(&line[n], now);
    line[n++] = (uint8_t)13;
    line[n++] = (uint8_t)10;

    g4b_evidence_emit_text(line, n);
}
#else
static inline void s3_batt_probe(void) {}
#endif

#if IS_ENABLED(CONFIG_APEX_G4B_TWIM_PROBE)
/* What the vendor loader left in the two TWIM instances.
 *
 * PURELY OBSERVATIONAL. Nothing is enabled, no pin is driven, no byte reaches
 * any bus. These are the same registers, read the same way, that identified
 * SPIM2's SCK and MOSI for the RGB controller - the loader configures a
 * peripheral and its PSEL registers still hold the pin assignment afterwards.
 *
 * The question is where the BQ25895 sits. Every SAADC input is accounted for
 * (AIN0/2/3 are the STM32 enable, SPIM3 SCK and ready; AIN1 is the mode switch;
 * AIN4-7 were swept and none of them moved when the battery was physically
 * disconnected), so there is no external divider to find. The charger has its
 * own ADC and reports VBAT over I2C, which is the remaining explanation.
 *
 * SPIM and TWIM share a base address and only one can be enabled at a time, so
 * both instances are read. TWIM0/SPIM0 serves the SPI-NOR flash; TWIM1/SPIM1
 * serves the charger bus on P0.16/P0.17.
 */
#define G4B_TWIM0_BASE 0x40003000u
#define G4B_TWIM1_BASE 0x40004000u
#define G4B_TWIM_ENABLE    0x500u
#define G4B_TWIM_PSEL_SCL  0x508u
#define G4B_TWIM_PSEL_SDA  0x50Cu
#define G4B_TWIM_FREQUENCY 0x524u
#define G4B_TWIM_ADDRESS   0x588u

static uint32_t twim_snap[10];

static void s3_twim_snapshot(void)
{
    const uint32_t bases[2] = { G4B_TWIM0_BASE, G4B_TWIM1_BASE };

    for (uint32_t i = 0u; i < 2u; i++) {
        const uint32_t b = bases[i];

        twim_snap[i * 5u + 0u] = *(volatile uint32_t *)(b + G4B_TWIM_ENABLE);
        twim_snap[i * 5u + 1u] = *(volatile uint32_t *)(b + G4B_TWIM_PSEL_SCL);
        twim_snap[i * 5u + 2u] = *(volatile uint32_t *)(b + G4B_TWIM_PSEL_SDA);
        twim_snap[i * 5u + 3u] = *(volatile uint32_t *)(b + G4B_TWIM_FREQUENCY);
        twim_snap[i * 5u + 4u] = *(volatile uint32_t *)(b + G4B_TWIM_ADDRESS);
    }
}

static void s3_emit_twim(void)
{
    static uint8_t line[224];
    static const char *tag[10] = {
        "t0en=", " t0scl=", " t0sda=", " t0freq=", " t0addr=",
        " t1en=", " t1scl=", " t1sda=", " t1freq=", " t1addr="
    };
    uint32_t n = 0u;

    n += txt_str(&line[n], "APXI ");
    for (uint32_t i = 0u; i < 10u; i++) {
        n += txt_str(&line[n], tag[i]);
        n += txt_u32(&line[n], twim_snap[i]);
    }
    line[n++] = (uint8_t)13;
    line[n++] = (uint8_t)10;

    g4b_evidence_emit_text(line, n);
}
#else
static inline void s3_twim_snapshot(void) {}
static inline void s3_emit_twim(void) {}
#endif

#if IS_ENABLED(CONFIG_APEX_G4B_BQ_PROBE)
/* Charger probe. Reads four registers, writes none - see twi_g4b.h. */
static uint32_t s3_bq_last_ms;

static uint32_t bq_field(uint8_t reg, uint8_t *err)
{
    struct g4b_twi_result r = g4b_bq_read(reg);

    *err |= r.errorsrc;
    return r.ok ? (uint32_t)r.value : 0xFFFFFFFFu;
}

static void s3_bq_probe(void)
{
    static uint8_t line[224];
    uint32_t now = k_uptime_get_32();
    uint8_t err = 0u;
    uint32_t n = 0u;
    uint32_t batv;

    if (s3_bq_last_ms != 0u && (now - s3_bq_last_ms) < 30000u) {
        return;
    }
    s3_bq_last_ms = now;

    n += txt_str(&line[n], "APXC st0b=");
    n += txt_u32(&line[n], bq_field(G4B_BQ_REG_STATUS, &err));
    n += txt_str(&line[n], " fault0c=");
    n += txt_u32(&line[n], bq_field(G4B_BQ_REG_FAULT, &err));
    batv = bq_field(G4B_BQ_REG_BATV, &err);
    n += txt_str(&line[n], " batv0e=");
    n += txt_u32(&line[n], batv);
    /* Decoded here so the capture is readable without a datasheet. Only
     * meaningful once the ADC has run; with the charger left in its autonomous
     * default this may well read 0, which is itself worth knowing.
     */
    n += txt_str(&line[n], " mv=");
    n += txt_u32(&line[n], (batv == 0xFFFFFFFFu) ? 0u
                                                 : 2304u + 20u * (batv & 0x7Fu));
    n += txt_str(&line[n], " vbus11=");
    n += txt_u32(&line[n], bq_field(G4B_BQ_REG_VBUSV, &err));
    /* The one-shot conversion, and the only write this build makes to the
     * charger. Reported separately from the passive batv0e above so the
     * capture shows both: batv0e is what the register held with the ADC idle,
     * shot= is what one conversion produced.
     */
    {
        uint32_t mv = g4b_bq_sample_mv();

        n += txt_str(&line[n], " shot=");
        n += txt_u32(&line[n], mv);
        n += txt_str(&line[n], " pct=");
        n += txt_u32(&line[n], g4b_bq_percent(mv));
        /* Charge current reads from the same conversion sample_mv triggered, so
         * it must come after it. Reported alongside the charging flag because 0
         * mA and "not applicable" are indistinguishable in the register.
         */
        n += txt_str(&line[n], " ma=");
        n += txt_u32(&line[n], g4b_bq_charge_ma());
        n += txt_str(&line[n], " chg=");
        n += txt_u32(&line[n], g4b_bq_is_charging() ? 1u : 0u);
    }
    n += txt_str(&line[n], " err=");
    n += txt_u32(&line[n], err);
    line[n++] = (uint8_t)13;
    line[n++] = (uint8_t)10;

    g4b_evidence_emit_text(line, n);
}
#else
static inline void s3_bq_probe(void) {}
#endif

#if IS_ENABLED(CONFIG_APEX_G4B_AB_ROLLBACK)
/* USB is part of the health check only when it is the active output. Wired and
 * dongle switch positions always expect USB when VBUS is present. In Bluetooth
 * mode, a charger or battery bank leaves BLE selected and must not prevent a
 * healthy boot. */
static bool ab_output_ready(void)
{
#if IS_ENABLED(CONFIG_ZMK_USB)
    bool usb_required = s3_usb_is_powered() &&
                        (g4b_mode_get() != G4B_MODE_BT ||
                         zmk_endpoint_get_selected().transport ==
                             ZMK_TRANSPORT_USB);

    return !usb_required || zmk_usb_is_hid_ready();
#else
    return true;
#endif
}

/* Count good scanner exchanges from both the key path and the idle keep-alive.
 * Once the active output is ready, the tally is cleared and the running image
 * may be copied into the recovery slot. */
#define G4B_AB_HEALTH_FRAMES 8u
static void ab_note_health(enum g4b_spim_result r)
{
    static uint32_t frames;

    if (r == G4B_SPIM_OK && ab_output_ready()) {
        if (++frames >= G4B_AB_HEALTH_FRAMES) {
            g4b_ab_mark_healthy();
        }
    } else {
        /* A failed scanner exchange or unavailable selected output starts the
         * count over. */
        frames = 0u;
    }
}
#endif

static void s3_run_keyboard(void)
{
    uint32_t last_a0 = g4b_cyccnt();
#if IS_ENABLED(CONFIG_APEX_G4B_ANALOG_PROBE)
    uint32_t last_a2 = g4b_cyccnt();
#endif
#if IS_ENABLED(CONFIG_APEX_G4B_KBD_CAPTURE)
    uint32_t cap_start_ms = k_uptime_get_32();
#elif IS_ENABLED(CONFIG_APEX_G4B_KBD_TELEMETRY)
    uint32_t last_tele_ms = k_uptime_get_32();
#endif

    /* Enables up before the peripheral. g4b_spim_enable() restores SCK/MOSI/
     * MISO (spim_g4b.c:78-82) but knows nothing about P0.02/P1.05, so clocking
     * first would clock at a scanner that is not switched on.
     */
    keyboard_link_reassert();

    g4b_spim_enable();

    /* Interrupt-drive the STM32 attention line so the idle wait at the bottom of
     * the loop wakes on the rising edge of a key event instead of polling. Done
     * after keyboard_link_reassert() has set P0.24 back to an input with its
     * buffer connected, which GPIOTE needs to sense the edge. */
    g4b_gpiote_attn_configure();

    s3_emit_boot_timing();
    s3_emit_twim();

#if IS_ENABLED(CONFIG_APEX_G4B_SPINOR_DUMP)
    /* One-shot, read-only dump of the external SPI-NOR before the scan loop, so
     * the whole chip (and the staged fs=3/file=11 image) is captured over UART.
     * SPIM0 is a separate peripheral from the scanner link (SPIM3), so this does
     * not disturb the keyboard bring-up around it. */
    g4b_spinor_dump();
#endif
#if IS_ENABLED(CONFIG_APEX_G4B_SPINOR_WRITETEST)
    /* One-shot SPIM0 write self-test against a spare, blank region (0x080000) -
     * touches no config, no profiles, and NOT the staged firmware. Reports the
     * result over UART (APXW ...). Proves erase+program+read-back before the
     * real staged-file write is ever attempted. */
    g4b_spinor_write_test();
#endif

#if IS_ENABLED(CONFIG_APEX_G4B_CHARGE_LIMIT)
    /* Charge configuration, once, before any sampling.
     *
     * Caps the charge voltage at 4.096 V for cell longevity (see
     * APEX_G4B_CHARGE_LIMIT) and sets the current to stock's 1472 mA, having
     * disarmed the charger's watchdog first so the settings persist rather than
     * reverting after 40 s. Deliberately NOT gated on the read-only BQ_PROBE
     * diagnostic: the cap has to apply in the shipping build, where that is off.
     * Failure is not fatal and not retried: every intermediate state is either
     * the power-on default or a lower limit than the target, so the worst case
     * is a slightly higher ceiling than intended, never an over-charge.
     */
    (void)g4b_bq_configure_charge();
#endif

#if CONFIG_APEX_G4B_SLEEP_MS > 0
    /* After keyboard_link_reassert(), never before: that writes PIN_CNF for
     * ATTN wholesale and would clear the SENSE field this depends on.
     */
    g4b_sleep_arm();
#endif

#if IS_ENABLED(CONFIG_APEX_G4B_PIN_BEACON)
    g4b_pin_beacon_init();
#endif

    for (;;) {
        struct g4b_exchange_stats stats = {0};

        g4b_wdt_keyboard_heartbeat();

#if CONFIG_APEX_G4B_STM32_STOP1_IDLE_MS > 0
        s3_stop1_emit_pending();
#endif

#if IS_ENABLED(CONFIG_APEX_G4B_RGB_PROBE)
        /* Colour-mapping probe driven from the Nordic scan thread, independent of
         * ZMK underglow state and persisted settings. At the 1 ms loop cadence,
         * 3000 iterations gives approximately three seconds per colour.
         */
        {
            static uint32_t probe_tick;
            static uint32_t probe_step = 0xFFFFFFFFu;
            static const uint8_t pat[5][3] = {
                {255, 0, 0}, {0, 255, 0}, {0, 0, 255},
                {255, 255, 255}, {0, 0, 0}
            };
            uint32_t step = (probe_tick++ / 3000u) % 5u;

            if (step != probe_step) {
                probe_step = step;
                for (uint8_t i = 0u; i < G4B_RGB_LEDS; i++) {
                    g4b_rgb_set_raw(i, pat[step][0], pat[step][1],
                                    pat[step][2]);
                }
                g4b_rgb_mark_pending();
            }
        }
#endif
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
        /* Transmit staged RGB frames on the scanner thread before arming a read.
         * This keeps every SPIM2 transfer in one context without another lock.
         * A pending frame takes about 400 us at the roughly 50 Hz RGB cadence. */
        s3_rgb_idle_update();
        /* Our effect engine gets first refusal on the frame. In passthrough it
         * returns false and everything below behaves exactly as before; while
         * an effect is selected it stages its own frame and ZMK's staged one
         * is simply not transmitted. Either way the flush is unchanged, so the
         * idle blanker and the rail cycling still gate the output.
         */
        (void)g4b_fx_tick();
        g4b_rgb_flush();
#endif

#if CONFIG_APEX_G4B_SLEEP_MS > 0
        g4b_sleep_poll_latch();
#endif

#if !IS_ENABLED(CONFIG_APEX_G4B_KBD_CAPTURE) && \
    IS_ENABLED(CONFIG_APEX_G4B_KBD_TELEMETRY)
        /* Diagnostic telemetry covers watchdog feeds, switch voltage, scanner
         * ingest and liveness counts, and remaining stack space. It is disabled
         * by default: two 1280-byte busy-wait UART writes at 19200 baud occupy
         * this thread for about 1.3 seconds and can starve ZMK Studio. */
        uint32_t tnow = k_uptime_get_32();

        if ((tnow - last_tele_ms) >= 10000u) {
            last_tele_ms = tnow;
            size_t stack_unused = 0;

            (void)k_thread_stack_space_get(k_current_get(), &stack_unused);
            /* wdt_stopped alone could not distinguish "the switch really is in
             * the dongle position" from "the mapping is wrong for this unit".
             * s3_fill_live() adds the millivolts, the sticky swing flags and the
             * sampler timestamp beside it, for zero extra UART bytes - the
             * fields reuse declared-spare padding. See evidence_g4b.h.
             */
            s3_fill_live();
#if IS_ENABLED(CONFIG_APEX_G4B_ANALOG_PROBE)
            /* A probe build reports the analog measurements instead of the
             * ingest counters - the two share this region and a probe run is
             * about the samples.
             */
            s3_analog_fill();
            record_s3.kbd_cap_max = (uint16_t)stack_unused;
#else
            record_s3.kbd_cap_reads = s3_ingest_events;
            record_s3.kbd_cap_ok = s3_a0_polls;
            record_s3.kbd_cap_max = (uint16_t)stack_unused;
#endif
#if IS_ENABLED(CONFIG_APEX_G4B_ANALOG_PROBE)
            s2_fill_head(&record_s3.head, G4B_EVIDENCE_VERSION_S3_ANALOG,
                         record_s3.head.resetreas, s2_launch_kind);
#else
            s2_fill_head(&record_s3.head, G4B_EVIDENCE_VERSION_S3_DIAG,
                         record_s3.head.resetreas, s2_launch_kind);
#endif
            g4b_evidence_emit_s3(&record_s3);
        }
#endif

        if (g4b_pin_read(G4B_PORT0, G4B_P0_ATTN)) {
#if CONFIG_APEX_G4B_STM32_IDLE_SCAN_PERIOD_MS > 1
            {
                uint32_t event_ms = k_uptime_get_32();

                s3_mode3_last_event_ms = event_ms;
                if (s3_mode3_slow_requested && !s3_mode3_resume_seen) {
                    uint32_t delay_ms = event_ms - s3_mode3_entered_ms;

                    /* The callback has run before ATTN, but an entry command
                     * racing it may have re-selected slow afterward. Until the
                     * period-1 normalization succeeds, the effective selector
                     * ordering is intentionally recorded as unknown. */
                    s3_mode3_resume_seen = true;
                    g4b_mode3_expected_period = UINT32_MAX;
                    g4b_mode3_attn_resumes++;
                    /* This is time spent in the slow-request window. The
                     * firmware cannot know when the human began pressing, so
                     * real key-to-ATTN latency still needs external timing. */
                    g4b_mode3_last_slow_residency_ms = delay_ms;
                    if (delay_ms > g4b_mode3_max_slow_residency_ms) {
                        g4b_mode3_max_slow_residency_ms = delay_ms;
                    }
                }
            }
#endif
            /* Read one 0xA1 report whenever ATTN is high. The bitmap is absolute
             * state, so redundant reads are idempotent. The loop's 1 ms sleep
             * sets the maximum active cadence.
             */
            memset(s2_tx, 0, sizeof(s2_tx));
            s2_tx[0] = 0xA1u;
            memset(s2_rx, 0, sizeof(s2_rx));

            g4b_spim_arm_ready();
            (void)g4b_spim_exchange(s2_tx, s2_rx, &stats);

#if IS_ENABLED(CONFIG_APEX_G4B_KBD_CAPTURE)
            record_s3.kbd_cap_reads++;
#endif

            if (stats.result != G4B_SPIM_OK) {
                k_msleep(1);
                continue;
            }

            /* Stock stamps the A0 timer whenever it takes the A1 path (0x259F8):
             * a successful A1 has already proven the link alive, so the 0xA0
             * keep-alive below need not contend with a key burst. Stamped after
             * the OK check rather than with the read, so a FAILING A1 cannot
             * silence the one transaction that reports rx[1]==1, i.e. "the
             * scanner is still configured".
             */
            last_a0 = g4b_cyccnt();

#if IS_ENABLED(CONFIG_APEX_G4B_AB_ROLLBACK)
            /* HEALTHY gate for A/B: a good 0xA1 exchange (already OK-checked
             * above). Also driven from the idle 0xA0 keep-alive below, so an
             * idle keyboard - which never takes this key path - still becomes
             * healthy and clears the tally. */
            ab_note_health(G4B_SPIM_OK);
#endif

#if !IS_ENABLED(CONFIG_APEX_G4B_KBD_CAPTURE)
            /* Clear unmapped scanner positions with the fixed matrix mask used
             * by the kscan driver. This keeps unused bits out of the key-count
             * limit and raw-bitmap deduplication without discarding initial key
             * transitions. */
            {
                const uint8_t *vm = apex_g4b_kscan_valid_mask();

                for (uint32_t i = 0U; i < APEX_G4B_KEY_BITMAP_SIZE; i++) {
                    s2_rx[i] &= vm[i];
                }
            }
#endif

            uint32_t bits = 0U;
            for (uint32_t i = 0U; i < APEX_G4B_KEY_BITMAP_SIZE; i++) {
                bits += (uint32_t)__builtin_popcount(s2_rx[i]);
            }
#if CONFIG_APEX_G4B_STM32_IDLE_SCAN_PERIOD_MS > 1
            /* ATTN low means no report is queued, not necessarily no key is
             * held. Remember the absolute bitmap independently of ZMK ingest,
             * and never slow the release path of a held key. */
            s3_mode3_keys_down = bits != 0u;
#endif

#if IS_ENABLED(CONFIG_APEX_G4B_BAG_GUARD) && \
    !IS_ENABLED(CONFIG_APEX_G4B_KBD_CAPTURE)
            {
                bool bag_powered = false;
#if IS_ENABLED(CONFIG_ZMK_USB)
                bag_powered = s3_usb_is_powered();
#endif
                /* Bag guard uses sustained key count rather than activity time,
                 * because pressure can make individual Hall switches chatter.
                 * It engages only on battery, uses hysteresis until the keyboard
                 * is calm, suppresses the implausible reports, blanks RGB, and
                 * polls slowly while ATTN remains high. Run it before the normal
                 * plausibility guard so large key counts enter the slow path.
                 */
                uint32_t bnow = k_uptime_get_32();
                uint32_t bag_idle = (bits >= G4B_BAG_OBVIOUS_KEYS)
                                        ? G4B_BAG_OBVIOUS_IDLE_MS
                                        : (uint32_t)CONFIG_APEX_G4B_BAG_IDLE_MS;
                bool bag_engage, bag_hold;

                if (bits >= (uint32_t)CONFIG_APEX_G4B_BAG_KEYS) {
                    if (!s3_bag_timing) {
                        s3_bag_timing = true;
                        s3_bag_since = bnow;
                    }
                } else if (bits <= 2u) {
                    s3_bag_timing = false;
                }

                bag_engage = s3_bag_timing &&
                             bits >= (uint32_t)CONFIG_APEX_G4B_BAG_KEYS &&
                             (bnow - s3_bag_since) >= bag_idle;
                bag_hold = s3_bag_active && bits > 2u; /* sticky until calm */

                if (!bag_powered && (bag_engage || bag_hold)) {
                    s3_bag_active = true;
                    k_msleep((uint32_t)CONFIG_APEX_G4B_BAG_POLL_MS);
                    continue;
                }
                s3_bag_active = false;
            }
#endif

#if IS_ENABLED(CONFIG_APEX_G4B_KBD_CAPTURE)
            /* Capture the read; never ingest, so this build cannot spam. Record
             * the popcount distribution and keep the first few non-empty reads
             * so the spurious bits are visible. Emit once after a window, then
             * halt - the watchdog feed keeps running from its own work item.
             */
            record_s3.kbd_cap_ok++;
            record_s3.kbd_cap_hist[bits < 11u ? bits : 11u]++;
            if (bits > record_s3.kbd_cap_max) {
                record_s3.kbd_cap_max = (uint16_t)bits;
            }
            if (bits >= 1u && record_s3.kbd_cap_samples < 8u) {
                uint8_t *smp = record_s3.kbd_cap_sample[record_s3.kbd_cap_samples];
                memcpy(smp, s2_rx, 10u); /* 9-byte bitmap + status byte */
                smp[10] = (uint8_t)stats.result;
                record_s3.kbd_cap_samples++;
            }
            if ((k_uptime_get_32() - cap_start_ms) > 12000u) {
                s3_fill_live();
                s2_fill_head(&record_s3.head, G4B_EVIDENCE_VERSION_S3_DIAG,
                             record_s3.head.resetreas, s2_launch_kind);
                g4b_evidence_emit_s3(&record_s3);
                /* Re-emit every window instead of halting, so the /-and-Z
                 * state can be watched continuously - in particular across a
                 * keyboard power-cycle, which is what recalibrates the STM32.
                 */
                record_s3.kbd_cap_reads = 0u;
                record_s3.kbd_cap_ok = 0u;
                record_s3.kbd_cap_max = 0u;
                record_s3.kbd_cap_samples = 0u;
                memset(record_s3.kbd_cap_hist, 0, sizeof(record_s3.kbd_cap_hist));
                cap_start_ms = k_uptime_get_32();
            }
            k_msleep(1);
            continue;
#endif

            /* Plausibility guard: drop a grossly corrupt read outright, WITHOUT
             * advancing any state, so link sync is preserved.
             */
            if (bits > G4B_KEYBOARD_MAX_KEYS) {
                k_msleep(1);
                continue;
            }

#if IS_ENABLED(CONFIG_APEX_G4B_DONGLE_LINK)
            /* Tap every valid absolute bitmap into the 2.4 GHz operational-link
             * report queue. The module dedups internally (independent of ZMK's
             * kscan state, which may be disabled in dongle mode) and queues the
             * all-zero release too. */
            g4b_dongle_link_on_bitmap(s2_rx);
#endif

            /* Deduplicate against the previous absolute bitmap. ATTN is event
             * gated, so a changed bitmap must be accepted on its first successful
             * read; waiting for a duplicate would discard the event.
             */
            if (s3_have_prev &&
                memcmp(s3_prev_bitmap, s2_rx, APEX_G4B_KEY_BITMAP_SIZE) == 0) {
                k_msleep(1);
                continue;
            }
            /* Advance local dedup state only after ZMK accepts the bitmap.
             * -EACCES leaves the kscan driver's previous state untouched. Since
             * each bitmap is absolute, retrying the next report is safe.
             */
            if (apex_g4b_kscan_ingest_bitmap(s3_kscan, s2_rx,
                                             APEX_G4B_KEY_BITMAP_SIZE) == 0) {
#if IS_ENABLED(CONFIG_APEX_G4B_RGB)
                /* Seed the reactive lighting on the RISING edge of each key,
                 * off the bitmap we just accepted. Rising-only, so a held key
                 * flares once rather than pinning its LED. s3_have_prev guards
                 * the first frame, where there is no previous to diff against.
                 * Costs a 9-byte compare and nothing on the bus.
                 */
                if (s3_have_prev) {
                    for (uint32_t byte = 0u; byte < APEX_G4B_KEY_BITMAP_SIZE;
                         byte++) {
                        uint8_t rising = (uint8_t)(s2_rx[byte] &
                                                   ~s3_prev_bitmap[byte]);

                        while (rising != 0u) {
                            uint32_t bit = (uint32_t)__builtin_ctz(rising);

                            g4b_fx_key((uint8_t)(byte * 8u + bit));
                            rising &= (uint8_t)(rising - 1u);
                        }
                    }
                }
#endif
                memcpy(s3_prev_bitmap, s2_rx, APEX_G4B_KEY_BITMAP_SIZE);
                s3_have_prev = true;
                s3_ingest_events++;
                s3_last_activity_ms = k_uptime_get_32();
            }
        } else {
#if IS_ENABLED(CONFIG_APEX_G4B_BAG_GUARD)
            /* ATTN low means every key is released, so whatever held keys down
             * (bag or hand) is gone: clear the bag state and its timer so the
             * RGB and the fast loop come straight back. */
            s3_bag_active = false;
            s3_bag_timing = false;
#endif
            /* Close a pending key/entry ordering race, or cancel slow cadence,
             * before any other housekeeping/configuration frame. */
            if (s3_mode3_normalize_if_needed()) {
                continue;
            }
            /* Service any shell-requested component reset/power change here,
             * before a possible sleep - this is the single-writer-safe point. */
            s3_service_shell_requests();
            /* Only ever from here, where ATTN is known low - see the note on
             * s3_sleep_maybe(). Never returns if it decides to sleep.
             */
            s3_sleep_maybe();
#if IS_ENABLED(CONFIG_APEX_G4B_RGB)
            s3_analog_light();
#endif
            s3_batt_probe();

            /* A keymap control moved a threshold; push it to the scanner.
             * Here, in the ATTN-low branch, because the scanner has nothing
             * queued - a config frame racing a key report is how you lose a
             * keypress.
             */
            if (s3_cfg_dirty != 0u) {
#if CONFIG_APEX_G4B_STM32_IDLE_SCAN_PERIOD_MS > 1
                /* A keymap callback can mark this after the normalization
                 * snapshot above. Re-enter the loop so period 1 is established
                 * before any threshold/config frame reaches the scanner. */
                if (s3_mode3_slow_requested) {
                    continue;
                }
#endif
                s3_cfg_dirty = 0u;
                s3_resend_config();
            }
            s3_bq_probe();

#if CONFIG_APEX_G4B_STM32_STOP1_IDLE_MS > 0
            /* Known-failing scanner-stop reproduction. The bounded link-resync
             * attempt falls back to a scanner power-cycle and disables itself. */
            s3_stop1_maybe();
            if (g4b_pin_read(G4B_PORT0, G4B_P0_ATTN)) {
                /* A key arrived during housekeeping or woke STOP1. Re-enter
                 * the loop immediately so A1 wins over the idle A0 below. */
                continue;
            }
#endif

            /* ATTN is LOW here: the scanner has no event queued. Everything in
             * this branch is therefore safe to issue without racing a key
             * report - which matters for 0xA2 in particular, because its
             * handler can drop the attention line, and a swallowed ATTN is a
             * LOST keypress rather than a late one.
             */
            if (elapsed_us_since(last_a0) >= G4B_S3_A0_GAP_US) {
                /* Occasional 0xA0 keep-alive between key events, exactly as the
                 * diagnostic poll and the stock link do. Counted, because the
                 * A1 path now stamps last_a0 and starving this is the only
                 * failure mode that change can introduce.
                 */
                last_a0 = g4b_cyccnt();
                s3_a0_polls++;
                memset(s2_tx, 0, sizeof(s2_tx));
                s2_tx[0] = 0xA0u;
                memset(s2_rx, 0, sizeof(s2_rx));

                g4b_spim_arm_ready();
                (void)g4b_spim_exchange(s2_tx, s2_rx, &stats);
#if IS_ENABLED(CONFIG_APEX_G4B_AB_ROLLBACK)
                /* Idle-path HEALTHY signal: the keep-alive proves the scanner
                 * answers even with no keys pressed. */
                ab_note_health(stats.result);
#endif
            }
#if IS_ENABLED(CONFIG_APEX_G4B_ANALOG_PROBE)
            else if (s3_analog_sample_wanted() &&
                     elapsed_us_since(last_a2) >=
                     (uint32_t)CONFIG_APEX_G4B_ANALOG_PERIOD_MS * 1000u) {
                /* Deliberately "else if": never two exchanges in one pass, so
                 * the loop's timing stays what the key path was measured with.
                 * The heartbeat wins when both are due - it is the transaction
                 * that reports whether the scanner is still configured.
                 */
                last_a2 = g4b_cyccnt();
                s3_analog_sample();
            }
#endif
        }

#if IS_ENABLED(CONFIG_APEX_G4B_PIN_BEACON)
        /* One step per pass. The loop sleeps 1 ms, so a 17-step cycle is about
         * 59 Hz - fast enough that a meter shows a steady average.
         */
        g4b_pin_beacon_tick();
#endif

#if CONFIG_APEX_G4B_IDLE_AFTER_MS > 0
        /* Idle in System ON rather than spinning at 1 kHz.
         *
         * Zephyr parks the core in WFI whenever every thread is sleeping, so
         * the only thing keeping this chip awake between keystrokes is this
         * loop. Sleeping longer once nothing has happened for a while lets the
         * core actually idle, which is what stock does - and unlike System OFF
         * it costs no reset, keeps the BLE link up, and wakes in microseconds.
         *
         * P0.24 is interrupt-driven, so a key releases the wait immediately;
         * IDLE_POLL_MS is only the no-input housekeeping cadence. Apply that
         * saving in Bluetooth mode only. USB keeps the original 1 ms cadence.
         */
        {
            /* Visible lighting still needs the renderer's 200 Hz cadence. It
             * does not need the surrounding scan loop to wake at 1 kHz, and a
             * selected effect needs no render cadence at all after rail-off. */
            bool idle = (k_uptime_get_32() - s3_last_activity_ms) >=
                        (uint32_t)CONFIG_APEX_G4B_IDLE_AFTER_MS;
            bool fx_live = false;
#if IS_ENABLED(CONFIG_APEX_G4B_RGB)
            /* A visible effect OR an in-flight backlight fade needs its native
             * 200 Hz cadence. Once the idle blanker has cut the LED rail there
             * is nothing left to render, even if a custom effect remains
             * selected in settings. */
            fx_live = !g4b_rgb_is_blanked() &&
                      ((g4b_fx_current() != G4B_FX_ZMK) || g4b_rgb_fading());
#endif
            /* Wait for the next ATTN edge, not a fixed sleep: a key press wakes
             * this immediately (via the GPIOTE ISR) instead of up to the poll
             * interval late - which matters most in the idle case, where that
             * interval may be IDLE_POLL_MS (200 ms), not 1 ms. On timeout it
             * behaves
             * exactly like the k_msleep it replaced, so nothing regresses if no
             * edge comes (the A0 keep-alive + idle work still run per pass). */
            uint32_t wait_ms = 1u;

            if (idle && g4b_mode_get() == G4B_MODE_BT) {
                /* Keep visible custom effects and fades at their actual render
                 * limit, rather than waking at 1 kHz for a 200 Hz renderer.
                 * With the LEDs blanked (or in ZMK passthrough), use the full
                 * housekeeping interval. ATTN releases either wait at once. */
                wait_ms = fx_live ? G4B_FX_PERIOD_MS
                                  : (uint32_t)CONFIG_APEX_G4B_IDLE_POLL_MS;
            }

            /* Slow entry is deliberately the final scanner command before the
             * wait. If ATTN rose anywhere in the idle branch, process A1 first
             * instead of issuing or sleeping after another transaction. */
            if (s3_mode3_enter_if_needed()) {
                continue;
            }
            g4b_attn_wait(wait_ms);
        }
#else
        g4b_attn_wait(1u);
#endif
    }
}
#endif
#endif

#if CONFIG_APEX_G4B_STAGE == 4 || CONFIG_APEX_G4B_STAGE == 6 || CONFIG_APEX_G4B_STAGE == 7
/* Same beacon read as stages 2 and 3, duplicated rather than shared because
 * classify_launch() lives inside the replay block that stage 4 excludes. Stage
 * 4 does not branch on the result - it surveys identically on every launch -
 * but recording which launch produced a record is what lets five copies be told
 * apart on the host.
 */
static uint32_t classify_launch_s4(uint32_t *resetreas)
{
    const volatile struct g4b_boot_beacon *beacon = &g4b_boot_beacon_ram;

    if (beacon->magic != G4B_BOOT_BEACON_MAGIC) {
        *resetreas = 0u;
        return G4B_LAUNCH_CONFIRM_NO_BEACON;
    }

    *resetreas = beacon->resetreas;

    return (*resetreas & G4B_RESETREAS_SREQ) ? G4B_LAUNCH_REPLAY
                                             : G4B_LAUNCH_CONFIRM;
}

/* FLASH SURVEY. Reads only - no NVMC register is touched anywhere in this
 * stage, so nothing here can erase or program regardless of control flow.
 *
 * Flash is memory-mapped on nRF52, so a survey is plain loads. The interesting
 * question is whether the loads succeed: APPROTECT blocks an external debugger
 * but not code running on the device, and if the vendor has set an ACL over the
 * bootloader region a read would bus-fault instead. A fault here is recoverable
 * - it locks up, the watchdog fires, the vendor loader counts it - so the cost
 * of finding out is one launch.
 *
 * Regions surveyed:
 *   0x00000000 - 0x0001C000   stock MBR + S113, 28 pages
 *   0x00067000 - 0x00080000   above the application image, 25 pages
 *
 * One CRC per 4 KiB page keeps the record small enough for the diagnostic UART.
 */
static struct g4b_record_s4 record_s4;

static const uint32_t s4_region_start[G4B_S4_REGIONS] = { 0x00000000u, 0x00067000u };
static const uint32_t s4_region_end[G4B_S4_REGIONS] = { 0x0001C000u, 0x00080000u };

/* CRC-32, same polynomial as the vendor image trailer, so a page can be
 * compared against a host-side computation without a second implementation.
 */
static uint32_t s4_crc32(const volatile uint8_t *data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFu;

    for (uint32_t i = 0u; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }

    return ~crc;
}

static void s4_run_survey(void)
{
    uint32_t slot = 0u;

    record_s4.page_size = G4B_S4_PAGE;

    /* UICR, read only. A UICR write remains forbidden and is not what the
     * lifted restriction covers: it is one-time-programmable and destroying it
     * is unrecoverable.
     */
    record_s4.uicr_approtect = *(volatile uint32_t *)0x10001208u;
    record_s4.uicr_nrffw0 = *(volatile uint32_t *)0x10001014u;
    record_s4.uicr_bootloaderaddr = *(volatile uint32_t *)0x10001018u;

    for (uint32_t r = 0u; r < G4B_S4_REGIONS; r++) {
        record_s4.region_start[r] = s4_region_start[r];
        record_s4.region_end[r] = s4_region_end[r];

        for (uint32_t i = 0u; i < G4B_S4_SAMPLE; i++) {
            record_s4.sample[r][i] =
                *(volatile uint8_t *)(s4_region_start[r] + i);
        }

        for (uint32_t addr = s4_region_start[r]; addr < s4_region_end[r];
             addr += G4B_S4_PAGE) {
            const volatile uint32_t *words = (const volatile uint32_t *)addr;
            bool all_ones = true;
            bool all_zero = true;

            for (uint32_t w = 0u; w < G4B_S4_PAGE / 4u; w++) {
                uint32_t value = words[w];

                if (value != 0xFFFFFFFFu) {
                    all_ones = false;
                }
                if (value != 0u) {
                    all_zero = false;
                }
                if (!all_ones && !all_zero) {
                    break;
                }
            }

            if (slot < G4B_S4_MAX_PAGES) {
                record_s4.page_crc[slot] =
                    s4_crc32((const volatile uint8_t *)addr, G4B_S4_PAGE);
            }
            slot++;

            record_s4.pages_surveyed++;
            if (all_ones) {
                record_s4.pages_erased++;
            }
            if (all_zero) {
                record_s4.pages_zero++;
            }
        }
    }
}
#endif

#if CONFIG_APEX_G4B_STAGE == 5
/* INTERNAL FLASH DUMP. Reads only - no NVMC register is touched here, and
 * the verifier enforces that the payload's only NVMC store is ICACHECNF.
 *
 * Every diagnostic launch emits a complete pass. Each page carries a CRC so
 * the host can reject a damaged copy independently.
 */
static struct g4b_dump_chunk s5_chunk;

static uint32_t s5_crc32(const uint8_t *data, uint32_t length, uint32_t crc)
{
    for (uint32_t i = 0u; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }

    return crc;
}

static void s5_emit_chunk(uint16_t seq, uint32_t addr)
{
    const volatile uint8_t *source = (const volatile uint8_t *)addr;
    uint32_t crc = 0xFFFFFFFFu;

    s5_chunk.magic = G4B_S5_MAGIC;
    s5_chunk.version = G4B_EVIDENCE_VERSION_S5;
    s5_chunk.seq = seq;
    s5_chunk.addr = addr;
    s5_chunk.length = G4B_S5_CHUNK;

    /* Byte at a time through a volatile pointer: memcpy from a peripheral-ish
     * address is free to widen or reorder accesses, and the point of this stage
     * is that the reads happen exactly as written.
     */
    for (uint32_t i = 0u; i < G4B_S5_CHUNK; i++) {
        s5_chunk.data[i] = source[i];
    }

    crc = s5_crc32((const uint8_t *)&s5_chunk.addr, sizeof(s5_chunk.addr), crc);
    crc = s5_crc32((const uint8_t *)&s5_chunk.length, sizeof(s5_chunk.length), crc);
    crc = s5_crc32(s5_chunk.data, G4B_S5_CHUNK, crc);
    s5_chunk.crc32 = ~crc;

    g4b_evidence_dump_chunk(&s5_chunk);
}

static void s5_run_dump(void)
{
    uint16_t seq = 0u;

    g4b_evidence_dump_open();

    for (uint32_t addr = G4B_S5_BOOT_START; addr < G4B_S5_BOOT_END;
         addr += G4B_S5_CHUNK) {
        s5_emit_chunk(seq++, addr);
    }

    /* UICR last. Small, read-only, and worth having in the same backup: it
     * carries APPROTECT and whatever else the vendor programmed there.
     */
    s5_emit_chunk(seq++, G4B_S5_UICR);

    g4b_evidence_dump_close();
}
#endif

#if CONFIG_APEX_G4B_STAGE == 6
#include <zephyr/drivers/flash.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>

/* NVS BRING-UP. The first stage on this device that writes flash.
 *
 * Raw NVS rather than the Zephyr settings layer, deliberately: settings is what
 * BT bonds and ZMK Studio need, but it is software on top of this. Isolating
 * the flash-touching half means a later settings failure cannot be mistaken for
 * a flash failure.
 *
 * The partition is 0x7A000+0x4000, above the CRC-covered image, with a guard
 * page below it and the top two pages left untouched. See g4b6.overlay.
 */
BUILD_ASSERT(FIXED_PARTITION_OFFSET(nvs_storage_partition) == 0x7A000,
             "the NVS partition moved; 0x7A000 is the address G4B-4 measured as erased");
BUILD_ASSERT(FIXED_PARTITION_SIZE(nvs_storage_partition) == 0x4000,
             "the NVS partition size changed");
BUILD_ASSERT(FIXED_PARTITION_OFFSET(nvs_storage_partition) >= 0x67000,
             "the NVS partition is inside the CRC-covered vendor image");

static struct g4b_record_s6 record_s6;
static struct nvs_fs s6_fs;

struct s6_value {
    uint32_t magic;
    uint32_t counter;
};

static void s6_peek(uint8_t *out)
{
    const volatile uint8_t *flash =
        (const volatile uint8_t *)FIXED_PARTITION_OFFSET(nvs_storage_partition);

    for (uint32_t i = 0u; i < G4B_S6_PEEK; i++) {
        out[i] = flash[i];
    }
}

static void s6_run_nvs(void)
{
    struct flash_pages_info info;
    struct s6_value value = {0};
    int rc;

    record_s6.partition_offset = FIXED_PARTITION_OFFSET(nvs_storage_partition);
    record_s6.partition_size = FIXED_PARTITION_SIZE(nvs_storage_partition);
    s6_peek(record_s6.peek_before);

    s6_fs.flash_device = FIXED_PARTITION_DEVICE(nvs_storage_partition);
    if (!device_is_ready(s6_fs.flash_device)) {
        record_s6.mount_rc = -1;
        s6_peek(record_s6.peek_after);
        return;
    }

    s6_fs.offset = FIXED_PARTITION_OFFSET(nvs_storage_partition);
    rc = flash_get_page_info_by_offs(s6_fs.flash_device, s6_fs.offset, &info);
    if (rc != 0) {
        record_s6.mount_rc = rc;
        s6_peek(record_s6.peek_after);
        return;
    }

    s6_fs.sector_size = info.size;
    s6_fs.sector_count = FIXED_PARTITION_SIZE(nvs_storage_partition) / info.size;
    record_s6.sector_size = s6_fs.sector_size;
    record_s6.sector_count = s6_fs.sector_count;

    record_s6.mount_rc = nvs_mount(&s6_fs);
    if (record_s6.mount_rc != 0) {
        s6_peek(record_s6.peek_after);
        return;
    }

    /* Read first, then write incremented. Launch 1 after a flash finds whatever
     * survived; every later launch must find exactly what the previous one
     * wrote, which is the persistence-across-watchdog-reset property that BLE
     * bonds need.
     */
    record_s6.read_rc = nvs_read(&s6_fs, G4B_S6_KEY, &value, sizeof(value));
    if (record_s6.read_rc == (int)sizeof(value)) {
        record_s6.magic_read = value.magic;
        record_s6.counter_read = value.counter;
    }

    value.magic = G4B_S6_MAGIC;
    value.counter = (record_s6.magic_read == G4B_S6_MAGIC)
                        ? record_s6.counter_read + 1u
                        : 1u;
    record_s6.counter_write = value.counter;
    record_s6.write_rc = nvs_write(&s6_fs, G4B_S6_KEY, &value, sizeof(value));

    s6_peek(record_s6.peek_after);
}
#endif

#if CONFIG_APEX_G4B_STAGE == 7
#include <errno.h>

#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>

BUILD_ASSERT(FIXED_PARTITION_OFFSET(nvs_storage_partition) == 0x7A000,
             "the settings partition moved from the address stage 6 proved");

static struct g4b_record_s7 record_s7;
static uint32_t s7_loaded;
static bool s7_found;

/* Called by the settings subsystem for every "apx/..." key it finds - both by
 * ZMK's own settings_load() during init and by our explicit subtree load.
 */
static int s7_set(const char *name, size_t len, settings_read_cb read_cb,
                  void *cb_arg)
{
    if (settings_name_steq(name, "count", NULL)) {
        if (len == sizeof(s7_loaded)) {
            ssize_t got = read_cb(cb_arg, &s7_loaded, sizeof(s7_loaded));

            if (got == (ssize_t)sizeof(s7_loaded)) {
                s7_found = true;
            }
        }
        return 0;
    }

    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(apx, "apx", NULL, s7_set, NULL, NULL);

static void s7_peek(uint8_t *out)
{
    const volatile uint8_t *flash =
        (const volatile uint8_t *)FIXED_PARTITION_OFFSET(nvs_storage_partition);

    for (uint32_t i = 0u; i < G4B_S7_PEEK; i++) {
        out[i] = flash[i];
    }
}

static void s7_run_settings(void)
{
    uint32_t value;

    s7_peek(record_s7.peek_before);

    /* ZMK calls settings_load() during its own init, long before this thread
     * starts, so the handler may already have run. Record that separately from
     * our own load rather than conflating the two.
     */
    record_s7.found_at_thread_start = s7_found ? 1u : 0u;

    record_s7.init_rc = settings_subsys_init();
    record_s7.load_rc = settings_load_subtree("apx");
    record_s7.found_after_load = s7_found ? 1u : 0u;
    record_s7.counter_read = s7_found ? s7_loaded : 0u;

    value = s7_found ? s7_loaded + 1u : 1u;
    record_s7.counter_write = value;
    record_s7.save_rc = settings_save_one("apx/count", &value, sizeof(value));

    s7_peek(record_s7.peek_after);
}
#endif

static void g4b_main(void *a, void *b, void *c)
{
    uint32_t start;
    uint32_t deadline_cycles;
    uint32_t edges = 0u;

    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    g4b_evidence_init();

    /* Before anything of ours runs, so the values are the loader's. */
    s3_twim_snapshot();

#if IS_ENABLED(CONFIG_APEX_G4B_UART_EVIDENCE)
    boot_t_main = k_uptime_get_32();
    boot_resetreas = *(volatile uint32_t *)G4B_POWER_RESETREAS;
#endif

    record.magic = G4B_EVIDENCE_MAGIC;
    record.version = G4B_EVIDENCE_VERSION;
    record.stage = CONFIG_APEX_G4B_STAGE;
    record.launch_ms = k_uptime_get_32();
    record.last_error = G4B_ERR_NONE;

    start = g4b_cyccnt();
    deadline_cycles = (uint32_t)CONFIG_APEX_G4B_DEADLINE_MS * (G4B_CPU_HZ / 1000u);

    record.p0_in_before = g4b_port_in(G4B_PORT0);
    record.p1_in_before = g4b_port_in(G4B_PORT1);
    record.p0_dir_before = g4b_port_dir(G4B_PORT0);
    record.p1_dir_before = g4b_port_dir(G4B_PORT1);

    /* Pull test before the enables rise. Both lines are expected to be
     * floating here, so both should follow the pull.
     */
    pull_test(&record.ready_pullup_before, &record.ready_pulldown_before,
              &record.attn_pullup_before, &record.attn_pulldown_before);

    bringup_pins();

    /* Refuse to proceed if anything we must not drive is an output. This is
     * cheap and catches the class of mistake that would otherwise present as
     * an unexplained silent board.
     */
    if ((g4b_port_dir(G4B_PORT0) & G4B_MUST_BE_INPUT_MASK) != 0u) {
        record.last_error = G4B_ERR_PIN_DIR;
        record.bringup_ok = 0u;
        goto emit;
    }

    g4b_gpiote_ready_clear();

#if CONFIG_APEX_G4B_STM32_RESET_MS > 0
    /* bringup_pins() has driven the enables (P0.02 + P1.05) low. Hold them low
     * long enough for the scanner's supply to actually discharge, so it truly
     * power-cycles and comes up fresh even when only the nRF was reset (a flash's
     * soft reset leaves the scanner running otherwise, which is the "needs a
     * replug" desync). The microsecond pulse bringup_pins does on its own is too
     * short to drain the rail. s2_run_replay() below re-sends the scanner config,
     * so a full reset here loses nothing. */
    k_msleep(CONFIG_APEX_G4B_STM32_RESET_MS);
#endif

    enable_stm32();

    {
        uint32_t enable_at = g4b_cyccnt();
        uint32_t elapsed = 0u;
        bool seen = false;

        /* Watch the ready line. Stage 0 only observes: nothing is clocked and
         * SPIM3 stays disabled regardless of what happens here.
         */
        while (elapsed < G4B_READY_POLL_LIMIT) {
            if (g4b_gpiote_ready_event()) {
                if (!seen) {
                    record.dwt_enable_to_ready = g4b_cyccnt() - enable_at;
                    seen = true;
                }
                edges++;
                g4b_gpiote_ready_clear();
            }
            elapsed = g4b_cyccnt() - enable_at;
        }

        if (!seen) {
            record.last_error = G4B_ERR_NOT_READY;
        }
        record.dwt_observe_window = elapsed;
    }

    record.ready_edges = edges;
    record.bringup_ok = 1u;

    /* Pull test again, now that the STM32 is enabled. A ready line that
     * followed the pull before and ignores it now is the decisive positive
     * result for this stage: something is driving it.
     */
    pull_test(&record.ready_pullup_after, &record.ready_pulldown_after,
              &record.attn_pullup_after, &record.attn_pulldown_after);

    record.p0_in_after = g4b_port_in(G4B_PORT0);
    record.p1_in_after = g4b_port_in(G4B_PORT1);
    record.p0_dir_after = g4b_port_dir(G4B_PORT0);
    record.p1_dir_after = g4b_port_dir(G4B_PORT1);
    record.gpiote_config0 = g4b_gpiote_config0();
    record.pin_cnf_ready = g4b_pin_cnf_read(G4B_PORT0, G4B_P0_READY);
    record.pin_cnf_attn = g4b_pin_cnf_read(G4B_PORT0, G4B_P0_ATTN);
    record.pin_cnf_miso = g4b_pin_cnf_read(G4B_PORT0, G4B_P0_MISO);

#if CONFIG_APEX_G4B_STAGE == 1
    record_s1.magic = G4B_EVIDENCE_MAGIC;
    record_s1.version = G4B_EVIDENCE_VERSION_S1;
    record_s1.stage = CONFIG_APEX_G4B_STAGE;
    record_s1.launch_ms = record.launch_ms;
    record_s1.bringup_ok = record.bringup_ok;
    record_s1.dwt_enable_to_ready = record.dwt_enable_to_ready;
    record_s1.last_error = record.last_error;

    g4b_spim_enable();

    for (int i = 0; i < G4B_S1_EXCHANGES; i++) {
        struct g4b_exchange_stats stats = {0};

        memset(s1_tx, 0, sizeof(s1_tx));
        s1_tx[0] = s1_opcodes[i];
        record_s1.opcode[i] = s1_opcodes[i];

        g4b_spim_arm_ready();
        (void)g4b_spim_exchange(s1_tx, s1_rx[i], &stats);

        memcpy(record_s1.exchange[i].rx, s1_rx[i], G4B_SPIM_FRAME);
        record_s1.exchange[i].tx_amount = stats.tx_amount;
        record_s1.exchange[i].rx_amount = stats.rx_amount;
        record_s1.exchange[i].events_end = stats.events_end;
        record_s1.exchange[i].result = stats.result;
        record_s1.exchange[i].wait_tx_us = stats.wait_tx_us;
        record_s1.exchange[i].wait_rx_us = stats.wait_rx_us;

        if ((g4b_cyccnt() - start) >= deadline_cycles) {
            record_s1.last_error = G4B_ERR_DEADLINE;
            break;
        }
    }

    g4b_spim_disable();
#endif

#if CONFIG_APEX_G4B_STAGE == 2 || CONFIG_APEX_G4B_STAGE == 3
    {
        uint32_t resetreas = 0u;
        uint32_t kind = classify_launch(&resetreas);

        g4b_spim_enable();

#if CONFIG_APEX_G4B_STAGE == 3
        /* Stage 3 replays on every launch because the STM32 loses configuration
         * across a Nordic reset. The replay is byte-exact, does not reach
         * non-volatile scanner storage, and matches stock boot behaviour.
         */
        (void)kind;
#if IS_ENABLED(CONFIG_ZMK_USB) && IS_ENABLED(CONFIG_APEX_G4B_USB_KICK)
        record_s3.usb_kick = usb_pwrrdy_kick();
#endif
        /* Give replay an independent three-second budget. Its 59 frames take
         * about 472 ms at the required 8 ms pacing, and earlier initialization
         * must not consume this deadline.
         */
#if IS_ENABLED(CONFIG_APEX_G4B_UART_EVIDENCE)
        boot_t_bringup = k_uptime_get_32();
#endif
        /* Bracket the paced replay: any external-SPI-NOR NVS write/erase (on the
         * system workqueue) blocks until end, so nothing lands in a frame gap and
         * desyncs the STM32. No-op unless CONFIG_APEX_G4B_SPINOR_FLASHDEV. */
        g4b_extbus_replay_begin();
        s2_run_replay(g4b_cyccnt(), G4B_S2_REPLAY_BUDGET_CYCLES);
        s2_post_replay_probe();
        g4b_extbus_replay_end();
#if IS_ENABLED(CONFIG_APEX_G4B_UART_EVIDENCE)
        boot_t_replay = k_uptime_get_32();
#endif
#if IS_ENABLED(CONFIG_APEX_G4B_BOOT_DIAG)
        /* 3.5 s of diagnostics between a configured scanner and a working
         * keyboard. Off by default - see CONFIG_APEX_G4B_BOOT_DIAG.
         */
        s3_chord_window();
        s3_run_poll(start, deadline_cycles);
#endif
#else
        if (kind == G4B_LAUNCH_REPLAY) {
            g4b_extbus_replay_begin();
            s2_run_replay(start, deadline_cycles);
            s2_post_replay_probe();
            g4b_extbus_replay_end();
        } else {
            s2_run_confirm(start, deadline_cycles);
        }
#endif

        g4b_spim_disable();

        /* Filled after the run so last_error reflects what actually happened. */
#if CONFIG_APEX_G4B_STAGE == 3
        s2_fill_head(&record_s3.head, G4B_EVIDENCE_VERSION_S3_DIAG, resetreas, kind);
        /* record_s2r is the replay's working buffer in both stages; stage 3
         * emits only the summary from it, not the 59-frame detail.
         */
#if IS_ENABLED(CONFIG_APEX_G4B_KSCAN_INGEST)
        record_s3.kscan_ready =
            (s3_kscan != NULL && device_is_ready(s3_kscan)) ? 1u : 0u;
        record_s3.ble_profile_connected =
            zmk_ble_active_profile_is_connected() ? 1u : 0u;
        record_s3.endpoint_transport =
            (uint32_t)zmk_endpoint_get_selected().transport;
#endif
        /* Raw, and independent of any driver: if VBUSDETECT is clear the chip
         * does not see bus power and nothing in software can enumerate.
         */
        record_s3.wdt_feeds = g4b_wdt.feeds;
        record_s3.wdt_uptime_ms = k_uptime_get_32();
        record_s3.wdt_crv = g4b_wdt.crv;
        record_s3.wdt_rren = g4b_wdt.rren;
        record_s3.wdt_config = g4b_wdt.config;
        record_s3.wdt_stopped = g4b_wdt.stopped_reason;

        record_s3.usb_regstatus = *(volatile uint32_t *)G4B_USBREGSTATUS;
        record_s3.usbd_preinit_enable = usbd_preinit_enable;
#if IS_ENABLED(CONFIG_ZMK_USB)
        record_s3.usb_status = (uint32_t)zmk_usb_get_status();
        record_s3.usb_conn_state = (uint32_t)zmk_usb_get_conn_state();
#endif
        record_s3.usbd_enable = *(volatile uint32_t *)G4B_USBD_ENABLE;
        record_s3.usbd_pullup = *(volatile uint32_t *)G4B_USBD_USBPULLUP;
        record_s3.usbd_eventcause = *(volatile uint32_t *)G4B_USBD_EVENTCAUSE;
        record_s3.hfclkstat = *(volatile uint32_t *)G4B_CLOCK_HFCLKSTAT;
        record_s3.frames_run = record_s2r.frames_run;
        record_s3.frames_matched = record_s2r.frames_matched;
        record_s3.first_mismatch_index = record_s2r.first_mismatch_index;
        record_s3.first_mismatch_offset = record_s2r.first_mismatch_offset;
        record_s3.post_replay_a0 = record_s2r.post_replay_a0;
#else
        if (kind == G4B_LAUNCH_REPLAY) {
            s2_fill_head(&record_s2r.head, G4B_EVIDENCE_VERSION_S2_REPLAY,
                         resetreas, kind);
        } else {
            s2_fill_head(&record_s2c.head, G4B_EVIDENCE_VERSION_S2_CONFIRM,
                         resetreas, kind);
        }
#endif
        s2_launch_kind = kind;
    }
#endif

#if CONFIG_APEX_G4B_STAGE == 7
    {
        uint32_t resetreas = 0u;
        uint32_t kind = classify_launch_s4(&resetreas);

        s7_run_settings();

        record_s7.head.magic = G4B_EVIDENCE_MAGIC;
        record_s7.head.version = G4B_EVIDENCE_VERSION_S7;
        record_s7.head.stage = CONFIG_APEX_G4B_STAGE;
        record_s7.head.launch_ms = record.launch_ms;
        record_s7.head.bringup_ok = record.bringup_ok;
        record_s7.head.dwt_enable_to_ready = record.dwt_enable_to_ready;
        record_s7.head.resetreas = resetreas;
        record_s7.head.launch_kind = kind;
    }
#endif

#if CONFIG_APEX_G4B_STAGE == 6
    {
        uint32_t resetreas = 0u;
        uint32_t kind = classify_launch_s4(&resetreas);

        s6_run_nvs();

        record_s6.head.magic = G4B_EVIDENCE_MAGIC;
        record_s6.head.version = G4B_EVIDENCE_VERSION_S6;
        record_s6.head.stage = CONFIG_APEX_G4B_STAGE;
        record_s6.head.launch_ms = record.launch_ms;
        record_s6.head.bringup_ok = record.bringup_ok;
        record_s6.head.dwt_enable_to_ready = record.dwt_enable_to_ready;
        record_s6.head.resetreas = resetreas;
        record_s6.head.launch_kind = kind;
    }
#endif

#if CONFIG_APEX_G4B_STAGE == 5
    /* Emitted before the stage-0 record so that if the launch is cut short the
     * dump is what survived, not the pin snapshot. The stage-0 record still
     * follows at the normal 19200.
     */
    s5_run_dump();
#endif

#if CONFIG_APEX_G4B_STAGE == 4
    /* Nothing STM32-related runs in stage 4 - SPIM3 is never enabled - but the
     * common bring-up above still ran, so the stage-0 pin record stays
     * comparable with every other stage.
     */
    {
        uint32_t resetreas = 0u;
        uint32_t kind = classify_launch_s4(&resetreas);

        s4_run_survey();

        record_s4.head.magic = G4B_EVIDENCE_MAGIC;
        record_s4.head.version = G4B_EVIDENCE_VERSION_S4;
        record_s4.head.stage = CONFIG_APEX_G4B_STAGE;
        record_s4.head.launch_ms = record.launch_ms;
        record_s4.head.bringup_ok = record.bringup_ok;
        record_s4.head.dwt_enable_to_ready = record.dwt_enable_to_ready;
        record_s4.head.resetreas = resetreas;
        record_s4.head.launch_kind = kind;
    }
#endif

#if CONFIG_APEX_G4B_STAGE != 3
    if ((g4b_cyccnt() - start) >= deadline_cycles) {
        record.last_error = G4B_ERR_DEADLINE;
    }
#else
    /* Stage 3 normally consumes its poll deadline. Replay reports its own
     * overrun, and poll_window_us records the time available to the poll. */
#endif

#if !G4B_KEYBOARD_BUILD
    shutdown_pins();
#else
    /* Keep the STM32 enabled through diagnostic emission and entry into the key
     * loop. Powering it down here would discard the volatile setup replayed at
     * boot. */
#endif

emit:
    g4b_evidence_emit(&record);
#if CONFIG_APEX_G4B_STAGE == 1
    g4b_evidence_emit_s1(&record_s1);
#endif
#if CONFIG_APEX_G4B_STAGE == 3
    /* The goto above can reach here before the head was filled, so fill it for
     * the abort case too. Re-filling is harmless and keeps last_error current.
     */
    s3_fill_live();
#if IS_ENABLED(CONFIG_APEX_G4B_PIN_SURVEY)
    /* Candidates: every pin this payload never touches and the vendor loader
     * does not drive, measured from its P0.DIR/P1.DIR at boot (0x0C000855 and
     * 0x00000321). P0.18 is nRESET and g4b_pin_survey masks it out anyway.
     */
    {
        uint32_t p0u = 0u, p1u = 0u, p0d = 0u, p1d = 0u;

        g4b_pin_survey(G4B_SURVEY_P0_MASK, G4B_SURVEY_P1_MASK,
                       &p0u, &p1u, &p0d, &p1d);

        record_s3.kbd_cap_reads = p0u;
        record_s3.kbd_cap_ok = p0d;
        record_s3.kbd_cap_hist[0] = (uint16_t)(p1u & 0xFFFFu);
        record_s3.kbd_cap_hist[1] = (uint16_t)(p1u >> 16);
        record_s3.kbd_cap_hist[2] = (uint16_t)(p1d & 0xFFFFu);
        record_s3.kbd_cap_hist[3] = (uint16_t)(p1d >> 16);
    }
    s2_fill_head(&record_s3.head, G4B_EVIDENCE_VERSION_S3_PROBE,
                 record_s3.head.resetreas, s2_launch_kind);
#else
    s2_fill_head(&record_s3.head, G4B_EVIDENCE_VERSION_S3_DIAG,
                 record_s3.head.resetreas, s2_launch_kind);
#endif
    g4b_evidence_emit_s3(&record_s3);
#elif CONFIG_APEX_G4B_STAGE == 7
    g4b_evidence_emit_s7(&record_s7);
#elif CONFIG_APEX_G4B_STAGE == 6
    g4b_evidence_emit_s6(&record_s6);
#elif CONFIG_APEX_G4B_STAGE == 4
    g4b_evidence_emit_s4(&record_s4);
#elif CONFIG_APEX_G4B_STAGE == 2
    /* The goto above can reach here before the head was filled, so fill it for
     * the abort case too. Re-filling is harmless and keeps last_error current.
     */
    if (s2_launch_kind == G4B_LAUNCH_REPLAY) {
        s2_fill_head(&record_s2r.head, G4B_EVIDENCE_VERSION_S2_REPLAY,
                     record_s2r.head.resetreas, s2_launch_kind);
        g4b_evidence_emit_s2_replay(&record_s2r);
    } else {
        s2_fill_head(&record_s2c.head, G4B_EVIDENCE_VERSION_S2_CONFIRM,
                     record_s2c.head.resetreas, s2_launch_kind);
        g4b_evidence_emit_s2_confirm(&record_s2c);
    }
#endif

#if G4B_KEYBOARD_BUILD
    /* Enter the permanent scan loop only after pin checks and scanner setup
     * succeed. A failed bring-up leaves the scanner off and lets the watchdog
     * hand control back to the bootloader. */
    /* An experimental dongle build gives its radio thread the CPU in the dongle
     * position. Normal releases compile that transport out and keep scanning.
     * The operational-link build (DONGLE_LINK) is the exception: the scanner
     * must keep running so its absolute bitmaps can be queued for the radio, so
     * s3_run_keyboard() runs even in the dongle position there. The link runs on
     * its own thread (g4b_dongle_link_tid). */
    if (record.bringup_ok &&
        (!IS_ENABLED(CONFIG_APEX_G4B_DONGLE_RADIO) ||
         g4b_mode_get() != G4B_MODE_DONGLE ||
         IS_ENABLED(CONFIG_APEX_G4B_DONGLE_LINK))) {
        s3_run_keyboard();
    }
#endif

    /* A diagnostic build, failed bring-up, or experimental dongle handoff can
     * end here. The loop yields while still allowing staged RGB frames to flush. */
    for (;;) {
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
        /* g4b_rgb_flush() is the only transmit site for staged RGB frames. */
        g4b_rgb_flush();
        k_msleep(20);
#else
        k_sleep(K_FOREVER);
#endif
    }
}

/* 2048 bytes, not 1024: CONFIG_ARM_MPU, CONFIG_HW_STACK_PROTECTION and
 * CONFIG_ASSERT are all n, so an overflow would silently corrupt a neighbouring
 * stack and produce a plausible-looking record rather than a crash. All buffers
 * are static, not stack.
 *
 * Preemptible priority 10 rather than the system workqueue, so the busy-waits
 * here cannot stall ZMK's own processing.
 */
K_THREAD_DEFINE(g4b_thread, 2048, g4b_main, NULL, NULL, NULL,
                K_PRIO_PREEMPT(10), 0, CONFIG_APEX_G4B_THREAD_DELAY_MS);
