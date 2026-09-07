/* SPDX-License-Identifier: MIT */
#include "gamepad.h"
#include "apex_radio_input.h"
#include <zephyr/kernel.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/drivers/usb/udc_buf.h>
#include <zephyr/shell/shell.h>

static const struct device *const hid = DEVICE_DT_GET(DT_NODELABEL(receiver_gamepad));
static const uint8_t descriptor[] = { APEX_GAMEPAD_DESCRIPTOR };
static struct k_spinlock lock;
static uint8_t current[APEX_GAMEPAD_REPORT_SIZE];
static bool ready, busy, dirty, enabled;
static bool live;
static uint32_t received_at;
static uint32_t reports, errors;
UDC_STATIC_BUF_DEFINE(tx, APEX_GAMEPAD_REPORT_SIZE);

void apex_radio_gamepad_receive(bool on, const uint8_t *report)
{
    k_spinlock_key_t key = k_spin_lock(&lock);
    enabled = on;
    live = on;
    received_at = k_uptime_get_32();
    if (on) memcpy(current, report, sizeof(current));
    else apex_gamepad_neutral(current);
    dirty = true;
    k_spin_unlock(&lock, key);
    receiver_usb_gamepad(on);
}

void receiver_gamepad_release(void)
{
    k_spinlock_key_t key = k_spin_lock(&lock);
    live = false;
    apex_gamepad_neutral(current);
    dirty = true;
    k_spin_unlock(&lock, key);
}

static void iface_ready(const struct device *dev, bool on)
{
    ARG_UNUSED(dev);
    k_spinlock_key_t key = k_spin_lock(&lock);
    ready = on;
    dirty = true;
    k_spin_unlock(&lock, key);
}

static void report_done(const struct device *dev, const uint8_t *const report, int status)
{
    ARG_UNUSED(dev); ARG_UNUSED(report);
    k_spinlock_key_t key = k_spin_lock(&lock);
    busy = false;
    if (status) { errors++; dirty = true; }
    else reports++;
    k_spin_unlock(&lock, key);
}

static int get_report(const struct device *dev, uint8_t type, uint8_t id,
                      uint16_t length, uint8_t *const report)
{
    ARG_UNUSED(dev);
    if (type != HID_REPORT_TYPE_INPUT || id || length < sizeof(current)) return -ENOTSUP;
    k_spinlock_key_t key = k_spin_lock(&lock);
    memcpy(report, current, sizeof(current));
    k_spin_unlock(&lock, key);
    return sizeof(current);
}

static const struct hid_device_ops ops = {
    .iface_ready = iface_ready,
    .get_report = get_report,
    .input_report_complete = report_done,
};

void receiver_gamepad_poll(void)
{
    k_spinlock_key_t key = k_spin_lock(&lock);
    if (live && k_uptime_get_32() - received_at >= 100) {
        apex_gamepad_neutral(current);
        dirty = true;
        live = false;
    }
    if (!ready || busy || !dirty) { k_spin_unlock(&lock, key); return; }
    memcpy(tx, current, sizeof(current));
    busy = true;
    dirty = false;
    k_spin_unlock(&lock, key);
    int rc = hid_device_submit_report(hid, sizeof(current), tx);
    if (rc) {
        key = k_spin_lock(&lock);
        busy = false;
        dirty = true;
        errors++;
        k_spin_unlock(&lock, key);
    }
}

int receiver_gamepad_init(void)
{
    apex_gamepad_neutral(current);
    return hid_device_register(hid, descriptor, sizeof(descriptor), &ops);
}

void receiver_gamepad_status(const struct shell *sh)
{
    k_spinlock_key_t key = k_spin_lock(&lock);
    bool e = enabled, r = ready, b = busy;
    uint32_t n = reports, err = errors;
    k_spin_unlock(&lock, key);
    shell_print(sh, "GAMEPAD enabled=%u ready=%u busy=%u reports=%u errors=%u", e, r, b, n, err);
}
