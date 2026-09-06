/* SPDX-License-Identifier: MIT
 *
 * Keymap behavior for toggling Studio's USB RPC interface with Fn+RCtrl+S.
 * The behavior is always compiled so the shared keymap remains valid. Without
 * USB (CONFIG_ZMK_USB) its backing calls are no-ops.
 */

#define DT_DRV_COMPAT apex_behavior_studio_toggle

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>

#if IS_ENABLED(CONFIG_ZMK_USB)
extern void zmk_usb_request_studio(bool on); /* app/src/usb.c */
extern bool zmk_usb_studio_is_on(void);
#else
static inline void zmk_usb_request_studio(bool on) { ARG_UNUSED(on); }
static inline bool zmk_usb_studio_is_on(void) { return false; }
#endif

#if IS_ENABLED(CONFIG_APEX_G4B_FN_OVERLAY)
#include "rgb_overlay_g4b.h"
#else
static inline void g4b_rgb_overlay_flash(uint8_t hid, bool on)
{
    ARG_UNUSED(hid);
    ARG_UNUSED(on);
}
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event)
{
    ARG_UNUSED(binding);
    ARG_UNUSED(event);

    /* Match ZMK's other toggle behaviors by acting on key press. */
    bool now_on = !zmk_usb_studio_is_on();

    zmk_usb_request_studio(now_on);
    g4b_rgb_overlay_flash(0x16u /* S */, now_on);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event)
{
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int behavior_studio_init(const struct device *dev)
{
    ARG_UNUSED(dev);
    return 0;
}

static const struct behavior_driver_api behavior_studio_driver_api = {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    /* Studio requires explicit empty metadata for a zero-parameter behavior. */
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
    .binding_pressed = on_pressed,
    .binding_released = on_released,
};

BEHAVIOR_DT_INST_DEFINE(0, behavior_studio_init, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_studio_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY */
