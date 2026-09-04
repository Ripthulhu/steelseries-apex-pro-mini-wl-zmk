/* SPDX-License-Identifier: MIT
 *
 * Keymap behavior for toggling the optional USB analog gamepad with Fn+Z.
 * The behavior is always compiled so the shared keymap remains valid. When
 * CONFIG_APEX_G4B_GAMEPAD is disabled, its backing calls are no-ops.
 */

#define DT_DRV_COMPAT apex_behavior_analog_toggle

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>

#include "actuation_g4b.h"

#if IS_ENABLED(CONFIG_APEX_G4B_GAMEPAD)
#include "gamepad_g4b.h"
#else
static inline bool g4b_gamepad_is_enabled(void) { return false; }
static inline void g4b_gamepad_set_enabled(bool on) { ARG_UNUSED(on); }
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event)
{
    ARG_UNUSED(binding);
    ARG_UNUSED(event);

    /* Match ZMK's other toggle behaviors by acting on key press. */
    g4b_gamepad_set_enabled(!g4b_gamepad_is_enabled());
    g4b_settings_mark_dirty();
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event)
{
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int behavior_analog_init(const struct device *dev)
{
    ARG_UNUSED(dev);
    return 0;
}

static const struct behavior_driver_api behavior_analog_driver_api = {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    /* Studio requires explicit empty metadata for a zero-parameter behavior. */
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
    .binding_pressed = on_pressed,
    .binding_released = on_released,
};

BEHAVIOR_DT_INST_DEFINE(0, behavior_analog_init, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_analog_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY */
