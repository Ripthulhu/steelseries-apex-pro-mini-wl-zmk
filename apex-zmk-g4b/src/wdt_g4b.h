/* SPDX-License-Identifier: MIT */
#ifndef APEX_G4B_WDT_H
#define APEX_G4B_WDT_H

/* Called by the scanner loop. The watchdog stops being fed if these calls stop
 * for 30 seconds. */
void g4b_wdt_keyboard_heartbeat(void);

#endif /* APEX_G4B_WDT_H */
