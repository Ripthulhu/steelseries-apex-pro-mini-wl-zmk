/* SPDX-License-Identifier: MIT
 *
 * A keymap behavior that reboots into the Adafruit_nRF52_Bootloader (UF2/serial
 * DFU). Bound via a combo to Fn + Escape + Right-Ctrl (see the keymap).
 *
 * Writes the Adafruit UF2 DFU magic (0x57) to NRF_POWER->GPREGRET and resets.
 * Done directly rather than via ZMK's &bootloader / sys_reboot(RST_UF2) because
 * this ZMK/Zephyr tree does NOT translate the reboot type to GPREGRET:
 * CONFIG_NRF_STORE_REBOOT_TYPE_GPREGRET is an unused Kconfig symbol here and
 * sys_arch_reboot() is the __weak stub that ignores its argument. GPREGRET
 * (0x4000051C) is exactly what the bootloader's check_dfu_mode() reads.
 * Same mechanism as the USB DFU triggers in src/dfu_trigger_g4b.c.
 */

#define DT_DRV_COMPAT apex_behavior_dfu

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#include <nrfx.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event)
{
    ARG_UNUSED(binding);
    ARG_UNUSED(event);

    NRF_POWER->GPREGRET = 0x57u; /* DFU_MAGIC_UF2_RESET */
    __DSB();
    NVIC_SystemReset();
    return ZMK_BEHAVIOR_OPAQUE; /* not reached */
}

static int on_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event)
{
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_dfu_driver_api = {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
    .binding_pressed = on_pressed,
    .binding_released = on_released,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_dfu_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY */
