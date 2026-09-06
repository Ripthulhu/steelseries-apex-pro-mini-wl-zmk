/* SPDX-License-Identifier: MIT */
/* Fixed-channel, low-rate hardware test. Not the production input scheduler. */
#include "apex_connection.h"
#include "apex_pair.h"
#include "apex_radio_probe.h"
#include "apex_radio_input.h"
#include "apex_aes.h"
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <hal/nrf_radio.h>
#include <zephyr/irq.h>

#define EVENT_RX IS_ENABLED(CONFIG_APEX_RECEIVER_RADIO_EVENT_RX)
#if EVENT_RX
static K_SEM_DEFINE(radio_event, 0, 1);
static atomic_t rx_interrupts;
static void radio_isr(const void *arg)
{
    ARG_UNUSED(arg);
    /* Leave END/DISABLED latched for the thread that owns the DMA buffer. */
    NRF_RADIO->INTENCLR = RADIO_INTENCLR_DISABLED_Msk;
    atomic_inc(&rx_interrupts);
    k_sem_give(&radio_event);
}
#endif

static K_SEM_DEFINE(start_signal, 0, 1);
static enum apex_role local_role;
static struct apex_connection connection;
static uint8_t dma[APEX_PACKET_MAX + 1] __aligned(4);
static uint8_t rx[APEX_PACKET_MAX], tx[APEX_PACKET_MAX], plain[APEX_PACKET_PAYLOAD_MAX];
static atomic_t running, state, received, transmitted, authenticated, rejected, sessions, error;
static atomic_t replay_rejected;
static atomic_t crc_errors;
static atomic_t last_header, last_crc;
static atomic_t max_loop_us, max_process_us, timeout_loop_us, timeout_silence_ms;
static atomic_t loops_over_10ms;
static atomic_t timeout_tx, timeout_rx, timeout_crc, timeout_rejected;
static uint32_t valid_tx, valid_rx, valid_crc, valid_rejected;

static void mark_valid_counters(void)
{
    valid_tx = atomic_get(&transmitted);
    valid_rx = atomic_get(&received);
    valid_crc = atomic_get(&crc_errors);
    valid_rejected = atomic_get(&rejected);
}

static void maximum(atomic_t *value, uint32_t sample)
{
    atomic_val_t old = atomic_get(value);
    while (sample > (uint32_t)old && !atomic_cas(value, old, sample)) old = atomic_get(value);
}
static bool receiving;
static struct onoff_client clock_client;
#define INPUT_ENABLED (IS_ENABLED(CONFIG_APEX_G4B_RADIO_INPUT) || IS_ENABLED(CONFIG_APEX_RECEIVER_RADIO_INPUT))
#if INPUT_ENABLED
static struct apex_input_queue input_queue;
static struct k_spinlock input_lock;
static atomic_t input_selected, resync, queue_overflows, input_acked;
static atomic_t rx_sequence, input_delivered, duplicate_reports, usb_waits;
static atomic_t link_timeouts, requested_resets;
__weak int apex_radio_deliver(const struct apex_input_frame *frame) { return -ENOTSUP; }
__weak void apex_radio_release(void) {}
__weak uint8_t apex_radio_host_leds(void) { return 0; }
__weak void apex_radio_update_leds(uint8_t leds) { ARG_UNUSED(leds); }

int apex_radio_queue_report(uint8_t type, const uint8_t *data, size_t length)
{
    if (!atomic_get(&input_selected)) return 0;
    k_spinlock_key_t key = k_spin_lock(&input_lock);
    int rc = apex_input_push(&input_queue, type, data, length);
    if (rc == -2) {
        atomic_inc(&queue_overflows);
        atomic_set(&resync, 1);
    }
    k_spin_unlock(&input_lock, key);
    return rc ? -ENOBUFS : 0;
}

void apex_radio_input_select(bool selected)
{
    if (atomic_set(&input_selected, selected) == selected) return;
    k_spinlock_key_t key = k_spin_lock(&input_lock);
    memset(&input_queue, 0, sizeof(input_queue));
    k_spin_unlock(&input_lock, key);
    atomic_set(&resync, 1);
}

bool apex_radio_input_connected(void)
{
    return atomic_get(&state) == APEX_CONNECTION_ESTABLISHED;
}

void apex_radio_request_session(void)
{
    atomic_set(&resync, 1);
#if EVENT_RX
    k_sem_give(&radio_event);
#endif
}

static int input_packet(uint8_t type, const uint8_t *data, size_t length)
{
    uint32_t ack = 0;
    if (local_role == APEX_KEYBOARD) {
        if (type != APEX_PACKET_ACK || length != 6 || data[0] != APEX_INPUT_VERSION) return -EINVAL;
        ack = sys_get_le32(data + 1);
        k_spinlock_key_t key = k_spin_lock(&input_lock);
        if (apex_input_ack(&input_queue, ack)) atomic_inc(&input_acked);
        k_spin_unlock(&input_lock, key);
        apex_radio_update_leds(data[5]);
        return 0;
    }
    if (type == APEX_PACKET_INPUT) {
        struct apex_input_frame frame;
        if (apex_input_unpack(data, length, &frame)) return -EINVAL;
        uint32_t previous = (uint32_t)atomic_get(&rx_sequence);
        if (frame.sequence == previous + 1) {
            int rc = apex_radio_deliver(&frame);
            if (rc == -EAGAIN) {
                atomic_inc(&usb_waits);
                /* Confirm radio liveness without acknowledging the pending
                 * USB report. The sender keeps retrying this sequence. */
            } else {
                if (rc) return rc;
                atomic_set(&rx_sequence, frame.sequence);
                atomic_inc(&input_delivered);
            }
        } else if (frame.sequence != previous) return -EINVAL;
        else atomic_inc(&duplicate_reports);
        ack = atomic_get(&rx_sequence);
    } else if (type != APEX_PACKET_KEEPALIVE || length != 1 || data[0] != APEX_INPUT_VERSION) {
        return -EINVAL;
    }
    uint8_t reply[6] = {APEX_INPUT_VERSION};
    sys_put_le32(ack, reply + 1);
    reply[5] = apex_radio_host_leds();
    return apex_connection_encode(&connection, APEX_PACKET_ACK, reply, sizeof(reply), tx, sizeof(tx));
}
#endif

static void wipe(void *ptr, size_t len)
{
    volatile uint8_t *p = ptr;
    while (len--) *p++ = 0;
}

static int radio_stop(void)
{
#if EVENT_RX
    NRF_RADIO->INTENCLR = RADIO_INTENCLR_DISABLED_Msk;
#endif
    receiving = false;
    if (NRF_RADIO->STATE == RADIO_STATE_STATE_Disabled) return 0;
    NRF_RADIO->EVENTS_DISABLED = 0;
    NRF_RADIO->TASKS_DISABLE = 1;
    uint32_t start = k_cycle_get_32();
    while (!NRF_RADIO->EVENTS_DISABLED) {
        if (k_cyc_to_us_floor32(k_cycle_get_32() - start) > 1000) return -ETIMEDOUT;
    }
    return 0;
}

static void radio_receive(void)
{
    memset(dma, 0, sizeof(dma));
    NRF_RADIO->PACKETPTR = (uint32_t)dma;
    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->EVENTS_DISABLED = 0;
    __DMB();
#if EVENT_RX
    NVIC_ClearPendingIRQ(RADIO_IRQn);
    NRF_RADIO->INTENSET = RADIO_INTENSET_DISABLED_Msk;
#endif
    NRF_RADIO->TASKS_RXEN = 1;
    receiving = true;
}

static int radio_send(const uint8_t *packet, size_t length)
{
    if (!length || length > APEX_PACKET_MAX) return -EINVAL;
    int rc = radio_stop();
    if (rc) return rc;
    dma[0] = length;
    memcpy(dma + 1, packet, length);
    NRF_RADIO->PACKETPTR = (uint32_t)dma;
    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->EVENTS_DISABLED = 0;
    __DMB();
    NRF_RADIO->TASKS_TXEN = 1;
    uint32_t start = k_cycle_get_32();
    while (!NRF_RADIO->EVENTS_DISABLED) {
        if (k_cyc_to_us_floor32(k_cycle_get_32() - start) > 2000) {
            (void)radio_stop();
            return -ETIMEDOUT;
        }
    }
    atomic_inc(&transmitted);
    radio_receive();
    return 0;
}

static int radio_init(void)
{
    struct onoff_manager *manager = z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
    sys_notify_init_spinwait(&clock_client.notify);
    int rc = onoff_request(manager, &clock_client);
    if (rc < 0) return rc;
    int64_t deadline = k_uptime_get() + 100;
    while (sys_notify_fetch_result(&clock_client.notify, &rc)) {
        if (k_uptime_get() >= deadline) {
            (void)onoff_cancel_or_release(manager, &clock_client);
            return -ETIMEDOUT;
        }
        k_sleep(K_MSEC(1));
    }
    if (rc) return rc;
    rc = radio_stop();
    if (rc) return rc;
    NRF_RADIO->INTENCLR = UINT32_MAX;
#if EVENT_RX
    irq_disable(RADIO_IRQn);
    irq_connect_dynamic(RADIO_IRQn, 3, radio_isr, NULL, 0);
    NVIC_ClearPendingIRQ(RADIO_IRQn);
    irq_enable(RADIO_IRQn);
#endif
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
    NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit;
    NRF_RADIO->TXPOWER = RADIO_TXPOWER_TXPOWER_0dBm;
    NRF_RADIO->FREQUENCY = CONFIG_APEX_RADIO_TEST_CHANNEL;
    NRF_RADIO->PCNF0 = 8 << RADIO_PCNF0_LFLEN_Pos;
    NRF_RADIO->PCNF1 = APEX_PACKET_MAX | (4 << RADIO_PCNF1_BALEN_Pos) |
                      RADIO_PCNF1_WHITEEN_Msk;
    NRF_RADIO->BASE0 = 0x53a9c671;
    NRF_RADIO->PREFIX0 = 0xd2;
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1;
    NRF_RADIO->DATAWHITEIV = 40;
    NRF_RADIO->CRCCNF = RADIO_CRCCNF_LEN_Three |
                       (RADIO_CRCCNF_SKIPADDR_Skip << RADIO_CRCCNF_SKIPADDR_Pos);
    NRF_RADIO->CRCINIT = 0xabcdef;
    NRF_RADIO->CRCPOLY = 0x100065b;
    radio_receive();
    return 0;
}

static int new_session(void)
{
#if INPUT_ENABLED
    k_spinlock_key_t key = k_spin_lock(&input_lock);
    apex_input_disconnect(&input_queue);
    k_spin_unlock(&input_lock, key);
    atomic_set(&rx_sequence, 0);
    if (local_role == APEX_RECEIVER) apex_radio_release();
#endif
    uint8_t nonce[16];
    const struct device *rng = DEVICE_DT_GET(DT_NODELABEL(rng));
    if (!device_is_ready(rng)) return -ENODEV;
    int rc = entropy_get_entropy(rng, nonce, sizeof(nonce));
    if (!rc) rc = apex_connection_start(&connection, nonce);
    wipe(nonce, sizeof(nonce));
    atomic_set(&state, connection.state);
    return rc;
}

static void probe_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    k_sem_take(&start_signal, K_FOREVER);
#if INPUT_ENABLED
    /* Keyboard calls start only after bt_disable(); receiver has no BLE stack. */
    apex_aes_enable();
#endif
    uint8_t bond[APEX_BOND_SIZE];
    int rc = apex_bond_load(bond);
    if (!rc) rc = apex_connection_init(&connection, bond + 12, bond + 4, local_role);
    wipe(bond, sizeof(bond));
    if (!rc) rc = radio_init();
    if (!rc) rc = new_session();
    if (rc) goto failed;
    int64_t last_valid = k_uptime_get(), next_send = 0;
    uint32_t last_loop = k_cycle_get_32();
#if !INPUT_ENABLED
    uint32_t ping = 0;
#endif
    for (;;) {
        uint32_t loop_start = k_cycle_get_32();
        uint32_t loop_us = k_cyc_to_us_floor32(loop_start - last_loop);
        last_loop = loop_start;
        maximum(&max_loop_us, loop_us);
        if (loop_us > 10000) atomic_inc(&loops_over_10ms);
        int64_t now = k_uptime_get();
        if (receiving && NRF_RADIO->EVENTS_END) {
            bool valid_crc = NRF_RADIO->CRCSTATUS != 0;
            atomic_set(&last_header, sys_get_le32(dma));
            atomic_set(&last_crc, NRF_RADIO->RXCRC);
            if (!valid_crc) atomic_inc(&crc_errors);
            size_t length = dma[0];
            rc = radio_stop();
            if (rc) goto failed;
            if (valid_crc && length >= 2 && length <= sizeof(rx)) {
                memcpy(rx, dma + 1, length);
                atomic_inc(&received);
                uint8_t previous = connection.state;
                int n;
                if (rx[1] >= 0x80) {
                    n = apex_connection_receive(&connection, rx, length, tx, sizeof(tx));
                } else {
                    uint8_t type = 0;
                    n = apex_connection_decode(&connection, rx, length, &type, plain, sizeof(plain));
                    if (n >= 0) {
                        atomic_inc(&authenticated);
#if INPUT_ENABLED
                        n = input_packet(type, plain, n);
#else
                        uint8_t check[APEX_PACKET_PAYLOAD_MAX], check_type;
                        if (apex_connection_decode(&connection, rx, length, &check_type,
                                                   check, sizeof(check)) == APEX_PACKET_REPLAY) {
                            atomic_inc(&replay_rejected);
                        }
                        if (local_role == APEX_RECEIVER && type == APEX_PACKET_KEEPALIVE) {
                            n = apex_connection_encode(&connection, APEX_PACKET_ACK, plain, n,
                                                       tx, sizeof(tx));
                        } else n = 0;
#endif
                    }
                }
                if (n >= 0) {
                    last_valid = now;
                    mark_valid_counters();
                    if (previous != APEX_CONNECTION_ESTABLISHED &&
                        connection.state == APEX_CONNECTION_ESTABLISHED) {
                        atomic_inc(&sessions);
#if INPUT_ENABLED
                        if (local_role == APEX_KEYBOARD) {
                            k_spinlock_key_t key = k_spin_lock(&input_lock);
                            apex_input_session(&input_queue);
                            k_spin_unlock(&input_lock, key);
                        }
#endif
                    }
                    if (n > 0) {
                        rc = radio_send(tx, n);
                        if (rc) goto failed;
                    }
                } else atomic_inc(&rejected);
                atomic_set(&state, connection.state);
            }
            if (!receiving) radio_receive();
        }
        bool reset_requested = false;
        int timeout_ms = local_role == APEX_KEYBOARD ? 5500 : 4000;
#if INPUT_ENABLED
        reset_requested = atomic_set(&resync, 0);
        if (connection.state == APEX_CONNECTION_ESTABLISHED) timeout_ms = 100;
#endif
        if (reset_requested || now - last_valid >= timeout_ms) {
            if (!reset_requested && connection.state == APEX_CONNECTION_ESTABLISHED) {
                atomic_set(&timeout_loop_us, loop_us);
                atomic_set(&timeout_silence_ms, now - last_valid);
                /* Activity since the last accepted packet, before session reset. */
                atomic_set(&timeout_tx, (uint32_t)atomic_get(&transmitted) - valid_tx);
                atomic_set(&timeout_rx, (uint32_t)atomic_get(&received) - valid_rx);
                atomic_set(&timeout_crc, (uint32_t)atomic_get(&crc_errors) - valid_crc);
                atomic_set(&timeout_rejected, (uint32_t)atomic_get(&rejected) - valid_rejected);
            }
#if INPUT_ENABLED
            if (reset_requested) atomic_inc(&requested_resets);
            else if (connection.state == APEX_CONNECTION_ESTABLISHED) atomic_inc(&link_timeouts);
#endif
            rc = new_session();
            if (rc) goto failed;
            last_valid = now;
            mark_valid_counters();
        }
        if (local_role == APEX_KEYBOARD && now >= next_send) {
            int n;
            if (connection.state == APEX_CONNECTION_ESTABLISHED) {
#if INPUT_ENABLED
                struct apex_input_frame frame;
                k_spinlock_key_t key = k_spin_lock(&input_lock);
                bool queued = apex_input_peek(&input_queue, &frame);
                k_spin_unlock(&input_lock, key);
                uint8_t payload[APEX_INPUT_CONSUMER_SIZE + 6];
                int length = queued ? apex_input_pack(&frame, payload, sizeof(payload)) : 1;
                if (!queued) payload[0] = APEX_INPUT_VERSION;
                n = apex_connection_encode(&connection, queued ? APEX_PACKET_INPUT : APEX_PACKET_KEEPALIVE,
                                           payload, length, tx, sizeof(tx));
#else
                uint8_t payload[4];
                sys_put_le32(++ping, payload);
                n = apex_connection_encode(&connection, APEX_PACKET_KEEPALIVE, payload,
                                           sizeof(payload), tx, sizeof(tx));
#endif
            } else n = apex_connection_request(&connection, tx, sizeof(tx));
            if (n > 0) {
                rc = radio_send(tx, n);
                if (rc) goto failed;
            }
            next_send = now + (INPUT_ENABLED ? 5 : 100);
        }
        maximum(&max_process_us, k_cyc_to_us_floor32(k_cycle_get_32() - loop_start));
#if EVENT_RX
        int64_t remaining = last_valid + timeout_ms - k_uptime_get();
        /* Idle wakeups keep timeout handling bounded without polling every ms. */
        k_sem_take(&radio_event, K_MSEC(MAX(1, MIN(remaining, 20))));
#else
        k_sleep(K_MSEC(1));
#endif
    }
failed:
#if INPUT_ENABLED
    if (local_role == APEX_RECEIVER) apex_radio_release();
#endif
    atomic_set(&error, rc);
    (void)radio_stop();
    apex_connection_clear(&connection);
    atomic_set(&state, APEX_CONNECTION_OFF);
}
K_THREAD_DEFINE(apex_probe_tid, 3072, probe_thread, NULL, NULL, NULL, 8, 0, 0);

void apex_radio_probe_start(enum apex_role role)
{
    if (!atomic_cas(&running, 0, 1)) return;
    local_role = role;
    k_sem_give(&start_signal);
}

int apex_radio_probe_status(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
#if EVENT_RX
    shell_print(sh, "RX_WAKE interrupts=%ld", (long)atomic_get(&rx_interrupts));
#endif
    shell_print(sh, "TIMING loop_max_us=%ld processing_max_us=%ld loops_over_10ms=%ld timeout_loop_us=%ld timeout_silence_ms=%ld",
                (long)atomic_get(&max_loop_us), (long)atomic_get(&max_process_us),
                (long)atomic_get(&loops_over_10ms), (long)atomic_get(&timeout_loop_us),
                (long)atomic_get(&timeout_silence_ms));
    shell_print(sh, "TIMEOUT_ACTIVITY tx=%ld rx=%ld crc=%ld rejected=%ld",
                (long)atomic_get(&timeout_tx), (long)atomic_get(&timeout_rx),
                (long)atomic_get(&timeout_crc), (long)atomic_get(&timeout_rejected));
#if INPUT_ENABLED
    k_spinlock_key_t key = k_spin_lock(&input_lock);
    unsigned int queued = input_queue.count;
    k_spin_unlock(&input_lock, key);
    shell_print(sh, "INPUT selected=%ld queued=%u acked=%ld overflow=%ld rx_sequence=%lu",
                (long)atomic_get(&input_selected), queued, (long)atomic_get(&input_acked),
                (long)atomic_get(&queue_overflows), (unsigned long)atomic_get(&rx_sequence));
    shell_print(sh, "DELIVERY total=%ld duplicate=%ld usb_wait=%ld link_timeout=%ld requested_reset=%ld",
                (long)atomic_get(&input_delivered), (long)atomic_get(&duplicate_reports),
                (long)atomic_get(&usb_waits), (long)atomic_get(&link_timeouts),
                (long)atomic_get(&requested_resets));
    shell_print(sh, "AES hardware_blocks=%lu software_fallback=%lu",
                (unsigned long)apex_aes_blocks(), (unsigned long)apex_aes_fallbacks());
#endif
    shell_print(sh, "APX_RF_TEST role=%u started=%ld state=%ld sessions=%ld tx=%ld rx=%ld auth=%ld replay=%ld rejected=%ld error=%ld",
                local_role, (long)atomic_get(&running), (long)atomic_get(&state),
                (long)atomic_get(&sessions), (long)atomic_get(&transmitted),
                (long)atomic_get(&received), (long)atomic_get(&authenticated),
                (long)atomic_get(&replay_rejected), (long)atomic_get(&rejected),
                (long)atomic_get(&error));
    shell_print(sh, "PHY state=%lu mode=%lu freq=%lu pcnf0=%08lx pcnf1=%08lx crc=%lu address=%lu end=%lu hf=%08lx crc_errors=%ld",
                (unsigned long)NRF_RADIO->STATE, (unsigned long)NRF_RADIO->MODE,
                (unsigned long)NRF_RADIO->FREQUENCY, (unsigned long)NRF_RADIO->PCNF0,
                (unsigned long)NRF_RADIO->PCNF1, (unsigned long)NRF_RADIO->CRCSTATUS,
                (unsigned long)NRF_RADIO->EVENTS_ADDRESS, (unsigned long)NRF_RADIO->EVENTS_END,
                (unsigned long)NRF_CLOCK->HFCLKSTAT, (long)atomic_get(&crc_errors));
    shell_print(sh, "RX header=%08lx crc=%06lx crccnf=%lx poly=%lx init=%lx white=%lx",
                (unsigned long)atomic_get(&last_header), (unsigned long)atomic_get(&last_crc),
                (unsigned long)NRF_RADIO->CRCCNF, (unsigned long)NRF_RADIO->CRCPOLY,
                (unsigned long)NRF_RADIO->CRCINIT, (unsigned long)NRF_RADIO->DATAWHITEIV);
    return 0;
}
