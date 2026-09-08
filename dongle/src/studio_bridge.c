/* SPDX-License-Identifier: MIT */
/* Transparent ZMK Studio bridge on the receiver.
 *
 * A dedicated USB CDC ("Receiver Studio") that the host's ZMK Studio connects to,
 * pumped byte-for-byte over the 2.4 GHz link so the keyboard can be configured
 * while wireless. Host bytes become APEX_STUDIO_DATA frames; the keyboard's
 * APEX_STUDIO_REPLY frames are written back to the host. The bytes are opaque -
 * ZMK Studio applies its own SOF/EOF framing, so this bridge understands none of
 * it.
 *
 * The CDC endpoints are serviced through the interrupt-driven UART API (the poll
 * API does not drain the cdc-acm OUT endpoint on the device_next USB stack, so a
 * host write would stall on flow control). The ISR only moves bytes to/from two
 * single-producer/single-consumer rings; the thread does the radio I/O, which can
 * block and must never run in interrupt context.
 *
 * The radio carries a single shared stream (also used by the `keyboard` shell
 * command and firmware `update`). To avoid stealing their frames, the bridge only
 * reads the radio while a Studio session is live - within SESSION_IDLE_MS of the
 * last Studio byte in either direction. Studio and those maintenance paths are
 * therefore mutually exclusive, which is fine: nobody flashes firmware while
 * editing a keymap.
 */
#include "apex_shell_link.h"
#include "apex_radio_input.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>

#define STUDIO_UART DEVICE_DT_GET(DT_NODELABEL(studio_uart))
#define SESSION_IDLE_MS 3000

RING_BUF_DECLARE(studio_usb_rx, 512); /* host -> radio */
RING_BUF_DECLARE(studio_usb_tx, 512); /* radio -> host */

static const struct device *studio_dev;

static void studio_isr(const struct device *dev, void *user_data)
{
    ARG_UNUSED(user_data);
    if (!uart_irq_update(dev)) {
        return;
    }
    /* host -> device: pull the OUT endpoint into the RX ring for the thread. */
    while (uart_irq_rx_ready(dev)) {
        uint8_t *buf;
        uint32_t space = ring_buf_put_claim(&studio_usb_rx, &buf, 64);
        if (!space) {
            uint8_t drop[32];
            (void)uart_fifo_read(dev, drop, sizeof(drop)); /* ring full: shed a little */
            continue;
        }
        int got = uart_fifo_read(dev, buf, space);
        ring_buf_put_finish(&studio_usb_rx, MAX(got, 0));
        if (got <= 0) {
            break;
        }
    }
    /* device -> host: drain the TX ring into the IN endpoint. */
    if (uart_irq_tx_ready(dev)) {
        while (ring_buf_size_get(&studio_usb_tx) > 0) {
            uint8_t *buf;
            uint32_t avail = ring_buf_get_claim(&studio_usb_tx, &buf, 64);
            if (!avail) {
                break;
            }
            int sent = uart_fifo_fill(dev, buf, avail);
            ring_buf_get_finish(&studio_usb_tx, MAX(sent, 0));
            if (sent <= 0) {
                break;
            }
        }
        if (ring_buf_size_get(&studio_usb_tx) == 0) {
            uart_irq_tx_disable(dev);
        }
    }
}

static void studio_bridge_main(void)
{
    int64_t last_activity = 0;

    studio_dev = STUDIO_UART;
    while (!device_is_ready(studio_dev)) {
        k_sleep(K_MSEC(100));
    }
    uart_irq_callback_user_data_set(studio_dev, studio_isr, NULL);
    uart_irq_rx_enable(studio_dev);

    for (;;) {
        bool worked = false;
        int64_t now = k_uptime_get();

        /* host -> keyboard: forward buffered Studio bytes in <=46-byte frames. */
        if (apex_radio_input_connected() && ring_buf_size_get(&studio_usb_rx) > 0) {
            uint8_t frame[APEX_STREAM_DATA_MAX];
            frame[0] = APEX_STUDIO_DATA;
            uint32_t n = ring_buf_get(&studio_usb_rx, frame + 1, sizeof(frame) - 1);
            if (n) {
                apex_shell_bulk_touch();
                (void)apex_shell_send(apex_shell_generation(), frame, n + 1, now + 2000);
                last_activity = now;
                worked = true;
            }
        }

        /* keyboard -> host: only while a session is live, so idle periods leave the
         * shared stream to the shell/update paths. */
        if (now - last_activity < SESSION_IDLE_MS) {
            uint8_t frame[APEX_STREAM_DATA_MAX];
            int n = apex_shell_read(apex_shell_generation(), frame);
            if (n > 1 && frame[0] == APEX_STUDIO_REPLY) {
                ring_buf_put(&studio_usb_tx, frame + 1, n - 1);
                uart_irq_tx_enable(studio_dev);
                last_activity = now;
                worked = true;
            }
        }

        if (!worked) {
            k_sleep(K_MSEC(1));
        }
    }
}

K_THREAD_DEFINE(studio_bridge, 1536, studio_bridge_main, NULL, NULL, NULL, 7, 0, 0);
