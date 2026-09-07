/* SPDX-License-Identifier: MIT */
#include "apex_radio_input.h"
#include "apex_latency.h"
#include "gamepad.h"
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/drivers/usb/udc_buf.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>

/* Report bodies match v0.1.4: six keyboard usages and six 16-bit consumer usages. */
static const uint8_t descriptor[] = {
    0x05,0x01, 0x09,0x06, 0xa1,0x01, 0x85,0x01,
    0x05,0x07, 0x19,0xe0, 0x29,0xe7, 0x15,0x00, 0x25,0x01,
    0x75,0x01, 0x95,0x08, 0x81,0x02,
    0x75,0x08, 0x95,0x01, 0x81,0x03,
    0x05,0x08, 0x19,0x01, 0x29,0x05, 0x75,0x01, 0x95,0x05, 0x91,0x02,
    0x75,0x03, 0x95,0x01, 0x91,0x03,
    0x05,0x07, 0x19,0x00, 0x29,0xff, 0x15,0x00, 0x26,0xff,0x00,
    0x75,0x08, 0x95,0x06, 0x81,0x00, 0xc0,
    0x05,0x0c, 0x09,0x01, 0xa1,0x01, 0x85,0x02,
    0x15,0x00, 0x26,0xff,0x0f, 0x19,0x00, 0x2a,0xff,0x0f,
    0x75,0x10, 0x95,0x06, 0x81,0x00, 0xc0,
};
static const struct device *const hid = DEVICE_DT_GET(DT_NODELABEL(receiver_hid));
UDC_STATIC_BUF_DEFINE(report, 16);
static struct k_spinlock lock;
static bool ready, busy;
static uint8_t release_mask = 3, protocol = HID_PROTOCOL_REPORT;
static uint32_t generation, pending_generation, pending_sequence, delivered_sequence;
static uint8_t pending_type;
static uint8_t keyboard[8], consumer[12];
static atomic_t leds;
static int64_t hold_until;
static uint32_t completed[2], releases, submit_errors, transfer_errors;
static uint16_t last_media_usage;
static uint32_t submitted_at, delivered_at, usb_max_us;
static struct apex_latency usb_latency;
static struct apex_latency usb_interval;
static uint32_t last_usb_at, last_usb_generation;
static bool have_usb_completion;
static struct apex_delivery_rx delivery;
static K_SEM_DEFINE(hid_event, 0, 1);

void receiver_hid_wait(void)
{
    /* Retry temporary submission failures even if USB produces no callback. */
    k_sem_take(&hid_event, K_MSEC(1));
}

void apex_radio_delivery_status(struct apex_delivery_ack *ack)
{
    k_spinlock_key_t key = k_spin_lock(&lock);
    apex_delivery_status(&delivery, atomic_get(&leds), ack);
    k_spin_unlock(&lock, key);
}

uint32_t apex_radio_completed(uint32_t *completed_at)
{
    k_spinlock_key_t key = k_spin_lock(&lock);
    uint32_t sequence = release_mask ? 0 : delivered_sequence;
    *completed_at = delivered_at;
    k_spin_unlock(&lock, key);
    return sequence;
}

void apex_radio_release(void)
{
    k_spinlock_key_t key = k_spin_lock(&lock);
    generation++;
    releases++;
    delivered_sequence = 0;
    apex_delivery_rx_reset(&delivery);
    release_mask = 3;
    memset(keyboard, 0, sizeof(keyboard));
    memset(consumer, 0, sizeof(consumer));
    k_spin_unlock(&lock, key);
    receiver_gamepad_release();
    k_sem_give(&hid_event);
}

uint8_t apex_radio_host_leds(void) { return atomic_get(&leds); }

static void iface_ready(const struct device *dev, bool is_ready)
{
    ARG_UNUSED(dev);
    k_spinlock_key_t key = k_spin_lock(&lock);
    ready = is_ready;
    k_spin_unlock(&lock, key);
    apex_radio_release();
    apex_radio_request_session();
}

static void report_done(const struct device *dev, const uint8_t *data, int status)
{
    ARG_UNUSED(dev); ARG_UNUSED(data);
    bool notify = false;
    k_spinlock_key_t key = k_spin_lock(&lock);
    if (status) transfer_errors++;
    if (!status && busy && pending_generation == generation) {
        delivered_sequence = pending_sequence;
        if (pending_sequence) {
            apex_delivery_complete(&delivery, pending_sequence);
            delivered_at = k_cycle_get_32();
            if (have_usb_completion && last_usb_generation == pending_generation)
                apex_latency_add(&usb_interval, k_cyc_to_us_floor32(delivered_at - last_usb_at));
            last_usb_at = delivered_at;
            last_usb_generation = pending_generation;
            have_usb_completion = true;
            uint32_t elapsed = k_cyc_to_us_floor32(delivered_at - submitted_at);
            apex_latency_add(&usb_latency, elapsed);
            if (elapsed > usb_max_us) usb_max_us = elapsed;
            notify = true;
            completed[pending_type - 1]++;
            if (pending_type == APEX_INPUT_CONSUMER && sys_get_le16(consumer)) {
                last_media_usage = sys_get_le16(consumer);
            }
        }
        if (!pending_sequence) release_mask &= ~(1u << (pending_type - 1));
    }
    busy = false;
    k_spin_unlock(&lock, key);
    k_sem_give(&hid_event);
    if (notify) apex_radio_delivery_notify();
}

static void set_protocol(const struct device *dev, uint8_t value)
{
    ARG_UNUSED(dev);
    k_spinlock_key_t key = k_spin_lock(&lock);
    protocol = value;
    k_spin_unlock(&lock, key);
    apex_radio_release();
    apex_radio_request_session();
}

static int get_report(const struct device *dev, uint8_t type, uint8_t id,
                      uint16_t length, uint8_t *buf)
{
    ARG_UNUSED(dev);
    k_spinlock_key_t key = k_spin_lock(&lock);
    bool boot = protocol == HID_PROTOCOL_BOOT;
    if (boot && id == 0) id = APEX_INPUT_KEYBOARD;
    if (type != HID_REPORT_TYPE_INPUT || !apex_input_size(id)) {
        k_spin_unlock(&lock, key);
        return -ENOTSUP;
    }
    size_t n = apex_input_size(id), prefix = boot ? 0 : 1;
    if (length < n + prefix || (boot && id != APEX_INPUT_KEYBOARD)) {
        k_spin_unlock(&lock, key);
        return -EINVAL;
    }
    if (prefix) buf[0] = id;
    memcpy(buf + prefix, id == APEX_INPUT_KEYBOARD ? keyboard : consumer, n);
    k_spin_unlock(&lock, key);
    return n + prefix;
}

static int set_report(const struct device *dev, uint8_t type, uint8_t id,
                      uint16_t length, const uint8_t *buf)
{
    ARG_UNUSED(dev);
    if (type != HID_REPORT_TYPE_OUTPUT || (id != 1 && id != 0) || length != 1) return -EINVAL;
    atomic_set(&leds, buf[0] & 0x1f);
    return 0;
}

static const struct hid_device_ops ops = {
    .iface_ready = iface_ready, .input_report_complete = report_done,
    .get_report = get_report, .set_report = set_report, .set_protocol = set_protocol,
};

static int submit(uint32_t sequence, uint8_t type, const uint8_t *data, uint32_t expected_generation)
{
    k_spinlock_key_t key = k_spin_lock(&lock);
    if (expected_generation != generation) {
        k_spin_unlock(&lock, key);
        return -ECANCELED;
    }
    if (k_uptime_get() < hold_until) {
        k_spin_unlock(&lock, key);
        return -EAGAIN;
    }
    if (sequence && sequence == delivered_sequence && !release_mask) {
        k_spin_unlock(&lock, key);
        return 0;
    }
    if (!ready || busy || (sequence && release_mask)) {
        k_spin_unlock(&lock, key);
        return -EAGAIN;
    }
    if (protocol == HID_PROTOCOL_BOOT && type == APEX_INPUT_CONSUMER) {
        if (!sequence) release_mask &= ~2u;
        else {
            delivered_sequence = sequence;
            delivered_at = k_cycle_get_32();
            apex_delivery_complete(&delivery, sequence);
        }
        k_spin_unlock(&lock, key);
        k_sem_give(&hid_event);
        apex_radio_delivery_notify();
        return 0;
    }
    size_t n = apex_input_size(type), prefix = protocol == HID_PROTOCOL_BOOT ? 0 : 1;
    if (prefix) report[0] = type;
    memcpy(report + prefix, data, n);
    memcpy(type == APEX_INPUT_KEYBOARD ? keyboard : consumer, data, n);
    pending_sequence = sequence;
    pending_type = type;
    pending_generation = generation;
    submitted_at = k_cycle_get_32();
    busy = true;
    k_spin_unlock(&lock, key);
    int rc = hid_device_submit_report(hid, n + prefix, report);
    if (rc) {
        key = k_spin_lock(&lock);
        submit_errors++;
        busy = false;
        k_spin_unlock(&lock, key);
    }
    return rc ? rc : -EAGAIN;
}

int apex_radio_deliver(const struct apex_input_frame *frame)
{
    k_spinlock_key_t key = k_spin_lock(&lock);
    int rc = apex_delivery_receive(&delivery, frame);
    k_spin_unlock(&lock, key);
    if (!rc) k_sem_give(&hid_event);
    return rc == -2 ? -EAGAIN : rc < 0 ? -EINVAL : 0;
}

void receiver_hid_poll(void)
{
    static const uint8_t zero[12];
    k_spinlock_key_t key = k_spin_lock(&lock);
    uint8_t type = release_mask & 1 ? APEX_INPUT_KEYBOARD :
                   release_mask & 2 ? APEX_INPUT_CONSUMER : 0;
    uint32_t current_generation = generation;
    struct apex_input_frame frame;
    bool queued = !type && apex_delivery_front(&delivery, &frame);
    k_spin_unlock(&lock, key);
    if (type) (void)submit(0, type, zero, current_generation);
    else if (queued) (void)submit(frame.sequence, frame.type, frame.data, current_generation);
    receiver_gamepad_poll();
}

int receiver_hid_init(void)
{
    int rc = hid_device_register(hid, descriptor, sizeof(descriptor), &ops);
    return rc ? rc : receiver_gamepad_init();
}

int receiver_hid_benchmark(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    if (!apex_radio_pause()) return -EBUSY;
    apex_radio_release();
    int rc = 0;
    int64_t deadline = k_uptime_get() + 1000;
    for (;;) {
        k_spinlock_key_t key = k_spin_lock(&lock);
        bool available = ready && !busy && !release_mask;
        k_spin_unlock(&lock, key);
        if (available) break;
        if (k_uptime_get() >= deadline) { rc = -ETIMEDOUT; goto done; }
        k_sleep(K_MSEC(1));
    }
    uint32_t sent = 0, complete = 0, started = k_cycle_get_32();
    deadline = k_uptime_get() + 5000;
    while (complete < 1000 && k_uptime_get() < deadline) {
        while (sent < 1000) {
            struct apex_input_frame frame = {.sequence = sent + 1, .type = APEX_INPUT_KEYBOARD};
            int result = apex_radio_deliver(&frame);
            if (result == -EAGAIN) break;
            if (result) { rc = result; goto report; }
            sent++;
        }
        uint32_t completed_at;
        complete = apex_radio_completed(&completed_at);
        if (complete < 1000) k_sleep(K_MSEC(1));
    }
    if (complete < 1000) rc = -ETIMEDOUT;
report:;
    uint32_t elapsed = k_cyc_to_us_floor32(k_cycle_get_32() - started);
    shell_print(sh, "USB_BENCH sent=%u completed=%u elapsed_us=%u reports_per_s=%llu error=%d",
                sent, complete, elapsed, elapsed ? (unsigned long long)complete * 1000000 / elapsed : 0, rc);
done:
    apex_radio_release();
    apex_radio_resume();
    return rc;
}

int receiver_hid_status(const struct shell *sh, size_t argc, char **argv)
{
    receiver_gamepad_status(sh);
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    k_spinlock_key_t key = k_spin_lock(&lock);
    bool r = ready, b = busy;
    uint8_t p = protocol, mask = release_mask;
    uint32_t keys = completed[0], media = completed[1], cleared = releases, errors = submit_errors;
    uint32_t failed = transfer_errors;
    uint32_t usb_us = usb_max_us;
    struct apex_latency latency = usb_latency;
    struct apex_latency interval = usb_interval;
    uint16_t usage = last_media_usage;
    k_spin_unlock(&lock, key);
    shell_print(sh, "HID ready=%u busy=%u protocol=%u release_pending=%u leds=%02lx",
                r, b, p, mask, (unsigned long)atomic_get(&leds));
    shell_print(sh, "HID keyboard=%lu consumer=%lu releases=%lu submit_errors=%lu last_media=%04x",
                (unsigned long)keys, (unsigned long)media, (unsigned long)cleared,
                (unsigned long)errors, usage);
    shell_print(sh, "HID transfer_errors=%lu", (unsigned long)failed);
    shell_print(sh, "HID submit_to_complete_max_us=%lu", (unsigned long)usb_us);
    shell_print(sh, "USB_COMPLETE count=%u min_us=%u max_us=%u total_us=%llu",
                latency.count, latency.min_us, latency.max_us,
                (unsigned long long)latency.total_us);
    shell_print(sh, "USB_COMPLETE buckets=%u,%u,%u,%u,%u,%u",
                latency.buckets[0], latency.buckets[1], latency.buckets[2],
                latency.buckets[3], latency.buckets[4], latency.buckets[5]);
    shell_print(sh, "USB_INTERVAL count=%u min_us=%u max_us=%u total_us=%llu",
                interval.count, interval.min_us, interval.max_us,
                (unsigned long long)interval.total_us);
    shell_print(sh, "USB_INTERVAL buckets=%u,%u,%u,%u,%u,%u",
                interval.buckets[0], interval.buckets[1], interval.buckets[2],
                interval.buckets[3], interval.buckets[4], interval.buckets[5]);
    return 0;
}

int receiver_hid_test_hold(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    /* A bounded USB stall for testing radio liveness independently of delivery. */
    k_spinlock_key_t key = k_spin_lock(&lock);
    hold_until = k_uptime_get() + 500;
    k_spin_unlock(&lock, key);
    apex_radio_release();
    apex_radio_request_session();
    shell_print(sh, "Holding HID submissions for 500 ms; requesting a fresh radio session.");
    return 0;
}
