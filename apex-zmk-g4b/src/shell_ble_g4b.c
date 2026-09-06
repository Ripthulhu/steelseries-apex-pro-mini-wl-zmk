/* SPDX-License-Identifier: MIT
 *
 * Wireless `apex` shell over the BLE Nordic UART Service (NUS).
 *
 * A second Zephyr shell instance whose transport is bt_nus: bytes a NUS client
 * writes become shell input, shell output is notified back over NUS. The NUS
 * GATT service is additive - it sits beside ZMK's HID and Studio services on
 * the existing connection and never touches advertising, so it does not risk
 * the controller the way stopping advertising from our own code once did.
 *
 * The bridge is gated by a persisted enable (settings key "apxbl/on"): off by
 * default, and `apex bleshell on` from the USB shell turns it on for good. When
 * off, received bytes are dropped and no notifications are sent, so the service
 * is inert.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/settings/settings.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/services/nus.h>

#include "shell_ble_g4b.h"

#define SH_BLE_RX_SIZE     256u
#define SH_BLE_TX_CHUNK    20u   /* safe ATT payload for the 23-byte default MTU */
#define SH_BLE_TX_RETRIES  20u

RING_BUF_DECLARE(sh_ble_rx_rb, SH_BLE_RX_SIZE);

struct sh_ble_ctx {
    shell_transport_handler_t handler;
    void *context;
    bool subscribed;
};
static struct sh_ble_ctx sh_ble;

/* Persisted, so it survives a reboot once enabled. */
static bool ble_shell_enabled;

/* --- NUS callbacks (run on the BT RX thread) ------------------------------ */

static void nus_notif_enabled(bool enabled, void *ctx)
{
    ARG_UNUSED(ctx);
    sh_ble.subscribed = enabled;
}

static void nus_received(struct bt_conn *conn, const void *data, uint16_t len,
                         void *ctx)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(ctx);
    if (!ble_shell_enabled) {
        return;
    }
    (void)ring_buf_put(&sh_ble_rx_rb, data, len);
    if (sh_ble.handler) {
        sh_ble.handler(SHELL_TRANSPORT_EVT_RX_RDY, sh_ble.context);
    }
}

static struct bt_nus_cb nus_cb = {
    .notif_enabled = nus_notif_enabled,
    .received = nus_received,
};

/* --- shell transport ------------------------------------------------------ */

static int sh_ble_init(const struct shell_transport *transport, const void *config,
                       shell_transport_handler_t handler, void *context)
{
    ARG_UNUSED(transport);
    ARG_UNUSED(config);
    sh_ble.handler = handler;
    sh_ble.context = context;
    (void)bt_nus_cb_register(&nus_cb, NULL);
    return 0;
}

static int sh_ble_uninit(const struct shell_transport *transport)
{
    ARG_UNUSED(transport);
    return 0;
}

static int sh_ble_enable(const struct shell_transport *transport, bool blocking)
{
    ARG_UNUSED(transport);
    ARG_UNUSED(blocking);
    return 0;
}

static int sh_ble_write(const struct shell_transport *transport, const void *data,
                        size_t length, size_t *cnt)
{
    ARG_UNUSED(transport);
    const uint8_t *p = data;
    size_t left = length;

    /* conn == NULL notifies every subscribed central. Drop (never block the
     * shell) if nobody is listening or the ATT queue stays full. */
    if (ble_shell_enabled && sh_ble.subscribed) {
        while (left > 0u) {
            uint16_t chunk = (left > SH_BLE_TX_CHUNK) ? SH_BLE_TX_CHUNK
                                                      : (uint16_t)left;
            int err;
            uint32_t tries = 0u;

            while ((err = bt_nus_send(NULL, p, chunk)) == -ENOMEM &&
                   tries++ < SH_BLE_TX_RETRIES) {
                k_msleep(4);
            }
            if (err) {
                break; /* give up on this output rather than hang */
            }
            p += chunk;
            left -= chunk;
        }
    }

    *cnt = length;
    if (sh_ble.handler) {
        sh_ble.handler(SHELL_TRANSPORT_EVT_TX_RDY, sh_ble.context);
    }
    return 0;
}

static int sh_ble_read(const struct shell_transport *transport, void *data,
                       size_t length, size_t *cnt)
{
    ARG_UNUSED(transport);
    *cnt = ring_buf_get(&sh_ble_rx_rb, data, length);
    return 0;
}

static const struct shell_transport_api sh_ble_transport_api = {
    .init = sh_ble_init,
    .uninit = sh_ble_uninit,
    .enable = sh_ble_enable,
    .write = sh_ble_write,
    .read = sh_ble_read,
};

static struct shell_transport sh_ble_transport = {
    .api = &sh_ble_transport_api,
    .ctx = &sh_ble,
};

SHELL_DEFINE(g4b_ble_shell, "apex-ble$ ", &sh_ble_transport, 4, 100,
             SHELL_FLAG_OLF_CRLF);

/* --- persistence ---------------------------------------------------------- */

static int bleshell_set(const char *name, size_t len, settings_read_cb read_cb,
                        void *cb_arg)
{
    if (settings_name_steq(name, "on", NULL)) {
        uint8_t v = 0u;

        if (len == sizeof(v) && read_cb(cb_arg, &v, sizeof(v)) == (int)sizeof(v)) {
            ble_shell_enabled = (v != 0u);
        }
        return 0;
    }
    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(apex_bleshell, "apxbl", NULL, bleshell_set, NULL,
                               NULL);

/* --- public API ----------------------------------------------------------- */

void g4b_ble_shell_set_enabled(bool on)
{
    ble_shell_enabled = on;
    if (!on) {
        (void)ring_buf_reset(&sh_ble_rx_rb);
    }
    uint8_t v = on ? 1u : 0u;
    (void)settings_save_one("apxbl/on", &v, sizeof(v));
}

bool g4b_ble_shell_is_enabled(void)
{
    return ble_shell_enabled;
}

bool g4b_ble_shell_subscribed(void)
{
    return sh_ble.subscribed;
}

/* --- init ----------------------------------------------------------------- */

static int g4b_ble_shell_backend_init(void)
{
    static const struct shell_backend_config_flags cfg =
        SHELL_DEFAULT_BACKEND_CONFIG_FLAGS;

    /* No log backend: the slow NUS link carries the interactive console only;
     * logs stay on the USB shell. */
    (void)shell_init(&g4b_ble_shell, NULL, cfg, false, 0);
    return 0;
}

SYS_INIT(g4b_ble_shell_backend_init, POST_KERNEL,
         CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
