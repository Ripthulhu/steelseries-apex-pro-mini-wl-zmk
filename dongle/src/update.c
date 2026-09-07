/* SPDX-License-Identifier: MIT */
#include "apex_shell_link.h"
#include "apex_radio_input.h"
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <string.h>

/* USB frame: AUP1, offset:u32, length:u16, reserved:u16, data, CRC32.
 * A zero-length frame closes the transfer. Host waits for each block's reply. */
static uint8_t block[272];
static size_t used;
static uint32_t generation;
static int64_t last_input;

static void reply(const struct shell *sh, int rc, uint32_t received)
{
    uint8_t response[16] = { 'A', 'U', 'R', '1' };
    sys_put_le32(received, response + 4);
    sys_put_le32((uint32_t)rc, response + 8);
    sys_put_le32(crc32_ieee(response, 12), response + 12);
    size_t sent = 0;
    int64_t end = k_uptime_get() + 2000;
    while (sent < sizeof(response) && k_uptime_get() < end) {
        size_t n = 0;
        if (sh->iface->api->write(sh->iface, response + sent, sizeof(response) - sent, &n)) break;
        sent += n;
        if (!n) k_sleep(K_MSEC(1));
    }
}

static int forward(uint32_t offset, const uint8_t *data, size_t length, uint32_t *received)
{
    for (size_t sent = 0; sent < length;) {
        size_t n = MIN(length - sent, 32);
        uint8_t frame[37] = { APEX_UPDATE_DATA };
        sys_put_le32(offset + sent, frame + 1);
        memcpy(frame + 5, data + sent, n);
        int64_t end = k_uptime_get() + 2000;
        int rc = apex_shell_send(generation, frame, n + 5, end);
        if (rc) return rc;
        for (;;) {
            uint8_t response[APEX_STREAM_DATA_MAX];
            int count = apex_shell_read(generation, response);
            if (count < 0) return count;
            if (count) {
                if (count != 9 || response[0] != APEX_UPDATE_REPLY) return -EPROTO;
                *received = sys_get_le32(response + 5);
                rc = (int32_t)sys_get_le32(response + 1);
                if (rc) return rc;
                if (*received != offset + sent + n) return -EPROTO;
                break;
            }
            if (k_uptime_get() >= end) return -ETIMEDOUT;
            k_sleep(K_MSEC(1));
        }
        sent += n;
    }
    return 0;
}

static void receive(const struct shell *sh, uint8_t *data, size_t length)
{
    /* An abandoned partial USB block can be cancelled with Ctrl+C after 2 s.
     * No incomplete block is forwarded to the keyboard. */
    if (k_uptime_get() - last_input > 2000) used = 0;
    last_input = k_uptime_get();
    for (size_t i = 0; i < length; i++) {
        if (!used && data[i] == 3) { shell_set_bypass(sh, NULL); return; }
        block[used++] = data[i];
        if (used < 12) continue;
        uint32_t offset = sys_get_le32(block + 4);
        uint16_t n = sys_get_le16(block + 8);
        if (memcmp(block, "AUP1", 4) || n > 256 || sys_get_le16(block + 10) ||
            n > 4096 - offset % 4096) {
            reply(sh, -EINVAL, 0);
            used = 0;
            shell_set_bypass(sh, NULL);
            return;
        }
        if (used < (size_t)n + 16) continue;
        int rc = crc32_ieee(block, n + 12) == sys_get_le32(block + n + 12) ? 0 : -EBADMSG;
        uint32_t received = offset;
        if (!rc && n) rc = forward(offset, block + 12, n, &received);
        reply(sh, rc, received);
        used = 0;
        last_input = k_uptime_get();
        if (rc || !n) {
            shell_set_bypass(sh, NULL);
            if (rc) apex_radio_request_session();
            return;
        }
    }
}

int receiver_update_start(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    if (!apex_radio_input_connected()) return -ENOTCONN;
    generation = apex_shell_generation();
    used = 0;
    last_input = k_uptime_get();
    shell_set_bypass(sh, receive);
    shell_print(sh, "APX-UPLOAD-1 READY");
    return 0;
}
