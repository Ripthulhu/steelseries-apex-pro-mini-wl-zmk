/* SPDX-License-Identifier: MIT
 *
 * led_strip shim over the direct-register IS31FL3743B driver (rgb_g4b.c).
 *
 * This adapter implements Zephyr's led_strip API over the direct SPIM2 driver.
 * It avoids enabling the generic SPI, pinctrl, and GPIO subsystems and keeps
 * board pin ownership in pins_g4b.c.
 */

#define DT_DRV_COMPAT apex_g4b_led_strip

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>

#include "rgb_g4b.h"
#include "rgb_fx_g4b.h"

static int g4b_strip_update_rgb(const struct device *dev, struct led_rgb *pixels,
                                size_t num_pixels)
{
    ARG_UNUSED(dev);

    /* Only one renderer may write the frame buffer. Drop ZMK underglow frames
     * while the high-rate effect engine owns it; passthrough mode accepts ZMK
     * as the writer. g4b_fx_current() is a plain read.
     */
    if (g4b_fx_current() != G4B_FX_ZMK) {
        return 0;
    }

    /* This runs on ZMK's low-priority underglow workqueue, NOT the g4b thread.
     * Writing the frame buffer here is fine (plain RAM); transmitting is not
     * done here, so that all SPIM access stays on one thread and needs no bus
     * lock. The g4b thread flushes within ~1 ms, far faster than the 50 ms
     * effect tick, so no frame is dropped. See g4b_rgb_mark_pending.
     */
    size_t n = (num_pixels < G4B_RGB_LEDS) ? num_pixels : G4B_RGB_LEDS;

    for (size_t i = 0U; i < n; i++) {
        g4b_rgb_set_pixel((uint8_t)i, pixels[i].r, pixels[i].g, pixels[i].b);
    }

    g4b_rgb_mark_pending();
    return 0;
}

/* ZMK passes DT_PROP(chain_length) as num_pixels, and Zephyr's
 * led_strip_update_rgb() returns -ERANGE when api->length(dev) is smaller. Both
 * are 66 today, so returning the driver's constant worked by coincidence; raise
 * chain-length in the overlay and the whole strip would go dark at runtime with
 * nothing in this file pointing at the cause. Take the length from devicetree
 * and make the mismatch a compile error instead.
 */
#define G4B_STRIP_LEN DT_INST_PROP(0, chain_length)
BUILD_ASSERT(G4B_STRIP_LEN == G4B_RGB_LEDS,
             "chain-length in the overlay must equal G4B_RGB_LEDS");

static size_t g4b_strip_length(const struct device *dev)
{
    ARG_UNUSED(dev);

    return G4B_STRIP_LEN;
}

static int g4b_strip_init(const struct device *dev)
{
    ARG_UNUSED(dev);

    /* Brings up SPIM2, the P0.11 CS and the IS31FL3743B (current, scaling,
     * out of shutdown). Runs at POST_KERNEL, after the vendor rails are raised
     * at PRE_KERNEL_1, so the RGB rails (P0.19/P0.23) are already on.
     */
    return g4b_rgb_init();
}

static const struct led_strip_driver_api g4b_strip_api = {
    .update_rgb = g4b_strip_update_rgb,
    .update_channels = NULL,
    .length = g4b_strip_length,
};

DEVICE_DT_INST_DEFINE(0, g4b_strip_init, NULL, NULL, NULL, POST_KERNEL,
                      CONFIG_LED_STRIP_INIT_PRIORITY, &g4b_strip_api);
