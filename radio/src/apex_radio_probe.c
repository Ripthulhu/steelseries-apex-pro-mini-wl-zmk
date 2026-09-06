/* SPDX-License-Identifier: MIT */
/* Radio development transport. Not the production input scheduler. */
#include "apex_connection.h"
#include "apex_pair.h"
#include "apex_radio_probe.h"
#include "apex_radio_input.h"
#include "apex_aes.h"
#include "apex_hop_link.h"
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <hal/nrf_radio.h>
#include <zephyr/irq.h>

#define EVENT_RX IS_ENABLED(CONFIG_APEX_RECEIVER_RADIO_EVENT_RX)
#define HOP_ENABLED IS_ENABLED(CONFIG_APEX_RADIO_HOPPING)
#if HOP_ENABLED
BUILD_ASSERT(!IS_ENABLED(CONFIG_NRFX_TIMER2) && !IS_ENABLED(CONFIG_NRFX_PPI),
             "Radio timing owns TIMER2 and PPI after Bluetooth shutdown");
static struct apex_hop_link hop_link;
static uint64_t timer_high, discovery_epoch;
static uint32_t timer_last;
static atomic_t hop_changes, scan_changes, sync_sent, sync_missed, stamp_error_max;
static atomic_t hop_live, current_channel, channel_seen;
static atomic_t hop_rx[4], clock_losses, control_rejected;
static atomic_t sync_received, sync_gap_max, control_error, control_error_age;
static atomic_t control_error_at, clock_loss_age;

static uint64_t radio_time_us(void)
{
    NRF_TIMER2->TASKS_CAPTURE[3] = 1;
    uint32_t low = NRF_TIMER2->CC[3];
    if (low < timer_last) timer_high += UINT64_C(1) << 32;
    timer_last = low;
    return timer_high | low;
}

static uint64_t radio_event_us(void)
{
    uint64_t now = radio_time_us();
    return now - (uint32_t)((uint32_t)now - NRF_TIMER2->CC[2]);
}

static void radio_timer_init(void)
{
    NRF_PPI->CHENCLR = BIT(17) | BIT(18);
    NRF_TIMER2->TASKS_STOP = 1;
    NRF_TIMER2->MODE = TIMER_MODE_MODE_Timer;
    NRF_TIMER2->BITMODE = TIMER_BITMODE_BITMODE_32Bit;
    NRF_TIMER2->PRESCALER = 4; /* 16 MHz / 16 = 1 MHz. */
    NRF_TIMER2->SHORTS = 0;
    NRF_TIMER2->INTENCLR = UINT32_MAX;
    NRF_TIMER2->TASKS_CLEAR = 1;
    NRF_TIMER2->TASKS_START = 1;
    NRF_PPI->CH[17].EEP = (uint32_t)&NRF_RADIO->EVENTS_END;
    NRF_PPI->CH[17].TEP = (uint32_t)&NRF_TIMER2->TASKS_CAPTURE[2];
    NRF_PPI->FORK[17].TEP = 0;
    NRF_PPI->CH[18].EEP = (uint32_t)&NRF_TIMER2->EVENTS_COMPARE[1];
    NRF_PPI->CH[18].TEP = (uint32_t)&NRF_RADIO->TASKS_TXEN;
    NRF_PPI->FORK[18].TEP = 0;
    NRF_PPI->CHENSET = BIT(17);
}
#endif
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
static atomic_t completion_acks, completion_ack_max_us;
static uint32_t pending_usb_sequence;
static atomic_t pause_requested, paused;
static atomic_t link_timeouts, requested_resets;
enum { RESET_QUEUE = 1, RESET_SELECTION = 2, RESET_COMMAND = 4, RESET_START = 8 };
static atomic_t reset_reason, reset_at, timeout_at;
__weak int apex_radio_deliver(const struct apex_input_frame *frame) { return -ENOTSUP; }
__weak uint32_t apex_radio_completed(uint32_t *completed_at) { *completed_at = 0; return 0; }
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
        atomic_or(&resync, RESET_QUEUE);
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
    atomic_or(&resync, RESET_SELECTION);
}

bool apex_radio_input_connected(void)
{
#if HOP_ENABLED
    if (!atomic_get(&hop_live)) return false;
#endif
    return atomic_get(&state) == APEX_CONNECTION_ESTABLISHED;
}

void apex_radio_request_session(void)
{
    atomic_or(&resync, RESET_COMMAND);
#if EVENT_RX
    k_sem_give(&radio_event);
#endif
}

void apex_radio_delivery_notify(void)
{
#if EVENT_RX
    k_sem_give(&radio_event);
#endif
}

bool apex_radio_pause(void)
{
    if (!atomic_get(&running) || atomic_get(&error)) return false;
    atomic_set(&pause_requested, 1);
    apex_radio_delivery_notify();
    int64_t deadline = k_uptime_get() + 50;
    do {
        if (atomic_get(&paused)) return true;
        k_sleep(K_MSEC(1));
    } while (k_uptime_get() < deadline);
    apex_radio_resume();
    return false;
}

void apex_radio_resume(void)
{
    atomic_set(&pause_requested, 0);
    apex_radio_delivery_notify();
}

static int input_ack(uint32_t sequence)
{
    uint8_t reply[6] = {APEX_INPUT_VERSION};
    sys_put_le32(sequence, reply + 1);
    reply[5] = apex_radio_host_leds();
    return apex_connection_encode(&connection, APEX_PACKET_ACK, reply, sizeof(reply), tx, sizeof(tx));
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
                /* Give USB completion the first reply opportunity. Two ACKs
                 * back-to-back can outrun the keyboard's receive rearm. A
                 * retry still receives a liveness ACK while USB is stalled. */
                if (pending_usb_sequence != frame.sequence) {
                    pending_usb_sequence = frame.sequence;
                    return 0;
                }
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
    return input_ack(ack);
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

static int radio_send(const uint8_t *packet, size_t length, uint64_t scheduled_end_us)
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
#if HOP_ENABLED
    if (scheduled_end_us) {
        /* One-byte preamble, five-byte address, length byte and three-byte CRC.
         * Fast ramp-up is 40 us; Nordic 2 Mbit mode sends each byte in 4 us. */
        uint64_t start_us = scheduled_end_us - (40 + (length + 10) * 4);
        NRF_TIMER2->CC[1] = (uint32_t)start_us;
        NRF_TIMER2->EVENTS_COMPARE[1] = 0;
        if (radio_time_us() + 100 >= start_us) {
            atomic_inc(&sync_missed);
            radio_receive();
            return -EAGAIN;
        }
        NRF_PPI->CHENSET = BIT(18);
    } else
#else
    ARG_UNUSED(scheduled_end_us);
#endif
    NRF_RADIO->TASKS_TXEN = 1;
    uint32_t start = k_cycle_get_32();
    while (!NRF_RADIO->EVENTS_DISABLED) {
        if (k_cyc_to_us_floor32(k_cycle_get_32() - start) > (scheduled_end_us ? 5000 : 2000)) {
#if HOP_ENABLED
            NRF_PPI->CHENCLR = BIT(18);
#endif
            (void)radio_stop();
            return -ETIMEDOUT;
        }
    }
#if HOP_ENABLED
    NRF_PPI->CHENCLR = BIT(18);
    if (scheduled_end_us) {
        int32_t delta = (uint32_t)NRF_TIMER2->CC[2] - (uint32_t)scheduled_end_us;
        maximum(&stamp_error_max, delta < 0 ? -delta : delta);
        atomic_inc(&sync_sent);
    }
#endif
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
#if HOP_ENABLED
    NRF_RADIO->MODECNF0 = RADIO_MODECNF0_RU_Fast;
    radio_timer_init();
#endif
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
#if HOP_ENABLED
    int stop_rc = radio_stop();
    if (stop_rc) return stop_rc;
    memset(&hop_link, 0, sizeof(hop_link));
    atomic_set(&hop_live, 0);
    discovery_epoch = radio_time_us();
    NRF_RADIO->FREQUENCY = apex_hop_discovery_channel(local_role, 0);
    atomic_set(&current_channel, NRF_RADIO->FREQUENCY);
    radio_receive();
#endif
#if INPUT_ENABLED
    k_spinlock_key_t key = k_spin_lock(&input_lock);
    apex_input_disconnect(&input_queue);
    k_spin_unlock(&input_lock, key);
    atomic_set(&rx_sequence, 0);
    pending_usb_sequence = 0;
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
#if INPUT_ENABLED
        if (atomic_get(&pause_requested)) {
            rc = radio_stop();
            if (rc) goto failed;
#if HOP_ENABLED
            atomic_set(&hop_live, 0);
#endif
            atomic_set(&paused, 1);
            while (atomic_get(&pause_requested)) k_sleep(K_MSEC(1));
            atomic_set(&paused, 0);
            rc = new_session();
            if (rc) goto failed;
            last_valid = k_uptime_get();
            next_send = 0;
            last_loop = k_cycle_get_32();
            mark_valid_counters();
        }
#endif
        uint32_t loop_start = k_cycle_get_32();
        uint32_t loop_us = k_cyc_to_us_floor32(loop_start - last_loop);
        last_loop = loop_start;
        maximum(&max_loop_us, loop_us);
        if (loop_us > 10000) atomic_inc(&loops_over_10ms);
        int64_t now = k_uptime_get();
        if (receiving && NRF_RADIO->EVENTS_END) {
#if HOP_ENABLED
            uint64_t received_us = radio_event_us();
#endif
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
                }
#if HOP_ENABLED
                else if (rx[1] == APEX_PACKET_CONTROL || rx[1] == APEX_PACKET_CHANNEL_MAP) {
                    bool had_clock = hop_link.clock.running;
                    uint64_t age = had_clock && received_us >= hop_link.clock.anchor_us ?
                                   received_us - hop_link.clock.anchor_us : 0;
                    n = apex_hop_link_receive(&hop_link, &connection, rx, length, received_us, tx, sizeof(tx));
                    if (n >= 0) {
                        atomic_inc(&authenticated);
                        if (local_role == APEX_RECEIVER && rx[1] == APEX_PACKET_CONTROL) {
                            atomic_inc(&sync_received);
                            if (had_clock) maximum(&sync_gap_max, MIN(age, INT32_MAX));
                        }
                    } else {
                        atomic_inc(&control_rejected);
                        atomic_set(&control_error, n);
                        atomic_set(&control_error_age, MIN(age, INT32_MAX));
                        atomic_set(&control_error_at, now);
                    }
                }
#endif
                else {
                    uint8_t type = 0;
                    n = apex_connection_decode(&connection, rx, length, &type, plain, sizeof(plain));
                    if (n >= 0) {
                        atomic_inc(&authenticated);
#if INPUT_ENABLED
#if HOP_ENABLED
                        if (!apex_hop_link_ready(&hop_link, received_us)) n = -EINVAL;
                        else
#endif
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
#if HOP_ENABLED
                    if (apex_hop_link_ready(&hop_link, received_us)) {
                        const uint8_t map[] = {6, 26, 50, 74};
                        for (unsigned int i = 0; i < sizeof(map); i++) {
                            if (NRF_RADIO->FREQUENCY == map[i]) atomic_inc(&hop_rx[i]);
                        }
                    }
#endif
                    last_valid = now;
                    mark_valid_counters();
                    if (previous != APEX_CONNECTION_ESTABLISHED &&
                        connection.state == APEX_CONNECTION_ESTABLISHED) {
                        atomic_inc(&sessions);
#if HOP_ENABLED
                        rc = apex_hop_link_init(&hop_link, &connection, NRF_RADIO->FREQUENCY);
                        if (rc) goto failed;
#endif
#if INPUT_ENABLED
                        if (local_role == APEX_KEYBOARD) {
                            k_spinlock_key_t key = k_spin_lock(&input_lock);
                            apex_input_session(&input_queue);
                            k_spin_unlock(&input_lock, key);
                        }
#endif
                    }
                    if (n > 0) {
                        rc = radio_send(tx, n, 0);
                        if (rc) goto failed;
                    }
                } else atomic_inc(&rejected);
                atomic_set(&state, connection.state);
            }
            if (!receiving) radio_receive();
        }
        uint32_t reset_requested = 0;
        int timeout_ms = local_role == APEX_KEYBOARD ? 5500 : 4000;
#if HOP_ENABLED
        timeout_ms = (connection.state == APEX_CONNECTION_WAIT_HELLO ||
                      connection.state == APEX_CONNECTION_WAIT_CHALLENGE) ? 1500 : 400;
        bool hop_expired = hop_link.clock.running &&
                           apex_hop_link_channel(&hop_link, radio_time_us()) < 0;
#else
        bool hop_expired = false;
#endif
#if INPUT_ENABLED
        reset_requested = atomic_set(&resync, 0);
        if (connection.state == APEX_CONNECTION_ESTABLISHED) timeout_ms = 100;
#endif
        if (reset_requested || hop_expired || now - last_valid >= timeout_ms) {
#if HOP_ENABLED
            if (hop_expired) {
                atomic_inc(&clock_losses);
                atomic_set(&clock_loss_age, MIN(radio_time_us() - hop_link.clock.anchor_us, INT32_MAX));
            }
#endif
            if (!reset_requested && connection.state == APEX_CONNECTION_ESTABLISHED) {
                atomic_set(&timeout_loop_us, loop_us);
                atomic_set(&timeout_silence_ms, now - last_valid);
#if INPUT_ENABLED
                atomic_set(&timeout_at, now);
#endif
                /* Activity since the last accepted packet, before session reset. */
                atomic_set(&timeout_tx, (uint32_t)atomic_get(&transmitted) - valid_tx);
                atomic_set(&timeout_rx, (uint32_t)atomic_get(&received) - valid_rx);
                atomic_set(&timeout_crc, (uint32_t)atomic_get(&crc_errors) - valid_crc);
                atomic_set(&timeout_rejected, (uint32_t)atomic_get(&rejected) - valid_rejected);
            }
#if INPUT_ENABLED
            if (reset_requested) {
                atomic_inc(&requested_resets);
                atomic_set(&reset_reason, reset_requested);
                atomic_set(&reset_at, now);
            }
            else if (connection.state == APEX_CONNECTION_ESTABLISHED) atomic_inc(&link_timeouts);
#endif
            rc = new_session();
            if (rc) goto failed;
            last_valid = now;
            mark_valid_counters();
        }
#if HOP_ENABLED
        uint64_t clock_now = radio_time_us();
        int channel = NRF_RADIO->FREQUENCY;
        if (connection.state == APEX_CONNECTION_ESTABLISHED) {
            channel = apex_hop_link_channel(&hop_link, clock_now);
            atomic_set(&hop_live, apex_hop_link_ready(&hop_link, clock_now));
        } else if (connection.state == APEX_CONNECTION_WAIT_HELLO ||
                   connection.state == APEX_CONNECTION_WAIT_CHALLENGE) {
            channel = apex_hop_discovery_channel(local_role, clock_now - discovery_epoch);
        }
        if (channel >= 0 && (uint32_t)channel != NRF_RADIO->FREQUENCY) {
            rc = radio_stop();
            if (rc) goto failed;
            NRF_RADIO->FREQUENCY = channel;
            atomic_set(&current_channel, channel);
            if (atomic_get(&hop_live)) atomic_inc(&hop_changes);
            else atomic_inc(&scan_changes);
            radio_receive();
        }
        if (atomic_get(&hop_live)) {
            const uint8_t map[] = {6, 26, 50, 74};
            for (unsigned int i = 0; i < sizeof(map); i++) {
                if (channel == map[i]) atomic_or(&channel_seen, BIT(i));
            }
        }
#endif
#if INPUT_ENABLED
        /* USB callbacks only wake this thread. It owns the session, cipher
         * counters and radio, including acknowledgements after USB completion. */
        if (local_role == APEX_RECEIVER && apex_radio_input_connected()
#if HOP_ENABLED
            && apex_hop_link_window(&hop_link, radio_time_us())
#endif
            && !(receiving && NRF_RADIO->EVENTS_END)) {
            uint32_t completed_at;
            uint32_t sequence = apex_radio_completed(&completed_at);
            if (sequence && sequence == (uint32_t)atomic_get(&rx_sequence) + 1) {
                atomic_set(&rx_sequence, sequence);
                atomic_inc(&input_delivered);
                int n = input_ack(sequence);
                if (n < 0) { rc = n; goto failed; }
                rc = radio_send(tx, n, 0);
                if (rc) goto failed;
                atomic_inc(&completion_acks);
                maximum(&completion_ack_max_us,
                        k_cyc_to_us_floor32(k_cycle_get_32() - completed_at));
            }
        }
#endif
        if (local_role == APEX_KEYBOARD && now >= next_send) {
            int n;
            uint64_t scheduled_end = 0;
            if (connection.state == APEX_CONNECTION_ESTABLISHED) {
#if INPUT_ENABLED
#if HOP_ENABLED
                if (!apex_hop_link_window(&hop_link, radio_time_us())) goto wait_next;
                scheduled_end = radio_time_us() + 2000 + 40 + (36 + 10) * 4;
                n = apex_hop_link_next(&hop_link, &connection, scheduled_end, tx, sizeof(tx));
                if (n < 0) {
                    atomic_or(&resync, RESET_START);
                    goto wait_next;
                }
                if (n > 0) {
                    if (tx[1] != APEX_PACKET_CONTROL) scheduled_end = 0;
                    goto transmit;
                }
                scheduled_end = 0;
#endif
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
#if HOP_ENABLED
transmit:
#endif
            if (n > 0) {
                rc = radio_send(tx, n, scheduled_end);
                if (rc == -EAGAIN) goto wait_next;
                if (rc) goto failed;
            }
            next_send = now + (INPUT_ENABLED ? 5 : 100);
        }
wait_next:
        maximum(&max_process_us, k_cyc_to_us_floor32(k_cycle_get_32() - loop_start));
#if EVENT_RX
        int64_t remaining = last_valid + timeout_ms - k_uptime_get();
        /* Hopping needs channel checks between packets; fixed-channel RX can sleep longer. */
        k_sem_take(&radio_event, K_MSEC(MAX(1, MIN(remaining, HOP_ENABLED ? 1 : 20))));
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
#if HOP_ENABLED
    NRF_PPI->CHENCLR = BIT(17) | BIT(18);
    NRF_TIMER2->TASKS_STOP = 1;
#endif
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
#if HOP_ENABLED
    shell_print(sh, "HOP active=%ld channel=%ld seen=%lx changes=%ld scan=%ld sync_tx=%ld missed=%ld stamp_error_us=%ld",
                (long)atomic_get(&hop_live), (long)atomic_get(&current_channel),
                (unsigned long)atomic_get(&channel_seen), (long)atomic_get(&hop_changes),
                (long)atomic_get(&scan_changes), (long)atomic_get(&sync_sent),
                (long)atomic_get(&sync_missed), (long)atomic_get(&stamp_error_max));
    shell_print(sh, "HOP_RX c6=%ld c26=%ld c50=%ld c74=%ld clock_lost=%ld control_rejected=%ld",
                (long)atomic_get(&hop_rx[0]), (long)atomic_get(&hop_rx[1]),
                (long)atomic_get(&hop_rx[2]), (long)atomic_get(&hop_rx[3]),
                (long)atomic_get(&clock_losses), (long)atomic_get(&control_rejected));
    shell_print(sh, "HOP_SYNC received=%ld gap_max_us=%ld error=%ld error_age_us=%ld error_at_ms=%ld loss_age_us=%ld",
                (long)atomic_get(&sync_received), (long)atomic_get(&sync_gap_max),
                (long)atomic_get(&control_error), (long)atomic_get(&control_error_age),
                (long)atomic_get(&control_error_at), (long)atomic_get(&clock_loss_age));
#endif
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
    shell_print(sh, "RESET reason=%ld at_ms=%ld timeout_at_ms=%ld uptime_ms=%lld",
                (long)atomic_get(&reset_reason), (long)atomic_get(&reset_at),
                (long)atomic_get(&timeout_at), (long long)k_uptime_get());
    shell_print(sh, "ACK completion_tx=%ld completion_to_tx_max_us=%ld",
                (long)atomic_get(&completion_acks), (long)atomic_get(&completion_ack_max_us));
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
