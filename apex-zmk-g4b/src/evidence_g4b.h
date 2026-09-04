/* SPDX-License-Identifier: MIT
 *
 * Binary APXG diagnostic records emitted over the direct UARTE0 backend.
 * Record versions keep older captures decodable. Release builds retain these
 * layouts but disable UART and USB evidence output.
 */

#pragma once

#include <stdint.h>

#define G4B_EVIDENCE_MAGIC 0x47585041u /* "APXG" little-endian */
#define G4B_EVIDENCE_VERSION 1u

enum g4b_error {
    G4B_ERR_NONE = 0,
    G4B_ERR_PIN_DIR = 1,   /* protected input line was configured as output */
    G4B_ERR_DEADLINE = 2,  /* global deadline hit */
    G4B_ERR_NOT_READY = 3, /* ready line never asserted */
    G4B_ERR_REPLAY = 4,    /* replay aborted: frame mismatch or SPIM error */
};

/* Fixed layout, little-endian, emitted as raw bytes. Kept small so a single
 * UARTE transfer covers it and so a truncated record is obvious.
 */
struct g4b_record {
    uint32_t magic;
    uint16_t version;
    uint16_t stage;

    uint32_t launch_ms;      /* uptime when the stage thread started */
    uint32_t last_error;

    /* Pin state, sampled before and after the enable transition. */
    uint32_t p0_in_before;
    uint32_t p1_in_before;
    uint32_t p0_dir_before;
    uint32_t p1_dir_before;
    uint32_t p0_in_after;
    uint32_t p1_in_after;
    uint32_t p0_dir_after;
    uint32_t p1_dir_after;

    /* Pull test: does the line follow an internal pull, or is it driven?
     * A line that follows the pull is floating - nothing is driving it.
     */
    uint8_t ready_pullup_before;
    uint8_t ready_pulldown_before;
    uint8_t attn_pullup_before;
    uint8_t attn_pulldown_before;
    uint8_t ready_pullup_after;
    uint8_t ready_pulldown_after;
    uint8_t attn_pullup_after;
    uint8_t attn_pulldown_after;

    /* Timing, in DWT CYCCNT ticks at 64 MHz. k_cycle_get_32() is backed by a
     * 32768 Hz RTC here (30.5 us granularity), far too coarse for a link that
     * clocks a 64-byte frame in 128 us.
     */
    uint32_t dwt_enable_to_ready;
    uint32_t dwt_observe_window;
    uint32_t ready_edges;
    uint32_t bringup_ok;

    uint32_t gpiote_config0;
    uint32_t pin_cnf_ready;
    uint32_t pin_cnf_attn;
    uint32_t pin_cnf_miso;
} __packed;

/* Stage 1 returns full frames, so it gets its own layout under version 2.
 * The decoder dispatches on the version field.
 */
#define G4B_EVIDENCE_VERSION_S1 2u
#define G4B_S1_EXCHANGES 2

struct g4b_exchange_record {
    uint8_t rx[64];
    uint32_t tx_amount;
    uint32_t rx_amount;
    uint32_t events_end;
    uint32_t result;
    uint32_t wait_tx_us;
    uint32_t wait_rx_us;
} __packed;

struct g4b_record_s1 {
    uint32_t magic;
    uint16_t version;
    uint16_t stage;

    uint32_t launch_ms;
    uint32_t last_error;
    uint32_t bringup_ok;
    uint32_t dwt_enable_to_ready;

    uint8_t opcode[G4B_S1_EXCHANGES];
    uint8_t pad[2];

    struct g4b_exchange_record exchange[G4B_S1_EXCHANGES];
} __packed;

/* Stage 2 uses separate replay and confirmation records so confirmation
 * captures do not include an unused frame-by-frame replay payload.
 */

#define G4B_EVIDENCE_VERSION_S2_CONFIRM 4u

/* Version 3 was the first replay record. Version 5 appends the post-replay
 * 0xA0 probe so older captures remain unambiguous and decodable.
 */
#define G4B_EVIDENCE_VERSION_S2_REPLAY 5u

#define G4B_S2_PREFIX_FRAMES 59
#define G4B_S2_VERBATIM_FRAMES 4
#define G4B_S2_CONFIRM_EXCHANGES 2
#define G4B_S2_NO_INDEX 0xFFFFFFFFu

/* The wrapper stores this beacon in .noinit RAM immediately before handing
 * control to the payload. Only magic and resetreas are common to every wrapper
 * build and may be consumed here.
 */
#define G4B_BOOT_BEACON_MAGIC 0x42585041u /* "APXB" little-endian */
#define G4B_RESETREAS_SREQ 0x00000004u

struct g4b_boot_beacon {
    uint32_t magic;
    uint32_t resetreas;
} __packed;

enum g4b_launch_kind {
    /* SREQ set: this boot followed the vendor updater's reset, so it is the
     * first launch after a flash and the STM32 is cold.
     */
    G4B_LAUNCH_REPLAY = 0,
    /* DOG only: a watchdog relaunch. Read-only. */
    G4B_LAUNCH_CONFIRM = 1,
    /* Missing beacon provenance; use the read-only confirmation path. */
    G4B_LAUNCH_CONFIRM_NO_BEACON = 2,
};

enum g4b_frame_result {
    G4B_FRAME_NOT_RUN = 0,
    G4B_FRAME_MATCH = 1,
    G4B_FRAME_MISMATCH = 2,
    G4B_FRAME_SPIM_ERROR = 3,
};

/* Common to both stage-2 layouts, so the host can read the header before it
 * knows which record it has.
 */
struct g4b_s2_head {
    uint32_t magic;
    uint16_t version;
    uint16_t stage;

    uint32_t launch_ms;
    uint32_t last_error;
    uint32_t bringup_ok;
    uint32_t dwt_enable_to_ready;

    uint32_t resetreas;   /* raw, so the branch decision can be audited */
    uint32_t launch_kind;
} __packed;

struct g4b_record_s2_replay {
    struct g4b_s2_head head;

    uint32_t frames_run;
    uint32_t frames_matched;
    uint32_t first_mismatch_index;  /* G4B_S2_NO_INDEX if none */
    uint32_t first_mismatch_offset; /* G4B_S2_NO_INDEX if none */

    uint8_t frame_result[G4B_S2_PREFIX_FRAMES];
    uint8_t pad[1];

    /* Only the frame that failed, in full, both sides. */
    uint8_t mismatch_expected[64];
    uint8_t mismatch_actual[64];

    /* Frames 0, 29, 57, 58: the oracle and the three scanner-state frames. */
    uint8_t verbatim[G4B_S2_VERBATIM_FRAMES][64];

    /* Read-only 0xA0 probe sent immediately after replay. It distinguishes a
     * configuration lost across reset from a misidentified status field.
     */
    struct g4b_exchange_record post_replay_a0;
} __packed;

struct g4b_record_s2_confirm {
    struct g4b_s2_head head;

    uint8_t opcode[G4B_S2_CONFIRM_EXCHANGES];
    uint8_t pad[2];

    struct g4b_exchange_record exchange[G4B_S2_CONFIRM_EXCHANGES];
} __packed;

/* Stage 3: live-key capture after replay. */

#define G4B_EVIDENCE_VERSION_S3 6u

/* Version 11 records the kscan ingest result, distinguishing a disabled kscan
 * device (-EACCES) from an event accepted and later dropped by ZMK. Version 6
 * remains supported for older captures.
 */
#define G4B_EVIDENCE_VERSION_S3_DIAG 37u

/* Version 38 reuses the 1280-byte diagnostic layout. Pin-survey builds
 * reinterpret the unused kbd_cap_* area as follows:
 *
 *   kbd_cap_reads    -> P0 levels with every candidate pulled UP
 *   kbd_cap_ok       -> P0 levels with every candidate pulled DOWN
 *   kbd_cap_hist[0]  -> P1 pulled up, low half   (P1 has only 10 pins)
 *   kbd_cap_hist[1]  -> P1 pulled up, high half
 *   kbd_cap_hist[2]  -> P1 pulled down, low half
 *   kbd_cap_hist[3]  -> P1 pulled down, high half
 * The candidate masks are compile-time constants, so the decoder carries its
 * own copy rather than spending record space on them.
 */
#define G4B_EVIDENCE_VERSION_S3_PROBE 38u

/* Version 39 uses the same record size. Analog builds reinterpret kbd_cap_*:
 *
 *   kbd_cap_reads      -> 0xA2 frames that completed
 *   kbd_cap_ok         -> samples rejected as out of the scanner's valid window
 *   kbd_cap_hist[k*3]  -> min, max, last for key k, in the order W, A, S, D
 *   kbd_cap_samples    -> how many ring slots were filled
 *   kbd_cap_sample     -> the ring itself, 4 keys x 11 uint16 little-endian,
 *                         packed across the [8][11] byte array
 *
 * Three reserved words hold gamepad counters without moving later fields.
 */
#define G4B_EVIDENCE_VERSION_S3_ANALOG 39u

/* Version 12 adds USB state. POWER->USBREGSTATUS bit 0 (VBUSDETECT) distinguishes
 * missing bus power from a USB stack or signalling failure.
 */
#define G4B_USBREGSTATUS 0x40000438u

/* Version 13 adds attach-state registers. USBPULLUP=0 indicates that attach
 * was not signalled; USBPULLUP=1 indicates that the device signalled attach
 * but did not receive host traffic. ENABLE, EVENTCAUSE, and HFCLKSTAT provide
 * the surrounding controller and clock state.
 */
#define G4B_USBD_ENABLE      0x40027500u
#define G4B_USBD_USBPULLUP   0x40027504u
#define G4B_USBD_EVENTCAUSE  0x40027400u
#define G4B_CLOCK_HFCLKSTAT  0x4000040Cu
#define G4B_USBD_INTEN       0x40027300u
#define G4B_USBD_LOWPOWER    0x4002752Cu
#define G4B_USBD_EVENTS_USBRESET 0x40027100u
#define G4B_USBD_EVENTS_USBEVENT 0x40027158u
#define G4B_USBD_EVENTS_EP0SETUP 0x4002715Cu

/* Escape-hatch chord. ESC is scan bit 0; check_keymap_chain.py verifies it
 * against the scan map, transform, and keymap. 0xA0 byte 2 is a matrix-wide
 * travel scalar: bit 7 reports any key down and bits 0..5 carry magnitude.
 */
#define G4B_CHORD_BYTE 0u
#define G4B_CHORD_MASK 0x01u
#define G4B_CHORD_WINDOW_MS 2500u
/* Local copy: evidence_g4b.h must not depend on kscan_g4b.h, and the
 * BUILD_ASSERT in link_g4b.c pins it to APEX_G4B_KEY_BITMAP_SIZE.
 */
#define G4B_CHORD_BITMAP_BYTES 9u

/* Watchdog stop reasons. */
#define G4B_WDT_STOP_NONE   0u
#define G4B_WDT_STOP_BUDGET 1u
#define G4B_WDT_STOP_CONFIG 2u
#define G4B_WDT_STOP_DONGLE 3u /* legacy diagnostic: dongle position */
#define G4B_WDT_STOP_HATCH  4u /* escape hatch went stale - refuse to feed */
#define G4B_WDT_STOP_STRAP  5u /* P1.07 boot strap pulled low */
#define G4B_WDT_STOP_SWREQ  6u /* legacy diagnostic: GPREGRET2 request */
#define G4B_WDT_STOP_KEYBOARD 7u /* scanner loop stopped making progress */

struct g4b_wdt_state {
	uint32_t feeds;
	uint32_t last_uptime_ms;
	uint32_t budget_ms;
	uint32_t crv;
	uint32_t rren;
	uint32_t config;
	uint32_t runstatus;
	uint32_t stopped_reason;
};

extern struct g4b_wdt_state g4b_wdt;
#define G4B_USBD_EPINEN      0x40027510u
#define G4B_USBD_EPOUTEN     0x40027514u
#define G4B_USBD_FRAMECNTR   0x40027520u
#define G4B_USBD_DPDMVALUE   0x40027508u
#define G4B_CLOCK_HFCLKSTART 0x40000000u
#define G4B_CLOCK_HFCLKRUN   0x40000408u

/* POWER and CLOCK share base 0x40000000 on nRF52. */
#define G4B_POWER_RESETREAS  0x40000400u
#define G4B_CLOCK_LFCLKSTAT  0x40000418u
#define G4B_CLOCK_LFCLKSRC   0x40000518u
#define G4B_POWER_DCDCEN     0x40000578u
#define G4B_POWER_DCDCEN0    0x40000580u
#define G4B_PWR_SNAP_WORDS 7u

/* Version 20 stores four snapshots of the USB controller, clock, and power
 * state.
 */
#define G4B_USB_SNAP_WORDS 12u
#define G4B_USB_SNAPS 4u
#define G4B_USBD_EVENTCAUSE_READY BIT(11)

/* The two hidden revision words nrf52_errata_187() and friends read to decide
 * whether a workaround applies. For nRF52833 the 187 workaround is applied only
 * when the first reads 0x0D; any other value falls through every guard and the
 * function returns false, silently skipping the workaround for an erratum whose
 * title is "USB cannot be enabled".
 */
#define G4B_FICR_ERRATA_VAR1 0x10000130u
#define G4B_FICR_ERRATA_VAR2 0x10000134u

/* Outcome of the USBPWRRDY re-post. See usb_pwrrdy_kick() in link_g4b.c. */
#define G4B_USB_KICK_APPLIED       0u
#define G4B_USB_KICK_NOT_NEEDED    1u  /* pullup already set - driver got there */
#define G4B_USB_KICK_USBD_OFF      2u  /* USBD never enabled; a different fault */
#define G4B_USB_KICK_NO_OUTPUTRDY  3u  /* regulator not ready; READY is legitimately pending */
#define G4B_USB_KICK_INT_DISABLED  4u  /* nrfx never armed USBPWRRDY; re-posting would be inert */
#define G4B_S3_EVENTS 8
#define G4B_S3_EVENT_RX 32

struct g4b_key_event {
    uint32_t t_us;   /* since the poll window opened */
    uint32_t result; /* enum g4b_spim_result */
    /* The bitmap is rx[0..8] and the status byte rx[9]. Retain 32 bytes so
     * alternate frame layouts can be decoded.
     */
    uint8_t rx[G4B_S3_EVENT_RX];
} __packed;

struct g4b_record_s3 {
    struct g4b_s2_head head;

    /* Compact replay result and first mismatch location. */
    uint32_t frames_run;
    uint32_t frames_matched;
    uint32_t first_mismatch_index;
    uint32_t first_mismatch_offset;

    /* A configured reply has rx[1] == 1. */
    struct g4b_exchange_record post_replay_a0;

    uint32_t poll_window_us;
    uint32_t polls_a0;
    uint32_t polls_a1;
    uint32_t attn_high_seen;
    uint32_t events_total;    /* may exceed G4B_S3_EVENTS */
    uint32_t a0_lost_config;  /* A0 polls that did not report rx[1] == 1 */

    struct g4b_key_event event[G4B_S3_EVENTS];

    /* Version 11 only. Zero in a version-6 record. */
    int32_t ingest_last_rc;   /* last apex_g4b_kscan_ingest_bitmap() return */
    uint32_t ingest_calls;
    uint32_t ingest_ok;
    uint32_t ble_profile_connected; /* zmk_ble_active_profile_is_connected() */
    uint32_t endpoint_transport;    /* zmk_endpoint_get_selected().transport */
    uint32_t kscan_ready;           /* device_is_ready() on the kscan node */

    /* Version 12 only. Zero in a version-11 record. */
    uint32_t usb_regstatus;   /* POWER->USBREGSTATUS: bit0 VBUSDETECT, bit1 OUTPUTRDY */
    uint32_t usb_status;      /* zmk_usb_get_status()  - enum usb_dc_status_code */
    uint32_t usb_conn_state;  /* zmk_usb_get_conn_state() - NONE/POWERED/HID */

    /* Version 13 only. Zero in a version-12 record. */
    uint32_t usbd_enable;      /* USBD->ENABLE */
    uint32_t usbd_pullup;      /* USBD->USBPULLUP: 1 = attach signalled */
    uint32_t usbd_eventcause;  /* USBD->EVENTCAUSE */
    uint32_t hfclkstat;        /* CLOCK->HFCLKSTAT: bit0 SRC(1=XTAL) bit16 STATE */

    /* Version 14 only. G4B_USB_KICK_*, sampled at stage entry - the registers
     * above are sampled at record time, so they show the state *after* it.
     */
    uint32_t usb_kick;

    /* Version 15 samples USBPWRRDY delivery around a synthetic event. A latched
     * event with no ISR consumption indicates a masked interrupt; a cleared
     * event indicates handler activity.
     */
    uint32_t usb_evt_before;   /* EVENTS_USBPWRRDY just before the write */
    uint32_t usb_evt_after;    /* EVENTS_USBPWRRDY ~20 ms after */
    uint32_t usb_power_inten;  /* POWER->INTENSET, whole mask */
    uint32_t usb_nvic_enabled; /* NVIC enable bit for POWER_CLOCK_IRQn */
    uint32_t usb_pullup_after; /* USBD->USBPULLUP ~20 ms after the re-post */

    /* Version 16 records a second usb_enable() result and the resulting
     * controller state. -EALREADY means the stack already considered USB
     * enabled.
     */
    int32_t usb_enable_rc;
    uint32_t usbd_inten;         /* USBD->INTEN - nonzero implies start() ran */
    uint32_t usb_pullup_final;   /* USBD->USBPULLUP after the second enable */

    /* Version 17 records USBD->ENABLE before application USB initialization. */
    uint32_t usbd_preinit_enable;

    /* Version 18 records a direct USBPULLUP write while endpoints are enabled. */
    uint32_t usbd_lowpower;         /* USBD->LOWPOWER before the forced write */
    uint32_t usb_pullup_forced;     /* read back immediately after writing 1 */
    uint32_t usb_pullup_forced_1s;  /* read again 1 s later - did it stick? */
    uint32_t usb_status_forced;     /* zmk_usb_get_status() 1 s after the write */

    /* Version 19 leaves USBD low-power mode before reading and writing
     * USBPULLUP.
     */
    uint32_t usbd_lowpower_after;      /* LOWPOWER read back after writing 0 */
    uint32_t usb_pullup_normal;        /* USBPULLUP once out of low power */
    uint32_t usb_pullup_forced_normal; /* after forcing it in normal mode */
    uint32_t usb_status_normal;        /* zmk_usb_get_status() 2 s later */

    /* Version 20 snapshot order: stage entry; HFXO on and LOWPOWER off;
     * USBPULLUP set. The register set is defined by G4B_USB_SNAP_WORDS.
     */
    uint32_t usb_snap[G4B_USB_SNAPS][G4B_USB_SNAP_WORDS];

    /* Version 21 adds a snapshot after cycling USBD->ENABLE and waiting for
     * EVENTCAUSE.READY.
     */
    uint32_t usbd_reenable_ready;  /* EVENTCAUSE.READY seen after re-enable */
    uint32_t usbd_reenable_spins;  /* iterations spent waiting for it */
    uint32_t usbd_reenable_pullup; /* USBPULLUP read back after the re-enable */

    /* Version 22 records FICR revision values, Nordic errata predicates, and a
     * forced errata-171/187 retry.
     */
    uint32_t ficr_errata_var1;
    uint32_t ficr_errata_var2;
    uint32_t errata_187_flag;      /* what nrf52_errata_187() returns here */
    uint32_t errata_171_flag;
    uint32_t forced_errata_ready;  /* EVENTCAUSE.READY with 171+187 applied */
    uint32_t forced_errata_spins;
    uint32_t forced_errata_pullup; /* USBPULLUP after that */

    /* Version 23 samples the attach sequence every millisecond from immediately
     * after USB initialization. It records whether the pull-up and endpoints
     * were visible before the driver entered low-power mode.
     */
    uint32_t early_samples;
    uint32_t early_pullup_seen;      /* USBPULLUP ever read nonzero */
    uint32_t early_pullup_first_ms;  /* when, or 0xFFFFFFFF for never */
    uint32_t early_epinen_max;       /* max EPINEN seen (reset value is 1) */
    uint32_t early_epouten_max;
    uint32_t early_lowpower_first_ms;/* when the driver suspended USBD */
    uint32_t early_eventcause_or;    /* OR of every EVENTCAUSE sample */

    /* Version 24 records a delayed direct detach/reattach and subsequent host
     * events.
     */
    uint32_t reattach_lowpower_before; /* LOWPOWER before we force it clear */
    uint32_t reattach_pullup_low;      /* USBPULLUP read back after writing 0 */
    uint32_t reattach_pullup_high;     /* USBPULLUP read back after writing 1 */
    uint32_t reattach_status;          /* zmk_usb_get_status() 2 s after */
    uint32_t reattach_eventcause_or;   /* EVENTCAUSE OR over the 2 s after */

    /* Version 25 performs detach/reattach through usb_disable()/usb_enable() so
     * the controller and endpoint state are rebuilt by the driver.
     */
    int32_t  reattach2_disable_rc;
    int32_t  reattach2_enable_rc;
    uint32_t reattach2_pullup_seen;     /* pullup observed after re-enable */
    uint32_t reattach2_pullup_first_ms;

    /* Version 26 stores power and clock snapshots before and after USB
     * initialization. Index 0 is PRE_KERNEL_1; index 1 is the beginning of the
     * early USB watcher.
     */
    uint32_t pwr_snap[2][G4B_PWR_SNAP_WORDS];

    /* Version 27 records the VBUS/OUTPUTRDY wait immediately before USB attach.
     * The wait closes the boot-time race where VBUS is present before the USB
     * regulator output is ready.
     */
    uint32_t regwait_before;   /* USBREGSTATUS on entry */
    uint32_t regwait_after;    /* USBREGSTATUS on exit */
    uint32_t regwait_ms;       /* ms waited, or 0xFFFFFFFF if it timed out */

    /* Version 28 applies candidate regulator-start actions one at a time and
     * records the first step that asserts OUTPUTRDY.
     */
    uint32_t trig_after_trim;    /* after the 187/211 trim window */
    uint32_t trig_after_encycle; /* after cycling USBD->ENABLE 0 -> 1 */
    uint32_t trig_after_lowpower;/* after USBD->LOWPOWER = 0 */
    uint32_t trig_after_evtpost; /* after re-posting EVENTS_USBPWRRDY */
    uint32_t trig_which;         /* 0 none, 1 trim, 2 encycle, 3 lowpower, 4 evtpost */

    /* Version 29 holds USBPULLUP active for five seconds and records USBRESET,
     * EP0SETUP, and USBEVENT activity.
     */
    uint32_t hold_usbreset_or;
    uint32_t hold_ep0setup_or;
    uint32_t hold_usbevent_or;

    /* These words held the version 32..37 P0.26 experiment and are gamepad
     * endpoint counters in version 39. Reusing the offsets preserves the
     * 1280-byte record layout. The counters distinguish these cases:
     *   gp_writes  hid_int_ep_write() calls that returned 0
     *   gp_busy    reports dropped because the previous one was still in flight
     *   gp_err     hid_int_ep_write() failures (-EAGAIN while unconfigured or
     *              suspended is normal, not a fault)
     */
    uint32_t gp_writes;
    uint32_t gp_busy;
    uint32_t gp_err;

    /* Version 31 records whether the boot escape chord was detected. */
    uint32_t chord_window_ms;   /* how long the window actually ran */
    uint32_t chord_a1_frames;   /* 0xA1 exchanges completed in the window */
    uint32_t chord_seen;        /* 1 if the chord bit was ever set */
    uint32_t chord_first_ms;    /* when, or 0xFFFFFFFF if never */
    uint8_t  chord_bitmap_or[G4B_CHORD_BITMAP_BYTES]; /* OR of all bitmaps */

    /* Mode-switch telemetry reuses three padding bytes without changing the
     * version-37 record layout. PRESENT distinguishes older zero-filled
     * records.
     *
     * mode_class bit layout, see src/mode_g4b.h for the constants:
     *   bits 0-3  enum g4b_mode as the feed gate and the BLE gate saw it
     *   bit 4     SEEN_HIGH - a sample read >= the dongle threshold
     *   bit 5     SEEN_LOW  - a sample read <  the dongle threshold
     *   bit 6     reserved, zero
     *   bit 7     PRESENT - set when these fields were written
     */
    uint8_t  mode_class;  /* class + flags, see above and src/mode_g4b.h */
    uint16_t mode_mv;     /* P0.03 / AIN1 millivolts, the gate's own value */

    /* Version 32 records 0xA0 byte 2 for the full diagnostic window. Bit 7 is
     * the key-down level, bits 0..5 are travel magnitude, and 0x40 is the
     * startup plateau observed in stock captures.
     */
    uint32_t a0_polls;           /* 0xA0 polls that returned a configured frame */
    uint32_t a0_bit7_first_ms;   /* first key-down level, or 0xFFFFFFFF */
    uint32_t a0_b2_or;           /* OR of every byte[2] seen */
    uint32_t a0_low6_max;        /* max travel magnitude seen */
    uint32_t a0_plateau40;       /* samples reading exactly 0x40 */
    uint32_t a0_b2_last;         /* last byte[2] */

    /* Version 33 records bounded watchdog feeding and its stop reason. */
    uint32_t wdt_feeds;         /* successful RR[7] writes */
    uint32_t wdt_uptime_ms;     /* uptime when the record was captured */
    uint32_t wdt_crv;
    uint32_t wdt_rren;          /* must be 0x80: RR[7] is the live channel */
    uint32_t wdt_config;
    uint32_t wdt_stopped;       /* G4B_WDT_STOP_* */

    /* Version 34. Uptime at which the delayed USB-watch thread began. A non-zero
     * value shows that the samples were taken after application initialization,
     * around the real attach sequence.
     */
    uint32_t early_start_ms;

    /* Version 35. Legacy mode-switch survey. Min and max raw counts cover the
     * complete polling window so the switch can move at any time.
     *
     * Channel order follows g4b_saadc_pselp: P0.03, P0.28, P0.29, P0.30, P0.31.
     * Millivolts are raw * 3600 / 4096 (gain 1/6, internal 0.6 V reference).
     * The mode pin is the one channel whose spread covers roughly 0 -> 1800 ->
     * 3300 mV; a floating pin wanders without landing on three levels.
     */
    uint16_t ain_min[5];
    uint16_t ain_max[5];
    uint16_t ain_last[5];
    uint16_t ain_samples;

    /* Version 36 records SPIM2 state inherited at payload entry. SPIM2 drives
     * the IS31FL3743B RGB controller; CS uses GPIO P0.11 rather than hardware
     * CSN. PSEL bit 31 means disconnected.
     */
    uint32_t spim2_psel_sck;
    uint32_t spim2_psel_mosi;
    uint32_t spim2_psel_miso;
    uint32_t spim2_psel_csn;
    uint32_t spim2_enable;
    uint32_t spim2_frequency;
    uint32_t spim2_config;

    /* Reuses the former spim2_reserved word at the same offset and size, keeping
     * the version-37 record at 1280 bytes.
     *
     * Uptime of the most recent successful mode-switch read. Compare it against
     * wdt_uptime_ms in the same record - both are now filled at the same instant
     * by s3_fill_live() in link_g4b.c - and a gap larger than a few seconds
     * means the 1 Hz sampler has stalled. The voltage says what was read,
     * mode_class says how it was classified, and this field says when it was
     * last read.
     */
    uint32_t mode_sample_ms;

    /* Version 37 captures keyboard-loop reads without ingesting them. Histogram
     * indexes 0..10 represent bitmap popcount; index 11 is 11 or more. Samples
     * retain the highest-popcount bitmaps, status byte, and SPIM result.
     */
    uint32_t kbd_cap_reads;
    uint32_t kbd_cap_ok;
    uint16_t kbd_cap_hist[12];
    uint16_t kbd_cap_max;
    uint16_t kbd_cap_samples;
    uint8_t  kbd_cap_sample[8][11];
} __packed;

/* Stage 4: read-only internal-flash survey.
 *
 * Region 0 covers 0x00000000..0x0001C000, the stock MBR+SoftDevice prefix.
 * Region 1 covers the unused-space candidate at 0x00067000..0x00080000. One
 * CRC and one sample are stored per 4 KiB page.
 *
 * The protected factory bootloader is a separate region and was never
 * obtained.
 */

#define G4B_EVIDENCE_VERSION_S4 7u
#define G4B_S4_PAGE 4096u
#define G4B_S4_REGIONS 2
#define G4B_S4_MAX_PAGES 64
#define G4B_S4_SAMPLE 64

struct g4b_record_s4 {
    struct g4b_s2_head head;

    uint32_t page_size;
    uint32_t region_start[G4B_S4_REGIONS];
    uint32_t region_end[G4B_S4_REGIONS];
    uint32_t pages_surveyed;
    uint32_t pages_erased; /* every word 0xFFFFFFFF */
    uint32_t pages_zero;   /* every word 0x00000000 */

    /* Selected UICR words recorded by the read-only survey. */
    uint32_t uicr_approtect;
    uint32_t uicr_nrffw0;
    uint32_t uicr_bootloaderaddr;

    /* First bytes of each region, so the content is identifiable and not just
     * hashed.
     */
    uint8_t sample[G4B_S4_REGIONS][G4B_S4_SAMPLE];

    uint32_t page_crc[G4B_S4_MAX_PAGES];
} __packed;

/* Stage 5: raw internal-flash dump. APXD records carry one 4 KiB page and its
 * CRC-32. The default range is the stock MBR+SoftDevice prefix.
 */

#define G4B_EVIDENCE_VERSION_S5 8u
#define G4B_S5_MAGIC 0x44585041u /* "APXD" little-endian */
#define G4B_S5_CHUNK 4096u
#if IS_ENABLED(CONFIG_APEX_G4B_DUMP_LOADER)
/* Optional target for the protected factory-loader range. No capture of this
 * range was obtained before the original Nordic flash was erased.
 */
#define G4B_S5_BOOT_START 0x0006E000u
#define G4B_S5_BOOT_END 0x00079000u
#define G4B_S5_CHUNKS 12 /* 11 loader pages plus one UICR page */
#else
/* Stock MBR+SoftDevice prefix. */
#define G4B_S5_BOOT_START 0x00000000u
#define G4B_S5_BOOT_END 0x0001C000u
#define G4B_S5_CHUNKS 29 /* 28 MBR+SoftDevice pages plus one UICR page */
#endif
#define G4B_S5_UICR 0x10001000u

struct g4b_dump_chunk {
    uint32_t magic;
    uint16_t version;
    uint16_t seq;
    uint32_t addr;
    uint32_t length;
    uint8_t data[G4B_S5_CHUNK];
    uint32_t crc32; /* over addr, length and data */
} __packed;

/* Stage 6: raw NVS persistence test.
 * Each launch reads and increments a counter. The before/after flash samples
 * distinguish successful API calls from actual media updates.
 */

#define G4B_EVIDENCE_VERSION_S6 9u
#define G4B_S6_KEY 0x4150u /* "AP" */
#define G4B_S6_MAGIC 0x58455041u /* "APEX" */
#define G4B_S6_PEEK 32

struct g4b_record_s6 {
    struct g4b_s2_head head;

    uint32_t partition_offset;
    uint32_t partition_size;
    uint32_t sector_size;
    uint32_t sector_count;

    int32_t mount_rc;
    int32_t read_rc;
    int32_t write_rc;

    uint32_t magic_read;   /* G4B_S6_MAGIC if a previous launch wrote it */
    uint32_t counter_read; /* what this launch found */
    uint32_t counter_write;/* what this launch stored */

    /* Raw flash at the partition start, before and after. NVS writes its own
     * sector headers, so this shows the medium changing rather than only the
     * API returning 0.
     */
    uint8_t peek_before[G4B_S6_PEEK];
    uint8_t peek_after[G4B_S6_PEEK];
} __packed;

/* Stage 7: settings and Bluetooth-settings persistence test.
 * The record tracks explicit settings load/save results and whether ZMK's
 * earlier settings_load() found the key. BLE connectivity is tested externally.
 */

#define G4B_EVIDENCE_VERSION_S7 10u
#define G4B_S7_MAGIC 0x58455041u /* "APEX" */
#define G4B_S7_PEEK 32

struct g4b_record_s7 {
    struct g4b_s2_head head;

    int32_t init_rc;
    int32_t load_rc;
    int32_t save_rc;

    uint32_t found_at_thread_start; /* set by ZMK's own settings_load() */
    uint32_t found_after_load;      /* set by our explicit subtree load */
    uint32_t counter_read;
    uint32_t counter_write;

    uint8_t peek_before[G4B_S7_PEEK];
    uint8_t peek_after[G4B_S7_PEEK];
} __packed;

void g4b_evidence_init(void);
void g4b_evidence_emit_text(const uint8_t *text, uint32_t length);
void g4b_evidence_emit(const struct g4b_record *record);
void g4b_evidence_emit_s1(const struct g4b_record_s1 *record);
void g4b_evidence_emit_s2_replay(const struct g4b_record_s2_replay *record);
void g4b_evidence_emit_s2_confirm(const struct g4b_record_s2_confirm *record);
void g4b_evidence_emit_s3(const struct g4b_record_s3 *record);
void g4b_evidence_emit_s4(const struct g4b_record_s4 *record);
void g4b_evidence_dump_open(void);
void g4b_evidence_dump_chunk(const struct g4b_dump_chunk *chunk);
void g4b_evidence_dump_close(void);
void g4b_evidence_emit_s6(const struct g4b_record_s6 *record);
void g4b_evidence_emit_s7(const struct g4b_record_s7 *record);

/* DWT cycle counter, enabled once by g4b_evidence_init(). */
uint32_t g4b_cyccnt(void);
