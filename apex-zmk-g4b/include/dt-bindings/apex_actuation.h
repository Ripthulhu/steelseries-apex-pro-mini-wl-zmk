/* SPDX-License-Identifier: MIT
 *
 * Parameter values for &actuation. Must match behavior_actuation_g4b.c - a
 * mismatch binds the wrong action to a key with no error anywhere.
 */
#ifndef DT_BINDINGS_APEX_ACTUATION_H
#define DT_BINDINGS_APEX_ACTUATION_H

#define ACT_DEEPER    0 /* actuation point further down - less sensitive */
#define ACT_SHALLOW   1 /* actuation point closer to the top - more sensitive */
#define ACT_RT_UP     2 /* rapid trigger less sensitive, +0.1 mm */
#define ACT_RT_DOWN   3 /* rapid trigger more sensitive, -0.1 mm */
#define ACT_RT_TOG    4 /* rapid trigger off, or back to its last setting */
#define ACT_RESET     5 /* cycle the reset point: 2, 4, 6, 8 counts below press */
#define ACT_FX_NEXT   6 /* next high-rate lighting effect, or back to ZMK */
#define ACT_RECOVERY  7 /* optional software recovery action */

#endif /* DT_BINDINGS_APEX_ACTUATION_H */
