/* SPDX-License-Identifier: MIT */
#ifndef APEX_G4B_RECOVERY_H
#define APEX_G4B_RECOVERY_H

/* Enter the current Adafruit UF2/serial DFU bootloader by writing its GPREGRET
 * magic and resetting. Does not return. See recovery_g4b.c.
 */
void g4b_enter_recovery(void);

#endif /* APEX_G4B_RECOVERY_H */
