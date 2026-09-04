/* SPDX-License-Identifier: MIT */
#ifndef APEX_G4B_SAADC_H
#define APEX_G4B_SAADC_H

#include <stdint.h>

/* The analog-capable pins that are not part of the STM32 link:
 * P0.03, P0.28, P0.29, P0.30, P0.31.
 */
#define G4B_SAADC_CHANNELS 5u

/* A 12-bit single-ended conversion can never produce INT16_MIN. Callers that
 * make control decisions must check this rather than treating a timed-out
 * conversion as a real 0 V sample. */
#define G4B_SAADC_READ_FAILED INT16_MIN

extern const uint8_t g4b_saadc_pselp[G4B_SAADC_CHANNELS];

/* Blocking, iteration-bounded single conversion. Returns the raw 12-bit
 * value; millivolts are result * 3600 / 4096. Returns
 * G4B_SAADC_READ_FAILED if START or END does not complete in bounds.
 */
int16_t g4b_saadc_read(uint8_t pselp);

#endif /* APEX_G4B_SAADC_H */
