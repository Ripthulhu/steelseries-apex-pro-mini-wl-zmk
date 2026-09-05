/* SPDX-License-Identifier: MIT
 *
 * Bare-metal 2.4 GHz ESB-compatible PHY for the vendor receiver. See
 * radio_esb_g4b.c. Only usable after the BLE controller has been stood down
 * (radio_g4b.c) so NRF_RADIO is free.
 */
#ifndef APEX_G4B_RADIO_ESB_G4B_H_
#define APEX_G4B_RADIO_ESB_G4B_H_

#include <stdbool.h>
#include <stdint.h>

/* Start the HF crystal oscillator. The RADIO needs HFXO (not the RC) for the
 * 2 Mbit PHY; the BLE controller released its clock request during standdown,
 * so it must be requested here. Bounded spin; returns true if it started. */
bool g4b_esb_hfclk_start(void);

/* Program NRF_RADIO with the confirmed operational vendor PHY. `statlen` is the
 * static on-air payload length (PCNF0=0 => no LENGTH field); it goes into
 * PCNF1.STATLEN. Everything else is fixed (address, CRC-24, MODE, whitening).
 * Does not enable TX/RX - just the register block. */
void g4b_esb_configure(uint8_t statlen);

/* Set the RF channel: RADIO.FREQUENCY = ch (2400 + ch MHz). ch is 0..100. */
void g4b_esb_set_channel(uint8_t ch);

/* Blocking single-packet TX on channel `ch`. Transmits the configured static
 * length (PCNF1.STATLEN) bytes from `buf` (must be in RAM, >= STATLEN). Runs the
 * TXEN->READY->START->END->DISABLE ramp via SHORTS. Returns true if it completed
 * (END/DISABLED reached) within the internal timeout. */
bool g4b_esb_tx(uint8_t ch, const uint8_t *buf);

/* Blocking single-packet RX on channel `ch`, up to `timeout_us`. Fills `buf`
 * (>= STATLEN) with the received static-length payload. Returns 1 = packet + CRC
 * OK, 0 = packet received but CRC failed, -1 = timed out (no packet). */
int g4b_esb_rx(uint8_t ch, uint8_t *buf, uint32_t timeout_us);

/* Called after BLE releases NRF_RADIO. Starts HFXO, applies the experimental
 * radio configuration, and emits a register snapshot over the evidence UART. */
void g4b_esb_on_radio_free(void);

#endif /* APEX_G4B_RADIO_ESB_G4B_H_ */
