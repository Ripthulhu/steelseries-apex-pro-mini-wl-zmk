/* SPDX-License-Identifier: MIT
 *
 * Starts the nRF52833 watchdog for production builds and adopts the same
 * watchdog when it survives a software reset through the bootloader. Older
 * diagnostic images can still inherit this configuration from their wrapper.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>

#include "evidence_g4b.h"
#include "mode_g4b.h"
#include "pins_g4b.h"
#include "wdt_g4b.h"

#define G4B_WDT_BASE      0x40010000u
#define G4B_WDT_START     (G4B_WDT_BASE + 0x000u)
#define G4B_WDT_CRV       (G4B_WDT_BASE + 0x504u)
#define G4B_WDT_RREN      (G4B_WDT_BASE + 0x508u)
#define G4B_WDT_CONFIG    (G4B_WDT_BASE + 0x50Cu)
#define G4B_WDT_RUNSTATUS (G4B_WDT_BASE + 0x400u)
#define G4B_WDT_RR(n)     (G4B_WDT_BASE + 0x600u + 4u * (n))

/* Must match failsafe_handoff.S as built by build_g4b.py. Do not feed a
 * watchdog whose configuration differs from these values.
 */
#define G4B_WDT_EXPECT_CRV    0x001E0000u   /* 60.0 s */
#define G4B_WDT_EXPECT_RREN   0x00000080u   /* RR[7] only */
#define G4B_WDT_EXPECT_CONFIG 0x00000009u
#define G4B_WDT_CHANNEL       7u
#define G4B_WDT_MAGIC         0x6E524635u
#define G4B_WDT_START_TASK    1u

/* Comfortably inside the 60 s window, and often enough that a couple of missed
 * ticks do not reset the device.
 */
#define G4B_WDT_FEED_PERIOD_MS 15000
#define G4B_WDT_KEYBOARD_GRACE_MS 30000u

/* Pin the reload-register address independently of compiler code generation. */
BUILD_ASSERT(G4B_WDT_RR(G4B_WDT_CHANNEL) == 0x4001061Cu,
	     "the feed must target RR[7]; RREN=0x80 makes every other channel a no-op");
/* Legacy diagnostic images use a fixed feed budget. Production images use the
 * health checks below and A/B recovery instead. */
#if !IS_ENABLED(CONFIG_APEX_G4B_WATCHDOG)
#if !IS_ENABLED(CONFIG_APEX_G4B_FEED_UNBOUNDED)
BUILD_ASSERT(CONFIG_APEX_G4B_FEED_BUDGET_MS >= CONFIG_APEX_G4B_DEADLINE_MS,
             "feed budget must outlast the poll window or no record is emitted");
#endif

BUILD_ASSERT(CONFIG_APEX_G4B_FEED_BUDGET_MS > 0,
	     "the diagnostic watchdog feed budget must be positive");
#endif

struct g4b_wdt_state g4b_wdt;

/* Use a dedicated thread so settings work cannot delay watchdog servicing. */
#define G4B_WDT_SAMPLE_PERIOD_MS 1000u
#define G4B_WDT_STACK_SIZE       768

static bool wdt_config_is_ours(void)
{
	return *(volatile uint32_t *)G4B_WDT_CRV == G4B_WDT_EXPECT_CRV &&
	       *(volatile uint32_t *)G4B_WDT_RREN == G4B_WDT_EXPECT_RREN &&
	       *(volatile uint32_t *)G4B_WDT_CONFIG == G4B_WDT_EXPECT_CONFIG;
}

/* A terminal fault disables feeding for the remainder of this launch. */
static bool wdt_stopped_for_good;
static volatile bool keyboard_heartbeat_started;
static volatile uint32_t keyboard_heartbeat_ms;

void g4b_wdt_keyboard_heartbeat(void)
{
	keyboard_heartbeat_ms = k_uptime_get_32();
	keyboard_heartbeat_started = true;
}

/* One feed decision. Returns true if the watchdog was actually fed. */
static bool wdt_feed_once(uint32_t up)
{
	/* Evaluate every stop condition before writing the reload register. */
#if !IS_ENABLED(CONFIG_APEX_G4B_WATCHDOG) && \
	!IS_ENABLED(CONFIG_APEX_G4B_FEED_UNBOUNDED)
	if (up >= CONFIG_APEX_G4B_FEED_BUDGET_MS) {
		g4b_wdt.stopped_reason = G4B_WDT_STOP_BUDGET;
		wdt_stopped_for_good = true;
		return false;
	}
#endif

	/* Require a recent mode sample before feeding. A blocked sampler therefore
	 * lets the watchdog transfer control to the recovery loader.
	 */
	if ((up - g4b_mode_last_sample_ms()) > (4u * G4B_WDT_FEED_PERIOD_MS)) {
		g4b_wdt.stopped_reason = G4B_WDT_STOP_HATCH;
		wdt_stopped_for_good = true;
		return false;
	}

#if IS_ENABLED(CONFIG_APEX_G4B_WATCHDOG)
	/* Give startup and scanner configuration time to finish. Once the keyboard
	 * loop begins, require it to keep moving. A build with the experimental
	 * dongle transport has a different loop in that switch position. */
	if ((!IS_ENABLED(CONFIG_APEX_G4B_DONGLE_RADIO) ||
	     g4b_mode_get() != G4B_MODE_DONGLE) &&
	    ((!keyboard_heartbeat_started && up >= G4B_WDT_KEYBOARD_GRACE_MS) ||
	     (keyboard_heartbeat_started &&
	      (up - keyboard_heartbeat_ms) >= G4B_WDT_KEYBOARD_GRACE_MS))) {
		g4b_wdt.stopped_reason = G4B_WDT_STOP_KEYBOARD;
		wdt_stopped_for_good = true;
		return false;
	}
#endif

	/* Strap-based feed suppression is limited to legacy diagnostic builds.
	 * Production recovery uses the Adafruit bootloader, A/B fallback, and reset. */
	if (!IS_ENABLED(CONFIG_APEX_G4B_WATCHDOG) && g4b_strap_asserted()) {
		g4b_wdt.stopped_reason = G4B_WDT_STOP_STRAP;
		return false;
	}

	if (!wdt_config_is_ours()) {
		g4b_wdt.stopped_reason = G4B_WDT_STOP_CONFIG;
		wdt_stopped_for_good = true;
		return false;
	}

	*(volatile uint32_t *)G4B_WDT_RR(G4B_WDT_CHANNEL) = G4B_WDT_MAGIC;
	g4b_wdt.feeds++;
	g4b_wdt.stopped_reason = G4B_WDT_STOP_NONE;
	return true;
}

static void wdt_thread_main(void *a, void *b, void *c)
{
	uint32_t last_feed_ms = 0u;
	bool fed_once = false;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		uint32_t up;

		/* Sample the switch immediately before evaluating the feed conditions. */
		g4b_mode_sample();

		up = k_uptime_get_32();
		g4b_wdt.last_uptime_ms = up;

		if (!wdt_stopped_for_good &&
		    (!fed_once || (up - last_feed_ms) >= G4B_WDT_FEED_PERIOD_MS)) {
			if (wdt_feed_once(up)) {
				last_feed_ms = up;
				fed_once = true;
			}
		}

		k_msleep(G4B_WDT_SAMPLE_PERIOD_MS);
	}
}

/* Cooperative priority keeps servicing independent of ordinary application
 * work. The thread sleeps between one-second sampling intervals.
 */
K_THREAD_DEFINE(g4b_wdt_thread, G4B_WDT_STACK_SIZE, wdt_thread_main,
		NULL, NULL, NULL, K_PRIO_COOP(4), 0, 0);

static int g4b_wdt_init(void)
{
	/* The nRF52 watchdog cannot be stopped by a software reset. Configure it
	 * only on a cold boot; after DFU or a watchdog reset, adopt the existing
	 * instance instead. The Adafruit bootloader feeds every enabled channel in
	 * its DFU loop, including RR[7]. */
#if IS_ENABLED(CONFIG_APEX_G4B_WATCHDOG)
	if (*(volatile uint32_t *)G4B_WDT_RUNSTATUS == 0u) {
		*(volatile uint32_t *)G4B_WDT_CRV = G4B_WDT_EXPECT_CRV;
		*(volatile uint32_t *)G4B_WDT_RREN = G4B_WDT_EXPECT_RREN;
		*(volatile uint32_t *)G4B_WDT_CONFIG = G4B_WDT_EXPECT_CONFIG;
		__DSB();
		*(volatile uint32_t *)G4B_WDT_START = G4B_WDT_START_TASK;
		__DSB();
	}
#endif

	g4b_wdt.crv = *(volatile uint32_t *)G4B_WDT_CRV;
	g4b_wdt.rren = *(volatile uint32_t *)G4B_WDT_RREN;
	g4b_wdt.config = *(volatile uint32_t *)G4B_WDT_CONFIG;
	g4b_wdt.runstatus = *(volatile uint32_t *)G4B_WDT_RUNSTATUS;
#if IS_ENABLED(CONFIG_APEX_G4B_WATCHDOG)
	g4b_wdt.budget_ms = 0u;
#else
	g4b_wdt.budget_ms = CONFIG_APEX_G4B_FEED_BUDGET_MS;
#endif

	/* Feed immediately because an inherited watchdog may already be well into
	 * its timeout window. */
	(void)wdt_feed_once(k_uptime_get_32());
	return 0;
}

SYS_INIT(g4b_wdt_init, APPLICATION, 98);
