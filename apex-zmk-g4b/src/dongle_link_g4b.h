/* SPDX-License-Identifier: MIT
 *
 * Operational (post-pairing) 2.4 GHz link to the SteelSeries receiver.
 *
 * radio_esb_g4b.c recovers the pairing PHY and (up to the handoff) completes the
 * pairing record exchange. This module is the layer that builds on top: the
 * CCM-encrypted operational link that carries key reports once the receiver has
 * bonded. It runs on the NRF_RADIO the BLE controller stood down (radio_g4b.c),
 * reusing the TX/RX primitives in radio_esb_g4b.h.
 *
 * State of the work: SCAFFOLD. Every constant here is recovered and recorded in
 * work/DONGLE_RE_2026-09-04.md, but the link is not proven end to end. The known
 * open gap is the microsecond-precise TX->RX turnaround (stock uses TIMER2 + PPI)
 * and the exact CCM data-structure packing; both are marked TODO in the .c.
 *
 * Gated behind CONFIG_APEX_G4B_DONGLE_LINK (implies CONFIG_APEX_G4B_ESB).
 * Release firmware leaves it disabled.
 */
#ifndef APEX_G4B_DONGLE_LINK_G4B_H_
#define APEX_G4B_DONGLE_LINK_G4B_H_

#include <stdbool.h>
#include <stdint.h>

#include "kscan_g4b.h" /* APEX_G4B_KEY_BITMAP_SIZE */

/* Release the operational-link thread. Called from g4b_esb_on_radio_free() in a
 * DONGLE_LINK build, in place of the pairing probe, once NRF_RADIO is free and
 * the vendor PHY is applied. Assumes the receiver is already bonded to this
 * keyboard (the bond survives reboots), so it goes straight to the operational
 * A0/90/90 counter exchange rather than repeating pairing. */
void g4b_dongle_link_start(void);

/* Feed one fresh absolute key bitmap (APEX_G4B_KEY_BITMAP_SIZE bytes) from the
 * STM32 scanner loop. The module deduplicates internally and queues a report on
 * every change, including the all-zero release. Safe to call from the scanner
 * thread while the link thread drains the queue. */
void g4b_dongle_link_on_bitmap(const uint8_t *bitmap);

#endif /* APEX_G4B_DONGLE_LINK_G4B_H_ */
