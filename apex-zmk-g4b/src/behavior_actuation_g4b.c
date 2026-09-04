/* SPDX-License-Identifier: MIT
 *
 * Parameterized keymap behavior for actuation, rapid trigger, and custom
 * lighting controls. It is compiled for every board profile so the shared
 * keymap remains valid; profiles without the backing features use stubs.
 */

#define DT_DRV_COMPAT apex_behavior_actuation

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>

#include "actuation_g4b.h"
#include "rgb_fx_g4b.h"
#if IS_ENABLED(CONFIG_APEX_G4B_SW_RECOVERY)
#include "recovery_g4b.h"
#endif

/* Binding parameter values. Kept in step with the dt-binding header the keymap
 * includes; a mismatch here would silently bind the wrong action to a key.
 */
#define G4B_ACT_DEEPER    0
#define G4B_ACT_SHALLOWER 1
#define G4B_ACT_RT_UP     2
#define G4B_ACT_RT_DOWN   3
#define G4B_ACT_RT_TOGGLE 4
#define G4B_ACT_RESET     5
#define G4B_ACT_FX_NEXT   6
#define G4B_ACT_RECOVERY  7

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event)
{
    ARG_UNUSED(event);

    /* One action per press; repeated scanner configuration would contend with
     * the key-report path. */
    switch (binding->param1) {
    case G4B_ACT_DEEPER:
        g4b_actuation_step(+1);
        break;
    case G4B_ACT_SHALLOWER:
        g4b_actuation_step(-1);
        break;
    case G4B_ACT_RT_UP:
        g4b_rapid_trigger_step(+1);
        break;
    case G4B_ACT_RT_DOWN:
        g4b_rapid_trigger_step(-1);
        break;
    case G4B_ACT_RT_TOGGLE:
        g4b_rapid_trigger_toggle();
        break;
    case G4B_ACT_RESET:
        g4b_reset_point_cycle();
        break;
    case G4B_ACT_FX_NEXT:
        g4b_fx_cycle();
        g4b_settings_mark_dirty();
        break;
#if IS_ENABLED(CONFIG_APEX_G4B_SW_RECOVERY)
    case G4B_ACT_RECOVERY:
        g4b_enter_recovery(); /* does not return if the loader honours it */
        break;
#endif
    default:
        /* An out-of-range parameter is a keymap bug, not a runtime condition.
         * Do nothing rather than guess which control was meant.
         */
        break;
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event)
{
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return ZMK_BEHAVIOR_OPAQUE;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
/* Studio needs every accepted parameter listed explicitly. */
static const struct behavior_parameter_value_metadata act_values[] = {
    {
        .display_name = "Actuation deeper",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = G4B_ACT_DEEPER,
    },
    {
        .display_name = "Actuation shallower",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = G4B_ACT_SHALLOWER,
    },
    {
        .display_name = "Rapid trigger less sensitive",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = G4B_ACT_RT_UP,
    },
    {
        .display_name = "Rapid trigger more sensitive",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = G4B_ACT_RT_DOWN,
    },
    {
        .display_name = "Rapid trigger on/off",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = G4B_ACT_RT_TOGGLE,
    },
    {
        .display_name = "Cycle reset point",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = G4B_ACT_RESET,
    },
    {
        .display_name = "Next lighting effect",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = G4B_ACT_FX_NEXT,
    },
#if IS_ENABLED(CONFIG_APEX_G4B_SW_RECOVERY)
    {
        .display_name = "Enter recovery (bootloader)",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = G4B_ACT_RECOVERY,
    },
#endif
};

static const struct behavior_parameter_metadata_set act_set = {
    .param1_values = act_values,
    .param1_values_len = ARRAY_SIZE(act_values),
};

static const struct behavior_parameter_metadata_set act_sets[] = { act_set };

static const struct behavior_parameter_metadata act_metadata = {
    .sets_len = ARRAY_SIZE(act_sets),
    .sets = act_sets,
};
#endif /* CONFIG_ZMK_BEHAVIOR_METADATA */

static const struct behavior_driver_api behavior_actuation_api = {
    .binding_pressed = on_pressed,
    .binding_released = on_released,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &act_metadata,
#endif
};

static int behavior_actuation_init(const struct device *dev)
{
    ARG_UNUSED(dev);
    return 0;
}

BEHAVIOR_DT_INST_DEFINE(0, behavior_actuation_init, NULL, NULL, NULL,
                        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_actuation_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY */
