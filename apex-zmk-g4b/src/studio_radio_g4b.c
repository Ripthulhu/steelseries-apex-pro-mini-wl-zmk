/* SPDX-License-Identifier: MIT */
/* ZMK Studio RPC transport over the 2.4 GHz radio link. It lets the host's ZMK
 * Studio configure the keyboard THROUGH the dongle while wireless: inbound RPC
 * bytes arrive as APEX_STUDIO_DATA frames (fed into the RPC RX ring here) and RPC
 * responses drain back out as APEX_STUDIO_REPLY frames.
 *
 * The RPC core is transport-agnostic - it applies its own SOF/EOF framing, so a
 * transport only carries opaque bytes. This tree's ZMK already defines
 * ZMK_TRANSPORT_RADIO and selects it whenever the keyboard is in dongle mode; it
 * simply had no rpc transport registered for that endpoint, so Studio responses
 * were dropped. Registering this one wires TX; RX is a global ring buffer that
 * anything may feed.
 */
#include "apex_shell_link.h"
#include "studio_radio_g4b.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>
#include <string.h>
#include <zmk/endpoints_types.h>
#include <zmk/studio/rpc.h>

static bool handling_rx;

void g4b_studio_radio_feed(const uint8_t *data, size_t len)
{
    if (!handling_rx || !len) {
        return;
    }
    struct ring_buf *rx = zmk_rpc_get_rx_buf();
    uint32_t copied = 0;
    while (copied < len) {
        uint8_t *buf;
        uint32_t claim = ring_buf_put_claim(rx, &buf, len - copied);
        if (!claim) {
            /* RX ring is full; drop the remainder. The RPC's SOF/EOF framing
             * resynchronises on the next complete message. */
            break;
        }
        memcpy(buf, data + copied, claim);
        copied += claim;
        ring_buf_put_finish(rx, claim);
    }
    zmk_rpc_rx_notify();
}

/* Drain the framed RPC response out over the radio, one <=46-byte frame per
 * stop-and-wait slot. Runs on the low-priority RPC thread; apex_shell_send uses a
 * bounded deadline and returns on a dropped or reset link, so a disconnect can
 * never wedge it. Anything that cannot be sent is consumed and discarded so the
 * ring never sticks and re-enters this callback forever. Batches until a message
 * completes or the ring half-fills, mirroring the upstream UART transport. */
static void radio_tx_notify(struct ring_buf *tx_buf, size_t added, bool msg_done, void *user_data)
{
    ARG_UNUSED(added);
    ARG_UNUSED(user_data);
    if (!msg_done && ring_buf_size_get(tx_buf) < ring_buf_capacity_get(tx_buf) / 2) {
        return;
    }
    while (ring_buf_size_get(tx_buf) > 0) {
        uint8_t frame[APEX_STREAM_DATA_MAX];
        uint8_t *buf;
        uint32_t claim = ring_buf_get_claim(tx_buf, &buf, sizeof(frame) - 1);
        if (!claim) {
            break;
        }
        frame[0] = APEX_STUDIO_REPLY;
        memcpy(frame + 1, buf, claim);
        apex_shell_bulk_touch();
        int rc = apex_shell_send(apex_shell_generation(), frame, claim + 1, k_uptime_get() + 2000);
        ring_buf_get_finish(tx_buf, claim);
        if (rc) {
            uint32_t drop;
            while ((drop = ring_buf_get_claim(tx_buf, &buf, sizeof(frame))) > 0) {
                ring_buf_get_finish(tx_buf, drop);
            }
            break;
        }
    }
}

static int radio_rx_start(void)
{
    handling_rx = true;
    return 0;
}

static int radio_rx_stop(void)
{
    handling_rx = false;
    return 0;
}

ZMK_RPC_TRANSPORT(radio, ZMK_TRANSPORT_RADIO, radio_rx_start, radio_rx_stop, NULL, radio_tx_notify);
