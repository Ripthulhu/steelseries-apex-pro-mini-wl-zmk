/* SPDX-License-Identifier: MIT
 *
 * BLE-controller standdown for the dongle (2.4 GHz) mode. See radio_g4b.c.
 */
#ifndef APEX_G4B_RADIO_G4B_H_
#define APEX_G4B_RADIO_G4B_H_

#include <stdbool.h>
#include <stdint.h>

/* Standdown status, for evidence / telemetry and for gating BLE work once the
 * controller has been torn down. */
struct g4b_radio_status {
	uint8_t  requested;   /* dongle mode at boot, so a standdown was asked for */
	uint8_t  bt_ready;    /* bt_is_ready() observed true before the disable    */
	uint8_t  stood_down;  /* bt_disable() returned success; RADIO now free      */
	int16_t  disable_err; /* bt_disable() return code (0 on success)           */
	uint16_t wait_ms;     /* time spent waiting for the stack to become ready  */
};

const struct g4b_radio_status *g4b_radio_get_status(void);

/* True once the BLE controller has been stood down and NRF_RADIO is free for
 * reuse. Safe to call from the mode gate every tick. */
bool g4b_radio_stood_down(void);

#endif /* APEX_G4B_RADIO_G4B_H_ */
