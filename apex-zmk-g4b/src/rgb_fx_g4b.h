/* SPDX-License-Identifier: MIT */
#ifndef APEX_G4B_RGB_FX_H
#define APEX_G4B_RGB_FX_H

#include <stdbool.h>
#include <stdint.h>

/* Board-specific effects layered over ZMK underglow. Effect 0 passes ZMK's
 * frame through unchanged; the remaining effects render in the existing LED
 * driver shim without modifying upstream ZMK.
 */
enum g4b_fx {
    G4B_FX_ZMK = 0,   /* ZMK underglow, unchanged */
    G4B_FX_PLASMA,    /* interfering sines, 8-bit dithered to look deeper */
    G4B_FX_FIRE,      /* per-LED heat, sparks and cooling */
    G4B_FX_AURORA,    /* slow drifting hue bands */
    G4B_FX_SPARKLE,   /* decaying starfield over a dim base */
    G4B_FX_RIPPLE,    /* rings expanding across the real board geometry */
    G4B_FX_REACTIVE,  /* keys flare and fade on press */
    G4B_FX_ANALOG,    /* brightness follows how far each key is pressed */
    G4B_FX_SHOCKWAVE, /* every key press launches a coloured spatial ring */
    G4B_FX_HEATMAP,   /* no background: dark at rest, green->red with depth */
    G4B_FX_RAIN,      /* layered falling trails over the real key geometry */
    G4B_FX_INK,       /* key presses diffuse as prismatic colour clouds */
    G4B_FX_COUNT,
};

/* The custom renderer is intentionally capped at 200 Hz. The Bluetooth idle
 * policy uses the same period while an effect remains visible, so it does not
 * wake the scan thread between frames that cannot render anything new. */
#define G4B_FX_PERIOD_MS 5u

/* Feed the reactive effects. Both take a SCAN INDEX (0..69), which is how the
 * key path and the 0xA2 analog path both address keys - the map turns it into
 * an LED.
 *
 * g4b_fx_key: a key crossed its actuation threshold. Seeds the selected
 * event-driven effect at its LED, driven from the already-ingested bitmap, so
 * it costs no extra scanner traffic. Called from the ingest path on the g4b
 * thread.
 *
 * g4b_fx_depth: how far a key is physically pressed, 0..255, from a 0xA2 raw
 * ADC sample. Consulted only by the two effects that genuinely visualize Hall
 * depth (ANALOG and HEATMAP). Called from the analog sampler, which runs in the
 * ATTN-low branch so it cannot race a key report.
 *
 * Both just store into a buffer; neither touches the bus. If no reactive effect
 * is selected the stores are harmless and the render side ignores them.
 */
void g4b_fx_key(uint8_t scan_idx);
void g4b_fx_depth(uint8_t scan_idx, uint8_t q8);

/* Does the current effect want per-key analog depth? The scan loop uses this to
 * decide whether to spend 0xA2 bus time - no point sampling if nothing renders
 * it. True only for G4B_FX_ANALOG and G4B_FX_HEATMAP.
 */
bool g4b_fx_wants_analog(void);

/* Advance to the next effect, wrapping. Persisted with the other switch
 * settings, so it survives a reboot like everything else on this keyboard.
 */
void g4b_fx_cycle(void);

enum g4b_fx g4b_fx_current(void);
void g4b_fx_set(enum g4b_fx fx);

/* Render one frame if one is due. Called from the g4b thread every scan pass,
 * which is the only thread allowed to transmit on SPIM2.
 *
 * Returns true if it rendered and staged a frame, meaning ZMK's staged frame
 * should be ignored this pass. Returns false in passthrough, so the caller
 * behaves exactly as it did before this file existed.
 */
bool g4b_fx_tick(void);

#endif /* APEX_G4B_RGB_FX_H */
