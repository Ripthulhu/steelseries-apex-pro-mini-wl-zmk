/* SPDX-License-Identifier: MIT
 *
 * Legacy SteelSeries-loader updater prototype. Release builds disable this
 * module and use Adafruit UF2 or serial DFU instead. It exposes a vendor HID
 * interface that speaks the SteelSeries Nordic updater protocol
 * (0x42 erase / 0x43 write-chunk / 0x41 reset) and writes the received image
 * into the external SPI-NOR staged file (fs=3/file=11 at 0x014000). On reset the
 * SteelSeries loader validates that staged image and applies it to the live app
 * slot.
 *
 * WHY A THREAD. A full-region erase is seconds long; running it in the USB
 * control callback would stall enumeration. So the callbacks only enqueue a
 * command and the dedicated updater thread performs every SPIM0 operation - one
 * owner for the bus, and the erase never blocks USB. The host polls a status
 * feature report between commands.
 *
 * SAFETY. g4b_spinor_stage_* only ever touch 0x014000..0x05F000 (the staged
 * firmware) - never the config profiles below or the free tail above, and never
 * the STM32. A bad or interrupted write leaves an invalid staged image, which
 * the SteelSeries loader declines to apply.
 */

#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>

#include <cmsis_core.h>
#include <zephyr/sys/crc.h>

#include "spinor_g4b.h"

/* Version marker: a CRC-32 of the live application image, computed once at
 * init and returned in the status report. The image the loader applies sits at
 * internal 0x1C000 and is exactly the 0x4B000-byte vendor.bin (see the
 * staged-file finding). The LAST 4 bytes of that image are the vendor CRC-32 of
 * everything before them, appended so crc32(whole image) is the fixed self-check
 * residue 0x2144DF1C for every valid build - useless as a fingerprint. So the
 * marker covers the image MINUS that 4-byte trailer: crc32 over [0x1C000,
 * 0x1C000+0x4AFFC). That is precisely the vendor image CRC the loader itself
 * checks (flash_apex_nordic.py prints it), and it varies per build. On the host,
 * reflash.py computes zlib.crc32(vendor.bin[:-4]) to match. Lets reflash.py
 * (a) tell one build from another and (b) verify, after the reboot, that the
 * image it streamed was applied byte-for-byte. crc32_ieee matches zlib/IEEE-
 * 802.3. Fully automatic: any source change changes it, nothing is bumped by
 * hand. */
#define G4B_IMG_BASE 0x0001C000u
#define G4B_IMG_LEN  0x0004AFFCu /* 307200 - 4: the image without its CRC trailer */
#define G4B_MARK_SENTINEL 0x4Du  /* 'M' at rep[1]: this firmware carries a marker */

/* Protocol bytes, matching the vendor updater (and reflash.py). The report is
 * 64 bytes; the command byte is first (the host's report-ID byte is stripped by
 * the stack), then for a write: fs, file, len(u16 LE), off(u32 LE), data. */
#define UPD_CMD_ERASE 0x42u
#define UPD_CMD_WRITE 0x43u
#define UPD_CMD_RESET 0x41u

#if IS_ENABLED(CONFIG_APEX_G4B_AB_ROLLBACK)
/* A/B rollback staging, layered onto the same vendor HID. B is streamed into
 * the NOR tail and the descriptor committed last - see ab_rollback_g4b.c. */
#include "ab_rollback_g4b.h"
#define UPD_CMD_AB_ERASE  0x50u /* erase the whole B slot */
#define UPD_CMD_AB_WRITE  0x51u /* write chunk: same len/off/data shape as 0x43 */
#define UPD_CMD_AB_COMMIT 0x52u /* CRC B and arm the descriptor; b_len in off field */
#endif

/* Status byte returned by the GET_FEATURE poll. */
#define UPD_ST_READY 0x00u /* idle / last command done, ready for the next */
#define UPD_ST_BUSY  0x01u /* a command is in flight on the updater thread */
#define UPD_ST_ERROR 0x02u /* the last SPIM0 operation failed */

#define UPD_REPORT_LEN 64u
#define UPD_MAX_CHUNK  (UPD_REPORT_LEN - 9u) /* data after cmd,fs,file,len,off */

static const struct device *upd_dev;

/* One command in flight at a time; the host polls to READY before the next. */
static struct {
    uint8_t cmd;
    uint16_t len;
    uint32_t off;
    uint8_t data[UPD_MAX_CHUNK];
} upd_mbox;

static volatile uint8_t upd_state = UPD_ST_READY;
static uint32_t upd_marker; /* CRC-32 fingerprint of the running image */
static K_SEM_DEFINE(upd_sem, 0, 1);

/* --- USB HID: one 64-byte Feature report carries both the command (SET_FEATURE)
 * and the status reply (GET_FEATURE). A 1-byte Input keeps the class-mandated IN
 * endpoint valid; it is never written. Vendor usage page 0xFFC0, echoing the
 * stock updater. */
static const uint8_t upd_desc[] = {
    0x06, 0xC0, 0xFF,       /* Usage Page (Vendor 0xFFC0)                  */
    0x09, 0x01,             /* Usage (0x01)                                */
    0xA1, 0x01,             /* Collection (Application)                    */
    0x09, 0x02,             /*   Usage (0x02) - status input (unused)      */
    0x15, 0x00,             /*   Logical Minimum (0)                       */
    0x26, 0xFF, 0x00,       /*   Logical Maximum (255)                     */
    0x75, 0x08,             /*   Report Size (8)                           */
    0x95, 0x01,             /*   Report Count (1)                          */
    0x81, 0x02,             /*   Input (Data,Var,Abs)                      */
    0x09, 0x03,             /*   Usage (0x03) - command/status feature     */
    0x95, UPD_REPORT_LEN,   /*   Report Count (64)                         */
    0xB1, 0x02,             /*   Feature (Data,Var,Abs)                    */
    0xC0,                   /* End Collection                              */
};

/* GET_FEATURE: the host polls this to learn when a command has finished. The
 * next-stack class provides the destination buffer and expects the byte count
 * written (or a negative errno) back. */
static int upd_get_report(const struct device *dev, const uint8_t type, const uint8_t id,
                          const uint16_t len, uint8_t *const buf)
{
    uint8_t rep[UPD_REPORT_LEN];
    uint16_t n;

    ARG_UNUSED(dev);
    ARG_UNUSED(id);
    if (type != HID_REPORT_TYPE_FEATURE) {
        return -ENOTSUP;
    }
    memset(rep, 0, sizeof(rep));
    rep[0] = upd_state;
    /* rep[1] sentinel + rep[2..5] marker (u32 LE). Placed after the status byte
     * so the existing poll (which reads only rep[0]) is undisturbed. */
    rep[1] = G4B_MARK_SENTINEL;
    rep[2] = (uint8_t)(upd_marker);
    rep[3] = (uint8_t)(upd_marker >> 8);
    rep[4] = (uint8_t)(upd_marker >> 16);
    rep[5] = (uint8_t)(upd_marker >> 24);
    n = MIN(len, (uint16_t)sizeof(rep));
    memcpy(buf, rep, n);
    return (int)n;
}

/* SET_FEATURE: parse a command and hand it to the updater thread. Fast - it does
 * no flash access itself, so the control transfer completes at once. The class
 * strips any report ID (passed separately as `id`) and hands the report body in
 * `buf`/`len`; a nonzero return signals an unsupported type or an error. */
static int upd_set_report(const struct device *dev, const uint8_t type, const uint8_t id,
                          const uint16_t len, const uint8_t *const buf)
{
    const uint8_t *d = buf;
    size_t n = len;
    uint16_t clen = 0U;
    uint32_t off = 0U;

    ARG_UNUSED(dev);
    ARG_UNUSED(id);
    if (type != HID_REPORT_TYPE_FEATURE) {
        return -ENOTSUP;
    }
    if (d == NULL || n < 1U) {
        return -EINVAL;
    }
    /* Defensive report-ID skip: the next-stack class strips the ID into `id`, so
     * d[0] is normally the command byte - but no command is 0x00, so if a host
     * still prefixes a 0x00 ID byte this harmlessly absorbs it either way. */
    if (d[0] == 0x00u && n > 1U) {
        d++;
        n--;
    }
    if (upd_state == UPD_ST_BUSY) {
        return 0; /* still working the previous command; host should poll */
    }

    if (d[0] == UPD_CMD_WRITE
#if IS_ENABLED(CONFIG_APEX_G4B_AB_ROLLBACK)
        || d[0] == UPD_CMD_AB_WRITE
#endif
       ) {
        if (n < 9U) {
            return -EINVAL;
        }
        clen = (uint16_t)((uint16_t)d[3] | ((uint16_t)d[4] << 8));
        if (clen > UPD_MAX_CHUNK || (size_t)clen > (n - 9U)) {
            return -EINVAL;
        }
        off = (uint32_t)d[5] | ((uint32_t)d[6] << 8) |
              ((uint32_t)d[7] << 16) | ((uint32_t)d[8] << 24);
    }
#if IS_ENABLED(CONFIG_APEX_G4B_AB_ROLLBACK)
    else if (d[0] == UPD_CMD_AB_COMMIT) {
        if (n < 9U) {
            return -EINVAL;
        }
        /* off carries b_len for the commit */
        off = (uint32_t)d[5] | ((uint32_t)d[6] << 8) |
              ((uint32_t)d[7] << 16) | ((uint32_t)d[8] << 24);
    }
#endif
    else if (d[0] != UPD_CMD_ERASE && d[0] != UPD_CMD_RESET
#if IS_ENABLED(CONFIG_APEX_G4B_AB_ROLLBACK)
             && d[0] != UPD_CMD_AB_ERASE
#endif
            ) {
        return -ENOTSUP;
    }

    /* Publish only after the whole command validates. A short WRITE/COMMIT must
     * never wake the worker with len/off/data left over from a prior command. */
    upd_mbox.cmd = d[0];
    upd_mbox.len = clen;
    upd_mbox.off = off;
    if (clen > 0U) {
        memcpy(upd_mbox.data, d + 9, clen);
    }

    upd_state = UPD_ST_BUSY;
    k_sem_give(&upd_sem);
    return 0;
}

static const struct hid_device_ops upd_ops = {
    .get_report = upd_get_report,
    .set_report = upd_set_report,
    /* No input report is ever submitted (the 1-byte IN endpoint just keeps the
     * class happy), so no .input_report_done is needed. */
};

/* The single owner of SPIM0 for the flash. Opens the bus on the first erase,
 * keeps it open across the chunk stream, and closes + resets on 0x41. */
static void upd_thread(void *a, void *b, void *c)
{
    bool open = false;

    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    for (;;) {
        k_sem_take(&upd_sem, K_FOREVER);

        switch (upd_mbox.cmd) {
        case UPD_CMD_ERASE:
            if (!open) {
                g4b_spinor_open();
                open = true;
            }
            upd_state = g4b_spinor_stage_erase() ? UPD_ST_READY : UPD_ST_ERROR;
            break;

        case UPD_CMD_WRITE:
            if (!open) {
                g4b_spinor_open();
                open = true;
            }
            upd_state = g4b_spinor_stage_write(upd_mbox.off, upd_mbox.data,
                                               upd_mbox.len)
                            ? UPD_ST_READY : UPD_ST_ERROR;
            break;

        case UPD_CMD_RESET:
            if (open) {
                g4b_spinor_close();
                open = false;
            }
            /* Let the SET_FEATURE status stage finish, then reboot into the
             * loader, which applies the freshly staged image. Does not return. */
            k_msleep(50);
            __DSB();
            __ISB();
            NVIC_SystemReset();
            break;

#if IS_ENABLED(CONFIG_APEX_G4B_AB_ROLLBACK)
        /* The A/B helpers open/close SPIM0 per op under the extbus lock, a
         * different bus-ownership model than the legacy `open`-held stream.
         * Close the legacy bus first so the two never overlap. */
        case UPD_CMD_AB_ERASE:
            if (open) { g4b_spinor_close(); open = false; }
            upd_state = g4b_ab_stage_erase() ? UPD_ST_READY : UPD_ST_ERROR;
            break;

        case UPD_CMD_AB_WRITE:
            if (open) { g4b_spinor_close(); open = false; }
            upd_state = g4b_ab_stage_write(upd_mbox.off, upd_mbox.data,
                                           upd_mbox.len)
                            ? UPD_ST_READY : UPD_ST_ERROR;
            break;

        case UPD_CMD_AB_COMMIT:
            if (open) { g4b_spinor_close(); open = false; }
            upd_state = g4b_ab_commit(upd_mbox.off) ? UPD_ST_READY : UPD_ST_ERROR;
            break;
#endif

        default:
            upd_state = UPD_ST_READY;
            break;
        }
    }
}

K_THREAD_DEFINE(g4b_updater_tid, 1024, upd_thread, NULL, NULL, NULL,
                K_PRIO_PREEMPT(10), 0, 0);

static int g4b_updater_init(void)
{
    /* Fingerprint the image once at boot. Reading XIP flash directly is
     * fine; the region is the applied vendor image and always present. */
    upd_marker = crc32_ieee((const uint8_t *)G4B_IMG_BASE, G4B_IMG_LEN);

    /* hid_updater sits alongside the keyboard and optional gamepad interfaces. */
    upd_dev = DEVICE_DT_GET(DT_NODELABEL(hid_updater));
    if (!device_is_ready(upd_dev)) {
        /* The hid_updater node isn't in this build. Fail quietly - the rest of
         * the board is unaffected; only host-driven self-flash is unavailable. */
        upd_dev = NULL;
        return 0;
    }
    hid_device_register(upd_dev, upd_desc, sizeof(upd_desc), &upd_ops);
    return 0;
}

/* 94, alongside the gamepad interface, before usb_enable() at 96. */
SYS_INIT(g4b_updater_init, APPLICATION, 94);
