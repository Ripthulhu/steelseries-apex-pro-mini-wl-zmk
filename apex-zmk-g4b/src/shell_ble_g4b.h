/* SPDX-License-Identifier: MIT */
#ifndef APEX_G4B_SHELL_BLE_H
#define APEX_G4B_SHELL_BLE_H

#include <stdbool.h>

/* Opt-in wireless shell over the BLE Nordic UART Service (NUS). The service is
 * additive GATT next to ZMK's HID/Studio services (advertising is untouched);
 * a NUS client on a connected host reaches the `apex-ble$` console. The bridge
 * is gated by a persisted enable so it can be turned on for good, and stays off
 * by default so it is not an ambient wireless debug surface. */

/* Enable/disable the BLE shell bridge and persist the choice to NVS. */
void g4b_ble_shell_set_enabled(bool on);

/* Current runtime state and whether a NUS client has subscribed. */
bool g4b_ble_shell_is_enabled(void);
bool g4b_ble_shell_subscribed(void);

#endif /* APEX_G4B_SHELL_BLE_H */
