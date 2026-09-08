/* SPDX-License-Identifier: MIT */
#ifndef STUDIO_RADIO_G4B_H
#define STUDIO_RADIO_G4B_H
#include <stddef.h>
#include <stdint.h>

/* Push inbound ZMK Studio RPC bytes (an APEX_STUDIO_DATA frame payload, arriving
 * over the radio) into the RPC receive ring. Called from the radio shell
 * dispatch on the g4b_radio_shell thread. */
void g4b_studio_radio_feed(const uint8_t *data, size_t len);

#endif
