/* SPDX-License-Identifier: MIT
 *
 * BLE-controller shutdown for the experimental 2.4 GHz dongle transport.
 * Zephyr's BLE controller owns NRF_RADIO, RTC0, TIMER0, and PPI. This module
 * waits for Bluetooth initialization, calls bt_disable(), and releases those
 * resources before the direct radio driver starts. Radio ownership is selected
 * at boot; crossing the dongle boundary resets the board through mode_g4b.c.
 * Release firmware leaves CONFIG_APEX_G4B_DONGLE_RADIO disabled.
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>

#include "evidence_g4b.h"
#include "mode_g4b.h"
#include "radio_g4b.h"
#if IS_ENABLED(CONFIG_APEX_G4B_ESB)
#include "radio_esb_g4b.h"
#endif

/* Bounded wait for the async bt_enable() to finish before we tear it down. The
 * stack is normally ready within a few hundred ms of boot. */
#define G4B_STANDDOWN_POLL_MS 50u
#define G4B_STANDDOWN_MAX_MS  4000u

static struct g4b_radio_status status;

const struct g4b_radio_status *g4b_radio_get_status(void)
{
	return &status;
}

bool g4b_radio_stood_down(void)
{
	return status.stood_down != 0u;
}

/* "APXRADIO req=1 rdy=1 down=1 err=0 wait=150\r\n" - one line, best-effort. */
static void standdown_emit(void)
{
	static const char hexd[] = "0123456789abcdef";
	uint8_t line[64];
	uint32_t n = 0u;
	const char *tag = "APXRADIO req=";

	for (const char *p = tag; *p; p++) {
		line[n++] = (uint8_t)*p;
	}
	line[n++] = status.requested ? '1' : '0';
	line[n++] = ' '; line[n++] = 'r'; line[n++] = 'd'; line[n++] = 'y'; line[n++] = '=';
	line[n++] = status.bt_ready ? '1' : '0';
	line[n++] = ' '; line[n++] = 'd'; line[n++] = 'o'; line[n++] = 'w'; line[n++] = 'n'; line[n++] = '=';
	line[n++] = status.stood_down ? '1' : '0';
	line[n++] = ' '; line[n++] = 'e'; line[n++] = 'r'; line[n++] = 'r'; line[n++] = '=';
	/* err as signed decimal, small range */
	{
		int v = status.disable_err;
		if (v < 0) { line[n++] = '-'; v = -v; }
		if (v >= 100) { line[n++] = hexd[(v / 100) % 10]; }
		if (v >= 10)  { line[n++] = hexd[(v / 10) % 10]; }
		line[n++] = hexd[v % 10];
	}
	line[n++] = ' '; line[n++] = 'w'; line[n++] = 'a'; line[n++] = 'i'; line[n++] = 't'; line[n++] = '=';
	{
		uint32_t w = status.wait_ms;
		if (w >= 1000u) { line[n++] = hexd[(w / 1000u) % 10u]; }
		if (w >= 100u)  { line[n++] = hexd[(w / 100u) % 10u]; }
		if (w >= 10u)   { line[n++] = hexd[(w / 10u) % 10u]; }
		line[n++] = hexd[w % 10u];
	}
	line[n++] = '\r'; line[n++] = '\n';

	g4b_evidence_emit_text(line, n);
}

static void standdown_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(standdown_work, standdown_work_fn);

static void standdown_work_fn(struct k_work *work)
{
	int err;

	ARG_UNUSED(work);

	/* Wait for the asynchronous bt_enable() to complete, bounded. */
	if (!bt_is_ready()) {
		status.wait_ms += G4B_STANDDOWN_POLL_MS;
		if (status.wait_ms < G4B_STANDDOWN_MAX_MS) {
			k_work_schedule(&standdown_work, K_MSEC(G4B_STANDDOWN_POLL_MS));
			return;
		}
		/* Never became ready - do not call bt_disable() on a half-up stack.
		 * Report and stop; the RADIO stays with the controller. */
		standdown_emit();
		return;
	}

	status.bt_ready = 1u;
	err = bt_disable();
	status.disable_err = (int16_t)err;
	status.stood_down = (err == 0) ? 1u : 0u;

	/* NRF_RADIO is now idle (or the disable failed and BLE keeps it). */
	standdown_emit();

#if IS_ENABLED(CONFIG_APEX_G4B_ESB)
	/* Radio is ours: bring up HFXO, apply the vendor PHY config, report it. */
	if (status.stood_down) {
		g4b_esb_on_radio_free();
	}
#endif
}

static int g4b_radio_init(void)
{
	/* g4b_mode_init() (APPLICATION 90) has already taken the first switch
	 * sample, so the boot position is known here at 99. */
	if (g4b_mode_boot_dongle()) {
		status.requested = 1u;
		k_work_schedule(&standdown_work, K_MSEC(G4B_STANDDOWN_POLL_MS));
	}
	return 0;
}

/* After ZMK's BLE init (which calls bt_enable); the work item then waits for
 * readiness regardless of exact ordering. */
SYS_INIT(g4b_radio_init, APPLICATION, 99);
