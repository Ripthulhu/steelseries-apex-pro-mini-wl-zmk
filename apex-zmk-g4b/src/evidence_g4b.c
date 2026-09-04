/* SPDX-License-Identifier: MIT */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <nrfx.h>

#include "evidence_g4b.h"

#if IS_ENABLED(CONFIG_APEX_G4B_EVIDENCE_USB) && DT_NODE_EXISTS(DT_NODELABEL(dfu_cdc))
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>

/* Mirror text evidence to the dfu_cdc CDC-ACM so it is readable over USB
 * (COM port) with no FTDI on P0.10. The device-next CDC driver only permits
 * uart_fifo_fill() from its own IRQ work callback; calling it from this thread
 * returns zero. uart_poll_out() is its supported arbitrary-context producer
 * and, with CDC flow control off, drops immediately rather than waiting when
 * the ring is full. Reading the DFU CDC at any baud other than 1200 shows the
 * lines. */
static const struct device *const g4b_evi_cdc = DEVICE_DT_GET(DT_NODELABEL(dfu_cdc));

static void g4b_evidence_usb_write(const uint8_t *data, uint32_t len)
{
	if (!device_is_ready(g4b_evi_cdc)) {
		return;
	}
	for (uint32_t off = 0u; off < len; off++) {
		uart_poll_out(g4b_evi_cdc, data[off]);
	}
}
#else
static inline void g4b_evidence_usb_write(const uint8_t *data, uint32_t len)
{
	ARG_UNUSED(data);
	ARG_UNUSED(len);
}
#endif

/* UARTE0 on P0.10, 19200 8N1 - the same line and rate the recovery wrapper
 * uses for its APXB beacon, so one capture sees both.
 */
#define G4B_UART_TX_PIN 10u
#define G4B_UART_BAUD_19200 0x004EA000u
/* Stage 5 only. 114,688 bytes at 19200 is 59.7 s against a 60 s watchdog;
 * at 115200 a full pass is 10 s. */
#define G4B_UART_BAUD_115200 0x01D7E000u
#define G4B_PSEL_DISCONNECTED 0xFFFFFFFFu
#define G4B_UART_GUARD 0x00400000u

/* Cortex-M DWT. Defined here rather than pulled from a CMSIS header so the
 * exact registers being touched are visible at the point of use.
 */
#define G4B_DEMCR (*(volatile uint32_t *)0xE000EDFCu)
#define G4B_DWT_CTRL (*(volatile uint32_t *)0xE0001000u)
#define G4B_DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)
#define G4B_DEMCR_TRCENA BIT(24)
#define G4B_DWT_CTRL_CYCCNTENA BIT(0)

/* EasyDMA can only read from RAM. A const buffer in flash faults the transfer
 * silently - nrfx guards against exactly this, and the direct-register path
 * used here has no such guard, so the staging buffer must be static.
 */
static uint8_t g4b_tx_buffer[sizeof(struct g4b_record)];
static uint8_t g4b_tx_buffer_s1[sizeof(struct g4b_record_s1)];
#if CONFIG_APEX_G4B_STAGE == 2
static uint8_t g4b_tx_buffer_s2r[sizeof(struct g4b_record_s2_replay)];
static uint8_t g4b_tx_buffer_s2c[sizeof(struct g4b_record_s2_confirm)];
#endif
#if CONFIG_APEX_G4B_STAGE == 3
static uint8_t g4b_tx_buffer_s3[sizeof(struct g4b_record_s3)];
#endif
#if CONFIG_APEX_G4B_STAGE == 4
static uint8_t g4b_tx_buffer_s4[sizeof(struct g4b_record_s4)];
#endif
#if CONFIG_APEX_G4B_STAGE == 6
static uint8_t g4b_tx_buffer_s6[sizeof(struct g4b_record_s6)];
#endif
#if CONFIG_APEX_G4B_STAGE == 7
static uint8_t g4b_tx_buffer_s7[sizeof(struct g4b_record_s7)];
#endif

/* Record sizes are pinned here because the host decoder derives its own layout
 * independently. If one side changes without the other, a capture is silently
 * misparsed rather than rejected - so make the build fail instead.
 * decode_apxg.py asserts the same numbers.
 */
BUILD_ASSERT(sizeof(struct g4b_record) == 88, "stage-0 record layout changed");
BUILD_ASSERT(sizeof(struct g4b_record_s1) == 204, "stage-1 record layout changed");
BUILD_ASSERT(sizeof(struct g4b_s2_head) == 32, "stage-2 header layout changed");
BUILD_ASSERT(sizeof(struct g4b_exchange_record) == 88, "exchange record layout changed");
#if CONFIG_APEX_G4B_STAGE == 2
BUILD_ASSERT(sizeof(struct g4b_record_s2_replay) == 580, "stage-2 replay record layout changed");
BUILD_ASSERT(sizeof(struct g4b_record_s2_confirm) == 212, "stage-2 confirm record layout changed");
#endif
#if CONFIG_APEX_G4B_STAGE == 3
BUILD_ASSERT(sizeof(struct g4b_record_s3) == 1280, "stage-3 record layout changed");
#endif
#if CONFIG_APEX_G4B_STAGE == 4
BUILD_ASSERT(sizeof(struct g4b_record_s4) == 460, "stage-4 record layout changed");
#endif
#if CONFIG_APEX_G4B_STAGE == 6
BUILD_ASSERT(sizeof(struct g4b_record_s6) == 136, "stage-6 record layout changed");
#endif
#if CONFIG_APEX_G4B_STAGE == 7
BUILD_ASSERT(sizeof(struct g4b_record_s7) == 124, "stage-7 record layout changed");
#endif

uint32_t g4b_cyccnt(void)
{
    return G4B_DWT_CYCCNT;
}

void g4b_evidence_init(void)
{
    G4B_DEMCR |= G4B_DEMCR_TRCENA;
    G4B_DWT_CYCCNT = 0u;
    G4B_DWT_CTRL |= G4B_DWT_CTRL_CYCCNTENA;
}

static void uart_open(void)
{
#if !IS_ENABLED(CONFIG_APEX_G4B_UART_EMIT)
    return;
#endif
    NRF_UARTE0->TASKS_STOPTX = 1u;
    __DSB();
    NRF_UARTE0->ENABLE = 0u;
    NRF_UARTE0->EVENTS_ENDTX = 0u;
    NRF_UARTE0->EVENTS_TXSTOPPED = 0u;

    NRF_UARTE0->PSEL.RTS = G4B_PSEL_DISCONNECTED;
    NRF_UARTE0->PSEL.CTS = G4B_PSEL_DISCONNECTED;
    NRF_UARTE0->PSEL.RXD = G4B_PSEL_DISCONNECTED;
    NRF_UARTE0->PSEL.TXD = G4B_UART_TX_PIN;

    NRF_UARTE0->BAUDRATE = G4B_UART_BAUD_19200;
    NRF_UARTE0->CONFIG = 0u; /* 8N1, no flow control, no parity */
    NRF_UARTE0->ENABLE = 8u; /* Enabled */
}

static void uart_close(void)
{
#if !IS_ENABLED(CONFIG_APEX_G4B_UART_EMIT)
    return;
#endif
    /* EVENTS_ENDTX fires when EasyDMA has read the last byte out of RAM, NOT
     * when that byte has finished shifting out of the pin. Asserting STOPTX on
     * ENDTX therefore truncates the final byte mid-shift.
     *
     * A truncated final byte corrupts the record tail, including full response
     * frames used by later diagnostic stages.
     *
     * Wait for EVENTS_TXSTOPPED, which is the event that means the shift
     * register has actually drained.
     */
    uint32_t guard = G4B_UART_GUARD;

    NRF_UARTE0->EVENTS_TXSTOPPED = 0u;
    NRF_UARTE0->TASKS_STOPTX = 1u;
    __DSB();

    while (NRF_UARTE0->EVENTS_TXSTOPPED == 0u && guard-- != 0u) {
        /* spin */
    }

    NRF_UARTE0->ENABLE = 0u;
    NRF_UARTE0->EVENTS_ENDTX = 0u;
    NRF_UARTE0->EVENTS_TXSTOPPED = 0u;
    NRF_UARTE0->PSEL.TXD = G4B_PSEL_DISCONNECTED;
}

static void uart_write(const uint8_t *data, uint32_t length)
{
#if !IS_ENABLED(CONFIG_APEX_G4B_UART_EMIT)
    return;
#endif
    /* Bounded wait. At 19200 baud a 240-byte record takes roughly 125 ms; the
     * loop limit is far beyond that and exists only so a stuck peripheral
     * cannot hang the stage thread.
     *
     * TX remains active between writes. uart_close() performs the single stop
     * and waits for TXSTOPPED.
     */
    uint32_t guard = G4B_UART_GUARD;

    NRF_UARTE0->EVENTS_ENDTX = 0u;
    NRF_UARTE0->TXD.PTR = (uint32_t)data;
    NRF_UARTE0->TXD.MAXCNT = length;
    __DSB();
    NRF_UARTE0->TASKS_STARTTX = 1u;

    while (NRF_UARTE0->EVENTS_ENDTX == 0u && guard-- != 0u) {
        /* spin */
    }
}

/* One short ASCII line, for measuring rather than decoding.
 *
 * The binary records are the right shape for structured evidence, but adding a
 * field to one means touching the struct, the record size, the decoder and
 * three separate version filters. Boot timing does not need that ceremony: it
 * needs four numbers, once,
 * in a form that is readable in the raw capture with no decoder at all.
 *
 * `text` must live in RAM. UARTE uses EasyDMA and cannot read flash, so a
 * string literal passed straight in would transmit nothing.
 */
void g4b_evidence_emit_text(const uint8_t *text, uint32_t length)
{
    if (text == NULL || length == 0u) {
        return;
    }

    uart_open();
    uart_write(text, length);
    uart_close();

    g4b_evidence_usb_write(text, length);
}

void g4b_evidence_emit(const struct g4b_record *record)
{
    if (record == NULL) {
        return;
    }

    memcpy(g4b_tx_buffer, record, sizeof(g4b_tx_buffer));

    uart_open();
    /* Emitted twice, as the stock-derived APXS capture does, so a single
     * corrupted copy is recoverable from the other.
     */
    uart_write(g4b_tx_buffer, sizeof(g4b_tx_buffer));
    uart_write(g4b_tx_buffer, sizeof(g4b_tx_buffer));
    uart_close();
}

void g4b_evidence_emit_s1(const struct g4b_record_s1 *record)
{
    if (record == NULL) {
        return;
    }

    memcpy(g4b_tx_buffer_s1, record, sizeof(g4b_tx_buffer_s1));

    uart_open();
    uart_write(g4b_tx_buffer_s1, sizeof(g4b_tx_buffer_s1));
    uart_write(g4b_tx_buffer_s1, sizeof(g4b_tx_buffer_s1));
    uart_close();
}

#if CONFIG_APEX_G4B_STAGE == 2
/* 492 bytes twice is about 512 ms at 19200. A launch has a full 60 s window -
 * measured intervals are 60.59 to 60.77 s across three runs - so this is not
 * close to any limit, but it is why the replay record carries only the frame
 * that failed rather than all 59 responses.
 */
void g4b_evidence_emit_s2_replay(const struct g4b_record_s2_replay *record)
{
    if (record == NULL) {
        return;
    }

    memcpy(g4b_tx_buffer_s2r, record, sizeof(g4b_tx_buffer_s2r));

    uart_open();
    uart_write(g4b_tx_buffer_s2r, sizeof(g4b_tx_buffer_s2r));
    uart_write(g4b_tx_buffer_s2r, sizeof(g4b_tx_buffer_s2r));
    uart_close();
}

void g4b_evidence_emit_s2_confirm(const struct g4b_record_s2_confirm *record)
{
    if (record == NULL) {
        return;
    }

    memcpy(g4b_tx_buffer_s2c, record, sizeof(g4b_tx_buffer_s2c));

    uart_open();
    uart_write(g4b_tx_buffer_s2c, sizeof(g4b_tx_buffer_s2c));
    uart_write(g4b_tx_buffer_s2c, sizeof(g4b_tx_buffer_s2c));
    uart_close();
}
#endif

#if CONFIG_APEX_G4B_STAGE == 4
void g4b_evidence_emit_s4(const struct g4b_record_s4 *record)
{
    if (record == NULL) {
        return;
    }

    memcpy(g4b_tx_buffer_s4, record, sizeof(g4b_tx_buffer_s4));

    uart_open();
    uart_write(g4b_tx_buffer_s4, sizeof(g4b_tx_buffer_s4));
    uart_write(g4b_tx_buffer_s4, sizeof(g4b_tx_buffer_s4));
    uart_close();
}
#endif

#if CONFIG_APEX_G4B_STAGE == 3
void g4b_evidence_emit_s3(const struct g4b_record_s3 *record)
{
    if (record == NULL) {
        return;
    }

    memcpy(g4b_tx_buffer_s3, record, sizeof(g4b_tx_buffer_s3));

    uart_open();
    uart_write(g4b_tx_buffer_s3, sizeof(g4b_tx_buffer_s3));
    uart_write(g4b_tx_buffer_s3, sizeof(g4b_tx_buffer_s3));
    uart_close();
}
#endif

#if CONFIG_APEX_G4B_STAGE == 5 || IS_ENABLED(CONFIG_APEX_G4B_SPINOR_DUMP)
/* The dump keeps the port open across all chunks. Opening and closing per chunk
 * would add a stop/start per 4 KB and risk truncating the record tail. Also
 * compiled for the read-only external
 * SPI-NOR dump (spinor_g4b.c), which reuses this exact chunk channel from a
 * Stage-3 keyboard build.
 */
static uint8_t g4b_tx_chunk[sizeof(struct g4b_dump_chunk)];

/* A copy of uart_open() with a different baud rather than a shared
 * parameterised version. Parameterising it changed code that every stage
 * compiles and broke byte-reproducibility of stage 1 and stage 4, both of which
 * had already passed on hardware. Duplication is the cheaper price.
 */
void g4b_evidence_dump_open(void)
{
    NRF_UARTE0->TASKS_STOPTX = 1u;
    __DSB();
    NRF_UARTE0->ENABLE = 0u;
    NRF_UARTE0->EVENTS_ENDTX = 0u;
    NRF_UARTE0->EVENTS_TXSTOPPED = 0u;

    NRF_UARTE0->PSEL.RTS = G4B_PSEL_DISCONNECTED;
    NRF_UARTE0->PSEL.CTS = G4B_PSEL_DISCONNECTED;
    NRF_UARTE0->PSEL.RXD = G4B_PSEL_DISCONNECTED;
    NRF_UARTE0->PSEL.TXD = G4B_UART_TX_PIN;

    NRF_UARTE0->BAUDRATE = G4B_UART_BAUD_115200;
    NRF_UARTE0->CONFIG = 0u;
    NRF_UARTE0->ENABLE = 8u;
}

void g4b_evidence_dump_chunk(const struct g4b_dump_chunk *chunk)
{
    if (chunk == NULL) {
        return;
    }

    memcpy(g4b_tx_chunk, chunk, sizeof(g4b_tx_chunk));
    uart_write(g4b_tx_chunk, sizeof(g4b_tx_chunk));
}

void g4b_evidence_dump_close(void)
{
    uart_close();
}
#endif

#if CONFIG_APEX_G4B_STAGE == 6
void g4b_evidence_emit_s6(const struct g4b_record_s6 *record)
{
    if (record == NULL) {
        return;
    }

    memcpy(g4b_tx_buffer_s6, record, sizeof(g4b_tx_buffer_s6));

    uart_open();
    uart_write(g4b_tx_buffer_s6, sizeof(g4b_tx_buffer_s6));
    uart_write(g4b_tx_buffer_s6, sizeof(g4b_tx_buffer_s6));
    uart_close();
}
#endif

#if CONFIG_APEX_G4B_STAGE == 7
void g4b_evidence_emit_s7(const struct g4b_record_s7 *record)
{
    if (record == NULL) {
        return;
    }

    memcpy(g4b_tx_buffer_s7, record, sizeof(g4b_tx_buffer_s7));

    uart_open();
    uart_write(g4b_tx_buffer_s7, sizeof(g4b_tx_buffer_s7));
    uart_write(g4b_tx_buffer_s7, sizeof(g4b_tx_buffer_s7));
    uart_close();
}
#endif
