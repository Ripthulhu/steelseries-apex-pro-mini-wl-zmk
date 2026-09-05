/* SPDX-License-Identifier: MIT
 *
 * Legacy vendor-HID recovery prototype. Release builds disable this module and
 * use the dedicated CDC trigger in dfu_trigger_g4b.c.
 *
 * This follows the Arctis Nova Pro Omni's recovery model. That transmitter
 * reboots when the host sends one "begin" frame to a vendor HID
 * command interface (usage page 0xFFC0), verified live by USB re-enumeration.
 * The devices use different processors and protocols, so this implementation
 * defines a small vendor HID interface. One exact Feature report calls the same
 * g4b_enter_recovery() path as Fn+Right Ctrl+Esc.
 *
 * This is HID_2, after the keyboard (HID_0) and gamepad (HID_1). It uses a
 * vendor Feature report instead of the current CDC-ACM trigger. The key
 * combination remains available when USB enumeration fails.
 */

#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>

#include "recovery_g4b.h"

/* One vendor-defined application collection on its own interface (no report ID).
 * A 1-byte Input keeps the class-mandated interrupt IN endpoint carrying a valid
 * report; it is never written. The 8-byte Feature report is the command channel,
 * received over the control endpoint via SET_REPORT - no OUT endpoint needed.
 */
static const uint8_t g4b_recovery_desc[] = {
    0x06, 0x00, 0xFF,       /* Usage Page (Vendor-Defined 0xFF00)          */
    0x09, 0x01,             /* Usage (0x01)                                */
    0xA1, 0x01,             /* Collection (Application)                    */
    0x09, 0x03,             /*   Usage (0x03) - status input, unused       */
    0x15, 0x00,             /*   Logical Minimum (0)                       */
    0x26, 0xFF, 0x00,       /*   Logical Maximum (255)                     */
    0x75, 0x08,             /*   Report Size (8)                           */
    0x95, 0x01,             /*   Report Count (1)                          */
    0x81, 0x02,             /*   Input (Data,Var,Abs)                      */
    0x09, 0x02,             /*   Usage (0x02) - command feature            */
    0x95, 0x08,             /*   Report Count (8)                          */
    0xB1, 0x02,             /*   Feature (Data,Var,Abs)                    */
    0xC0,                   /* End Collection                              */
};

/* The exact 8-byte payload that means "enter recovery". Deliberately long and
 * distinctive so a stray SET_REPORT to this interface cannot trip it; only a
 * host that means it, sending these bytes, reaches g4b_enter_recovery(). Keep in
 * step with MAGIC in the matching host-side recovery tool.
 */
#define G4B_RECOVERY_CMD_LEN 8u
static const uint8_t g4b_recovery_magic[G4B_RECOVERY_CMD_LEN] = {
    'A', 'P', 'X', 'R', 'E', 'C', 'O', 'V'
};

static void recovery_reset_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(recovery_reset_work, recovery_reset_fn);

static void recovery_reset_fn(struct k_work *work)
{
    ARG_UNUSED(work);
    g4b_enter_recovery(); /* writes GPREGRET2 and resets; does not return */
}

static bool recovery_cmd_match(const uint8_t *buf, int32_t len)
{
    if (len >= (int32_t)G4B_RECOVERY_CMD_LEN &&
        memcmp(buf, g4b_recovery_magic, G4B_RECOVERY_CMD_LEN) == 0) {
        return true;
    }
    /* hidapi prepends a report-ID byte; with no report IDs it is 0x00, and some
     * hosts pass it into the data stage rather than folding it into wValue.
     * Accept the magic one byte in so the host side is not fussy about it.
     */
    if (len >= (int32_t)(G4B_RECOVERY_CMD_LEN + 1u) && buf[0] == 0x00u &&
        memcmp(buf + 1, g4b_recovery_magic, G4B_RECOVERY_CMD_LEN) == 0) {
        return true;
    }
    return false;
}

static int recovery_set_report(const struct device *dev,
                               struct usb_setup_packet *setup,
                               int32_t *len, uint8_t **data)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(setup);

    if (*data != NULL && recovery_cmd_match(*data, *len)) {
        /* Let the SET_REPORT status stage complete before resetting. */
        k_work_schedule(&recovery_reset_work, K_MSEC(50));
    }
    return 0;
}

/* Only set_report - no IN reports are sent, so int_in_ready is not needed and
 * the IN endpoint simply stays idle. No int_out_ready either: the command comes
 * over the control endpoint, so this interface needs no interrupt OUT endpoint.
 */
static const struct hid_ops recovery_ops = {
    .set_report = recovery_set_report,
};

static int g4b_recovery_usb_init(void)
{
    const struct device *dev = device_get_binding("HID_2");

    if (dev == NULL) {
        /* Recovery HID is optional; the key and mode-switch paths remain. */
        return 0;
    }

    usb_hid_register_device(dev, g4b_recovery_desc, sizeof(g4b_recovery_desc),
                            &recovery_ops);
    usb_hid_init(dev);
    return 0;
}

/* 94, alongside the gamepad and one below ZMK's own HID init (95), so this
 * registers before the USB stack builds its configuration descriptor at
 * usb_enable() (96). The host finds this interface by its 0xFF00 usage page, not
 * by interface number, so the order relative to the gamepad does not matter.
 */
SYS_INIT(g4b_recovery_usb_init, APPLICATION, 94);
