/* SPDX-License-Identifier: MIT
 *
 * USB HID gamepad using Hall-effect key depth. The horizontal axis is the
 * difference between D and A depth. It is exposed as the hid_gamepad devicetree
 * node and does not alter the keyboard HID descriptor. Generic HID gamepads are
 * supported by DirectInput and RawInput, but not XInput. BLE gamepad reports are
 * not implemented.
 *
 * Based on the MIT-licensed HID work in:
 * https://github.com/cr3eperall/zmk-feature-hall-effect
 * https://github.com/badjeff/zmk-hid-io
 */

#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/drivers/usb/udc_buf.h>

#include "gamepad_g4b.h"

/* One application collection on its own interface, so no report ID is needed.
 * Five 16-bit unsigned axes then eight buttons: 5*2 + 1 = 11 bytes.
 *   X  steering, D - A, centred        (a left-stick X)
 *   Y  combined  S - W, centred        (a left-stick Y)
 *   Z  throttle, W only, rests at 0    (a right trigger)
 *   Rz brake,    S only, rests at 0    (a left trigger)
 *   Rx steering, D - A, centred        (a right-stick X - the SAME value as X)
 * Rx exists because some games want on-foot steering and vehicle steering on
 * different sticks (Cyberpunk's driving workaround is the well-known case), and
 * a host mapper cannot send one physical axis to two sticks. Exposing the
 * steering value on a second axis lets the mapping stay entirely host-side and
 * game-specific - the firmware itself knows nothing about any game.
 * Unsigned rather than signed because Windows maps logical-min..max onto its
 * own range either way, and unsigned avoids sign-extension mistakes in
 * third-party mapping tools.
 */
static const uint8_t g4b_gamepad_desc[] = {
    0x05, 0x01,             /* Usage Page (Generic Desktop)   */
    0x09, 0x05,             /* Usage (Game Pad)               */
    0xA1, 0x01,             /* Collection (Application)       */
    0xA1, 0x00,             /*   Collection (Physical)        */
    0x09, 0x30,             /*     Usage (X)                  */
    0x09, 0x31,             /*     Usage (Y)                  */
    0x09, 0x32,             /*     Usage (Z)                  */
    0x09, 0x35,             /*     Usage (Rz)                 */
    0x09, 0x33,             /*     Usage (Rx)                 */
    0x15, 0x00,             /*     Logical Minimum (0)        */
    0x26, 0xFF, 0x7F,       /*     Logical Maximum (32767)    */
    0x75, 0x10,             /*     Report Size (16)           */
    0x95, 0x05,             /*     Report Count (5)           */
    0x81, 0x02,             /*     Input (Data,Var,Abs)       */
    0xC0,                   /*   End Collection               */
    0x05, 0x09,             /*   Usage Page (Button)          */
    0x19, 0x01,             /*   Usage Minimum (1)            */
    0x29, 0x08,             /*   Usage Maximum (8)            */
    0x15, 0x00,             /*   Logical Minimum (0)          */
    0x25, 0x01,             /*   Logical Maximum (1)          */
    0x75, 0x01,             /*   Report Size (1)              */
    0x95, 0x08,             /*   Report Count (8)             */
    0x81, 0x02,             /*   Input (Data,Var,Abs)         */
    0xC0,                   /* End Collection                 */
};

#define G4B_GP_REPORT_BYTES 11u

static const struct device *gp_dev;
static K_SEM_DEFINE(gp_sem, 1, 1);
static struct k_spinlock gp_lock;
static uint8_t gp_published[G4B_GP_REPORT_BYTES];
static void gp_work_fn(struct k_work *work);
static K_WORK_DEFINE(gp_work, gp_work_fn);

/* hid_device_submit_report() requires a UDC-aligned buffer; the staged report
 * lives in a plain array, so bounce through this. Only ever touched under
 * gp_sem (one in-flight submit at a time), so no extra lock needed. */
UDC_STATIC_BUF_DEFINE(gp_txbuf, G4B_GP_REPORT_BYTES);

/* Diagnostics, read by the evidence record. A dead axis and a busy endpoint
 * look identical from outside otherwise.
 */
uint32_t g4b_gp_writes;
uint32_t g4b_gp_busy;
uint32_t g4b_gp_err;

/* input_report_done makes hid_device_submit_report() asynchronous: it returns
 * as soon as the transfer is queued and calls this when the IN transfer
 * completes. Releasing the semaphore here is what lets the next frame go - the
 * same drop-on-busy rate limit as the legacy .int_in_ready. */
static void gp_in_done(const struct device *dev, const uint8_t *const report)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(report);
    k_sem_give(&gp_sem);
}

/* The next-stack HID class REQUIRES a get_report callback for every device
 * (hid_device_register() returns -EINVAL without it, leaving ddata->ops
 * unset -> usbd_hid_enable() later calls a garbage pointer -> UsageFault).
 * The gamepad only streams state over the interrupt-IN pipe and has no host
 * GET_REPORT use, so this just refuses politely. */
static int gp_get_report(const struct device *dev, const uint8_t type, const uint8_t id,
                         const uint16_t len, uint8_t *const buf)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(type);
    ARG_UNUSED(id);
    ARG_UNUSED(len);
    ARG_UNUSED(buf);
    return -ENOTSUP;
}

static const struct hid_device_ops gp_ops = {
    .get_report = gp_get_report,
    .input_report_done = gp_in_done,
};

static void gp_work_fn(struct k_work *work)
{
    k_spinlock_key_t key;
    int rc;

    ARG_UNUSED(work);

    if (gp_dev == NULL) {
        /* DEVICE_DT_GET(hid_gamepad) unavailable at init - the gamepad node
         * isn't in the build. Count it, or all three counters stay at zero
         * forever and that reads as "nothing was ever published", which is
         * false: publish ran, the work item was submitted, and this handler
         * executed. Same thread as the busy/err increments below, so no race.
         */
        g4b_gp_err++;
        return;
    }
    /* K_NO_WAIT, never a timeout. If the previous submit hasn't completed the
     * frame is simply dropped - the next sample is along in a few milliseconds
     * and carries the current position anyway, because this is absolute state
     * rather than a delta. Waiting here would let a stalled host back up into
     * the caller. The semaphore is released in gp_in_done().
     */
    if (k_sem_take(&gp_sem, K_NO_WAIT) != 0) {
        g4b_gp_busy++;
        return;
    }

    key = k_spin_lock(&gp_lock);
    memcpy(gp_txbuf, gp_published, G4B_GP_REPORT_BYTES);
    k_spin_unlock(&gp_lock, key);

    rc = hid_device_submit_report(gp_dev, G4B_GP_REPORT_BYTES, gp_txbuf);
    if (rc != 0) {
        /* -EAGAIN/-ENODEV while unconfigured or suspended is normal, not a
         * fault. gp_in_done() will NOT fire for a submit that never queued, so
         * release the semaphore here to avoid a permanent stall. */
        g4b_gp_err++;
        k_sem_give(&gp_sem);
        return;
    }
    g4b_gp_writes++;
}

/* Off by default. A gamepad that is always live is not free: some games grab
 * the first controller they enumerate and then ignore the keyboard, or refuse to
 * start while an unexpected controller is present. So the axis is opt-in, toggled
 * from the keymap - see behavior_analog_g4b.c, bound to Fn+Z.
 *
 * zmk_usb_request_gamepad() rebuilds the USB configuration on a work item, so
 * the host re-enumerates with hid_1 present or absent. A game sees no controller
 * at all while the feature is disabled.
 */
extern void zmk_usb_request_gamepad(bool on); /* app/src/usb.c */

static bool gp_enabled;

bool g4b_gamepad_is_enabled(void)
{
    return gp_enabled;
}

void g4b_gamepad_set_enabled(bool on)
{
    if (on == gp_enabled) {
        return;
    }
    gp_enabled = on;
    if (on) {
        /* A report that was mid-submit when the interface was last torn down can
         * leave gp_sem taken - its completion callback never fires for a transfer
         * the stack cancelled. Reset it to "free" so the first frame after
         * re-enumeration isn't dropped forever (k_sem_give caps at the max of 1,
         * so a late completion can't overflow it). */
        k_sem_reset(&gp_sem);
        k_sem_give(&gp_sem);
    }
    /* Add or remove the gamepad USB interface. The teardown/rebuild + host
     * re-enumeration runs on a work item in usb.c; no centred report is needed
     * because the interface itself disappears when off. */
    zmk_usb_request_gamepad(on);
}

void g4b_gamepad_publish(uint16_t x, uint16_t y, uint16_t z, uint16_t rz,
                         uint16_t rx, uint8_t buttons)
{
    k_spinlock_key_t key;

    if (!gp_enabled) {
        return;
    }
    key = k_spin_lock(&gp_lock);

    gp_published[0] = (uint8_t)(x & 0xFFu);
    gp_published[1] = (uint8_t)(x >> 8);
    gp_published[2] = (uint8_t)(y & 0xFFu);
    gp_published[3] = (uint8_t)(y >> 8);
    gp_published[4] = (uint8_t)(z & 0xFFu);
    gp_published[5] = (uint8_t)(z >> 8);
    gp_published[6] = (uint8_t)(rz & 0xFFu);
    gp_published[7] = (uint8_t)(rz >> 8);
    gp_published[8] = (uint8_t)(rx & 0xFFu);
    gp_published[9] = (uint8_t)(rx >> 8);
    gp_published[10] = buttons;
    k_spin_unlock(&gp_lock, key);

    /* Submitting an already-pending work item is a no-op - a free rate limit:
     * however fast the caller publishes, at most one report is queued at a time.
     */
    k_work_submit(&gp_work);
}

static int g4b_gamepad_init(void)
{
    gp_dev = DEVICE_DT_GET(DT_NODELABEL(hid_gamepad));
    if (!device_is_ready(gp_dev)) {
        /* The hid_gamepad node isn't in this build. Nothing else in the payload
         * depends on it, so fail quietly rather than take the keyboard down. */
        gp_dev = NULL;
        return 0;
    }

    hid_device_register(gp_dev, g4b_gamepad_desc, sizeof(g4b_gamepad_desc),
                        &gp_ops);

    /* Centre the staged report so the very first thing sent after the axis is
     * switched on is neutral, not hard-left. Written directly because publish()
     * is gated on gp_enabled, which is false here by design.
     */
    gp_published[0] = (uint8_t)(G4B_GP_CENTRE & 0xFFu);
    gp_published[1] = (uint8_t)(G4B_GP_CENTRE >> 8);
    gp_published[2] = gp_published[0];
    gp_published[3] = gp_published[1];
    return 0;
}

/* 94, deliberately one below ZMK's own HID init (CONFIG_ZMK_USB_HID_INIT_PRIORITY
 * defaults to 95) and safely below usb_enable() at 96. Registration has to land
 * before the USB stack builds its configuration descriptor, and sharing a
 * priority with ZMK would make the order depend on link order.
 */
SYS_INIT(g4b_gamepad_init, APPLICATION, 94);
