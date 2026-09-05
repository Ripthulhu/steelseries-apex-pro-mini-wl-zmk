/* SPDX-License-Identifier: MIT
 *
 * Operational (post-pairing) 2.4 GHz link to the SteelSeries receiver. See
 * dongle_link_g4b.h for the overall shape and status.
 *
 * SCAFFOLD. The constants and frame layouts below are recovered facts recorded
 * in work/DONGLE_RE_2026-09-04.md (stock keyboard 272111140 and receiver
 * 1038:1624, traced on hardware 2026-09-04). The runtime state machine that ties
 * them together is a first cut: it is structured to reach and decode the
 * receiver's encrypted type-2 "switch to normal mode" message, which is the one
 * point where the current keyboard-to-dongle handoff fails.
 *
 * Two things are known-incomplete and marked TODO(hw) inline:
 *   1. TX->RX turnaround timing. Stock sequences each leg with TIMER2 CC
 *      0x310/0x360 + PPI; a software loop listens at the wrong instant even when
 *      the frames are correct. This module currently uses software timing.
 *   2. The exact CCM data-structure packing. The counter/direction/IV offsets
 *      here follow the nRF52833 CCM "encryption data structure" and the stock
 *      context layout (key +0x88, counter +0x98, direction +0xa0, IV +0xa4), but
 *      have not been byte-verified against a live decrypt.
 */

#include <string.h>

#include <zephyr/kernel.h>

#include <nrfx.h>

#include "evidence_g4b.h"
#include "radio_esb_g4b.h"
#include "dongle_link_g4b.h"

/* ======================================================================== *
 * Section 1 - recovered operational-link constants
 * ======================================================================== */

/* Receiver -> keyboard leg (RX). Private address, 38-byte static. */
#define DL_RX_BASE0     0x76412900u
#define DL_RX_PREFIX0   0x00000071u
#define DL_RX_STATLEN   38u

/* Keyboard -> receiver leg (TX). 19-byte static; the 4-byte CCM MIC is omitted on
 * air, so this direction is encrypted but not authenticated. */
#define DL_TX_BASE0     0x89BED600u
#define DL_TX_PREFIX0   0x0000008Eu
#define DL_TX_STATLEN   19u

/* Shared PHY (same as the pairing PHY except the address): Ble_2Mbit, PCNF0=0,
 * big-endian, whitening on (DATAWHITEIV=0x40), CRC-24 0x65B, operational CRCINIT
 * 0x00FFFFFF. PCNF1 top byte 0x03 = BALEN 3 (4-byte address) | ENDIAN big |
 * WHITEEN. */
#define DL_PCNF1_BASE   0x030300FEu /* | (STATLEN << 8) */
#define DL_CRCCNF       0x00000103u
#define DL_CRCPOLY      0x0000065Bu
/* Pairing-initial seed 1 and learned-link seed 0x087fc1 = fold(network id
 * 0xb7bfc001), both recovered from the stock dongle-link capture. The RX leg
 * cycles these two to lock whichever phase is on air. */
#define DL_CRCINIT_PAIR 0x00000001u
#define DL_CRCINIT_LINK 0x00087FC1u
#define DL_DATAWHITEIV  0x40u

/* Negotiated channel window for this receiver record: alternate 76, 2, starting
 * on 76 (from the class-0x80 config record `80 82 cc`). The window is assigned
 * per bond and lives only in RAM, so a different receiver may differ. */
static const uint8_t dl_channels[] = { 76u, 2u };

/* Slot schedule. The receiver picks a random 32-bit slot and streams it in the
 * clear; the CCM packet counter is (slot << 1) | phase, five bytes little-endian.
 * Phase toggles after each radio leg; the receiver advances the slot when phase
 * returns to zero. The slot period is 1600 us. */
#define DL_SLOT_PERIOD_US 1600u

/* Operational opcode classes (dispatch on frame[opcode] & 0xF0). */
#define DL_CLASS_90     0x90u /* clear: slot counter bytes 1..4 */
#define DL_CLASS_A0     0xA0u /* clear: slot counter byte 0 */
#define DL_CLASS_B0     0xB0u /* CCM: application link (type-2 handoff lives here) */

/* Slot-reconstruction progress. The keyboard records completion in these
 * sync-mask bits: A0 -> 0x10, first 90 -> 0x20, second 90 -> 0x40. */
#define DL_SYNC_A0      0x10u
#define DL_SYNC_90_LOW  0x20u
#define DL_SYNC_90_HIGH 0x40u
#define DL_SYNC_ALL     (DL_SYNC_A0 | DL_SYNC_90_LOW | DL_SYNC_90_HIGH)

/* CCM directions (nRF CCM direction bit). Receiver->keyboard = 0, keyboard->
 * receiver = 1. */
#define DL_DIR_RX 0u
#define DL_DIR_TX 1u

/* Transport control nibble (low nibble of the transport header):
 *   bit0 valid-link ACK, bit1 application payload, bit2 acknowledges the peer's
 *   bit3, bit3 local application sequence. Stock role 2 idles with 0x0d and
 *   uses 0x0f when data is queued; a payload is retransmitted until the peer's
 *   bit2 matches the sender's bit3. */
#define DL_CTRL_ACK      0x01u
#define DL_CTRL_PAYLOAD  0x02u
#define DL_CTRL_PEER_ACK 0x04u
#define DL_CTRL_LOCAL_SEQ 0x08u
#define DL_CTRL_IDLE     0x0Du
#define DL_CTRL_DATA     0x0Fu

/* Application report: 11 bytes = 72-bit absolute key bitmap (0..8), output mode
 * (9, value 2 for dongle), consumer/special bitmap (10). The transport prepends
 * an endpoint/type byte 0, giving 12 bytes, padded to the 16-byte CCM payload. */
#define DL_REPORT_BITMAP_BYTES 9u
#define DL_REPORT_MODE_DONGLE  2u
#define DL_APP_PAYLOAD_BYTES   16u

/* The receiver's type-2 application body is the pairing->normal handoff. In the
 * decrypted class-B0 body (direction 0), offsets 4-5 carry `02 00`. Missing or
 * mis-decoded, the keyboard stays in the pairing UI and no keys flow - this is
 * the current failure. */
#define DL_APP_TYPE_OFFSET 4u
#define DL_APP_TYPE_NORMAL 0x02u

/* ======================================================================== *
 * Section 2 - CCM peripheral (key/IV all-zero, no authentication on TX)
 * ======================================================================== *
 *
 * nRF52833 CCM "encryption data structure" pointed to by CNFPTR. The stock
 * context stages the same fields (key +0x88, counter +0x98, direction +0xa0,
 * IV +0xa4). Key and IV are all zero for this link; only the counter and
 * direction change per packet.
 *
 * TODO(hw): byte-verify this packing against a live decrypt of a known B0.
 */
struct dl_ccm_cnf {
	uint8_t key[16];      /* +0x00 : all zero */
	uint8_t counter[5];   /* +0x10 : (slot<<1)|phase, little-endian */
	uint8_t rfu[3];       /* +0x15 : reserved */
	uint8_t direction;    /* +0x18 : bit0, DL_DIR_* */
	uint8_t iv[8];        /* +0x19 : all zero */
} __packed;

static struct dl_ccm_cnf dl_ccm __aligned(4);
/* CCM scratch: Nordic requires >= (max on-air length) + 16 for extended length.
 * The 38-byte RX frame plus margin fits comfortably here. */
static uint8_t dl_ccm_scratch[128] __aligned(4);
/* Cleartext output of a decrypt; formatted [header, length, reserved, payload]. */
static uint8_t dl_ccm_out[64] __aligned(4);

/* The 5-byte packet counter, assembled in arrival order from A0 (byte 0), the
 * first 90 (bytes 1-2), and the second 90 (bytes 3-4). Section 3 fills it. */
static uint8_t dl_ctr[5];

static void dl_ccm_set_counter(void)
{
	memcpy(dl_ccm.counter, dl_ctr, sizeof(dl_ccm.counter));
}

/* Decrypt one received frame (INPTR = encrypted [hdr,len,rfu,payload,MIC]).
 * Returns 1 on MIC-OK, 0 on MIC-fail, output in dl_ccm_out. The receiver->
 * keyboard direction carries the MIC, so MICSTATUS is meaningful here. */
static int dl_ccm_decrypt(const uint8_t *in)
{
	dl_ccm_set_counter();
	dl_ccm.direction = DL_DIR_RX;

	NRF_CCM->ENABLE = 2u;               /* Enable */
	/* MODE: decrypt (1), 2Mbit datarate (1<<16), extended length (1<<24). */
	NRF_CCM->MODE = 1u | (1u << 16) | (1u << 24);
	NRF_CCM->CNFPTR = (uint32_t)&dl_ccm;
	NRF_CCM->INPTR = (uint32_t)in;
	NRF_CCM->OUTPTR = (uint32_t)dl_ccm_out;
	NRF_CCM->SCRATCHPTR = (uint32_t)dl_ccm_scratch;

	NRF_CCM->EVENTS_ENDKSGEN = 0;
	NRF_CCM->EVENTS_ENDCRYPT = 0;
	NRF_CCM->EVENTS_ERROR = 0;
	/* KSGEN then CRYPT. Stock chains this off RADIO via SHORTS/PPI; run here on a
	 * captured buffer, adequate for offline decode. */
	NRF_CCM->TASKS_KSGEN = 1;
	while (NRF_CCM->EVENTS_ENDCRYPT == 0 && NRF_CCM->EVENTS_ERROR == 0) {
		/* CCM completes in a few microseconds. */
	}

	return (NRF_CCM->MICSTATUS != 0u) ? 1 : 0;
}

/* ======================================================================== *
 * Section 3 - slot / counter reconstruction from A0 + 90 + 90
 * ======================================================================== */

static uint8_t dl_asm_state;   /* 0 idle, 1 have A0, 2 have first 90 */
static uint32_t dl_slot;       /* low 32 bits of the assembled counter */
static uint8_t dl_slot_mask;   /* DL_SYNC_* progress of the current ordered set */
static uint8_t dl_phase;       /* counter bit 0 */
static uint32_t dl_asm_prev;   /* previous complete counter low 32, for delta */
static bool dl_asm_have_prev;
static uint32_t dl_asm_span;   /* frames seen since this set's A0 */
static uint32_t dl_asm_delta;  /* this counter minus the previous complete one */

static void dl_slot_reset(void)
{
	dl_asm_state = 0u;
	dl_slot_mask = 0u;
	dl_asm_have_prev = false;
}

/* Ordered assembler. A0 starts a set (counter byte 0); the next 90 fills bytes
 * 1-2; the next 90 fills bytes 3-4 and completes it. Records of other classes
 * are counted into the span but do not reset the set; a fresh A0 restarts it.
 * Returns true only on the completing second 90, when dl_ctr holds a coherent
 * single-slot counter. A small, positive, roughly-constant delta between
 * consecutive completions indicates one live slot is being tracked. */
static bool dl_slot_apply(uint8_t opcode, const uint8_t *body)
{
	dl_asm_span++;
	switch (opcode & 0xF0u) {
	case DL_CLASS_A0:
		dl_ctr[0] = body[0];
		dl_asm_state = 1u;
		dl_slot_mask = DL_SYNC_A0;
		dl_asm_span = 0u;
		return false;
	case DL_CLASS_90:
		if (dl_asm_state == 1u) {
			dl_ctr[1] = body[0];
			dl_ctr[2] = body[1];
			dl_asm_state = 2u;
			dl_slot_mask |= DL_SYNC_90_LOW;
			return false;
		}
		if (dl_asm_state == 2u) {
			dl_ctr[3] = body[0];
			dl_ctr[4] = body[1];
			dl_asm_state = 0u;
			dl_slot_mask |= DL_SYNC_90_HIGH;
			dl_slot = (uint32_t)dl_ctr[0] |
				  ((uint32_t)dl_ctr[1] << 8) |
				  ((uint32_t)dl_ctr[2] << 16) |
				  ((uint32_t)dl_ctr[3] << 24);
			dl_phase = dl_ctr[0] & 1u;
			dl_asm_delta = dl_asm_have_prev ? (dl_slot - dl_asm_prev) : 0u;
			dl_asm_prev = dl_slot;
			dl_asm_have_prev = true;
			return true;
		}
		return false;
	default:
		return false;
	}
}

static bool dl_slot_ready(void)
{
	return (dl_slot_mask & DL_SYNC_ALL) == DL_SYNC_ALL;
}

/* ======================================================================== *
 * Section 4 - key-report queue (scanner thread -> link thread)
 * ======================================================================== */

#define DL_QUEUE_DEPTH 8u

static struct {
	uint8_t payload[DL_APP_PAYLOAD_BYTES]; /* 12 meaningful, padded to 16 */
	uint8_t used;
} dl_queue[DL_QUEUE_DEPTH];

static uint8_t dl_q_head;
static uint8_t dl_q_tail;
static uint8_t dl_last_bitmap[DL_REPORT_BITMAP_BYTES];
static bool dl_have_last;
static struct k_spinlock dl_q_lock;

/* Build the 12-byte transport record from an absolute bitmap and enqueue it.
 * Layout: [0]=endpoint/type 0, [1..9]=72-bit bitmap, [10]=mode 2, [11]=consumer.
 * Releases (all-zero bitmap) must be sent, so no empty-frame suppression. */
static void dl_queue_push(const uint8_t *bitmap)
{
	k_spinlock_key_t k = k_spin_lock(&dl_q_lock);
	uint8_t next = (uint8_t)((dl_q_head + 1u) % DL_QUEUE_DEPTH);

	if (next != dl_q_tail) { /* drop the oldest silently if full */
		uint8_t *p = dl_queue[dl_q_head].payload;

		memset(p, 0, DL_APP_PAYLOAD_BYTES);
		p[0] = 0u;                                  /* endpoint/type */
		memcpy(&p[1], bitmap, DL_REPORT_BITMAP_BYTES);
		p[10] = DL_REPORT_MODE_DONGLE;
		p[11] = 0u;                                 /* consumer bitmap TODO */
		dl_queue[dl_q_head].used = 1u;
		dl_q_head = next;
	}
	k_spin_unlock(&dl_q_lock, k);
}

void g4b_dongle_link_on_bitmap(const uint8_t *bitmap)
{
	/* Own dedup, independent of ZMK's kscan state (which may be disabled in
	 * dongle mode). Queue on every change, including the all-zero release. */
	if (dl_have_last &&
	    memcmp(dl_last_bitmap, bitmap, DL_REPORT_BITMAP_BYTES) == 0) {
		return;
	}
	memcpy(dl_last_bitmap, bitmap, DL_REPORT_BITMAP_BYTES);
	dl_have_last = true;
	dl_queue_push(bitmap);
}

static bool dl_queue_pop(uint8_t *payload_out)
{
	k_spinlock_key_t k = k_spin_lock(&dl_q_lock);
	bool got = false;

	if (dl_q_tail != dl_q_head) {
		memcpy(payload_out, dl_queue[dl_q_tail].payload,
		       DL_APP_PAYLOAD_BYTES);
		dl_q_tail = (uint8_t)((dl_q_tail + 1u) % DL_QUEUE_DEPTH);
		got = true;
	}
	k_spin_unlock(&dl_q_lock, k);
	return got;
}

/* ======================================================================== *
 * Section 5 - evidence emitters
 * ======================================================================== */

static void dl_app_hex(uint8_t *o, uint32_t *n, uint32_t v, int nibbles)
{
	static const char h[] = "0123456789abcdef";

	for (int i = nibbles - 1; i >= 0; i--) {
		o[(*n)++] = (uint8_t)h[(v >> (4 * i)) & 0xFu];
	}
}

static void dl_app_str(uint8_t *o, uint32_t *n, const char *s)
{
	while (*s) { o[(*n)++] = (uint8_t)*s++; }
}

/* "APXDL slot=<8> mask=<2> phase=<1> handoff=<1>\r\n" */
static void dl_emit_state(uint8_t handoff)
{
	uint8_t line[64];
	uint32_t n = 0u;

	dl_app_str(line, &n, "APXDL slot=");
	dl_app_hex(line, &n, dl_slot, 8);
	dl_app_str(line, &n, " mask=");
	dl_app_hex(line, &n, dl_slot_mask, 2);
	dl_app_str(line, &n, " phase=");
	dl_app_hex(line, &n, dl_phase, 1);
	dl_app_str(line, &n, " handoff=");
	dl_app_hex(line, &n, handoff, 1);
	line[n++] = '\r';
	line[n++] = '\n';
	g4b_evidence_emit_text(line, n);
}

/* "APXB0 mic=<1> type=<2> b=<hex...>\r\n" - one decrypted class-B0 body. */
static void dl_emit_b0(int mic, const uint8_t *body, uint32_t len)
{
	uint8_t line[160];
	uint32_t n = 0u;
	uint32_t nb = (len < 40u) ? len : 40u;

	dl_app_str(line, &n, "APXB0 mic=");
	dl_app_hex(line, &n, (uint32_t)mic, 1);
	dl_app_str(line, &n, " type=");
	dl_app_hex(line, &n, body[DL_APP_TYPE_OFFSET], 2);
	dl_app_str(line, &n, " b=");
	for (uint32_t i = 0; i < nb; i++) {
		dl_app_hex(line, &n, body[i], 2);
	}
	line[n++] = '\r';
	line[n++] = '\n';
	g4b_evidence_emit_text(line, n);
}

/* ======================================================================== *
 * Section 6 - radio leg helpers (reuse radio_esb_g4b primitives)
 * ======================================================================== */

/* Point the radio at one operational leg. base/prefix/statlen select RX (private
 * 0x76412900/0x71/38) or TX (0x89BED600/0x8E/19). MODE/CRC/whitening are already
 * set by g4b_esb_configure(); only address, length and CRCINIT change per leg. */
static void dl_radio_set_leg(uint32_t base0, uint32_t prefix0, uint8_t statlen)
{
	NRF_RADIO->PCNF0 = 0u;
	NRF_RADIO->PCNF1 = DL_PCNF1_BASE | ((uint32_t)statlen << 8);
	NRF_RADIO->BASE0 = base0;
	NRF_RADIO->PREFIX0 = prefix0;
	NRF_RADIO->TXADDRESS = 0u;
	NRF_RADIO->RXADDRESSES = 1u;
	NRF_RADIO->CRCCNF = DL_CRCCNF;
	NRF_RADIO->CRCPOLY = DL_CRCPOLY;
	NRF_RADIO->CRCINIT = DL_CRCINIT_LINK; /* default; dl_rx_leg cycles it */
	NRF_RADIO->DATAWHITEIV = DL_DATAWHITEIV;
}

/* ======================================================================== *
 * Section 7 - operational thread
 * ======================================================================== */

static K_SEM_DEFINE(dl_go, 0, 1);

static uint8_t dl_rxbuf[64] __aligned(4);
static uint8_t dl_txbuf[64] __aligned(4);
static uint8_t dl_ctrl_local_seq; /* bit3 application sequence */

/* Per-capture diagnostic counters. A frame is "heard" when g4b_esb_rx returns a
 * packet (any CRC); crcok counts CRC-24 matches under the operational CRCINIT. */
static uint32_t dl_rx_heard;
static uint32_t dl_rx_crcok;
static uint32_t dl_rx_crcbad;

/* "APXRX ch=NN crc=C op=XX b=<full frame hex>" - one received frame, verbatim
 * rather than a derived value. Dumps the whole static-length frame for offline
 * field analysis. */
static void dl_emit_rx(uint8_t ch, int crc, const uint8_t *frame, uint32_t len)
{
	uint8_t line[160];
	uint32_t n = 0u;
	uint32_t nb = len;

	dl_app_str(line, &n, "APXRX ch=");
	dl_app_hex(line, &n, ch, 2);
	dl_app_str(line, &n, " crc=");
	dl_app_hex(line, &n, (uint32_t)crc, 1);
	dl_app_str(line, &n, " seed=");
	dl_app_hex(line, &n, NRF_RADIO->CRCINIT, 6);
	dl_app_str(line, &n, " rxcrc=");
	dl_app_hex(line, &n, NRF_RADIO->RXCRC, 6);
	dl_app_str(line, &n, " op=");
	dl_app_hex(line, &n, frame[0], 2);
	dl_app_str(line, &n, " b=");
	for (uint32_t i = 0; i < nb; i++) {
		dl_app_hex(line, &n, frame[i], 2);
	}
	line[n++] = '\r';
	line[n++] = '\n';
	g4b_evidence_emit_text(line, n);
}

/* "APXSLOT ctr=<10hex> span=NN delta=<8hex>" - one complete ordered A0/90/90
 * set. span = frames between the A0 and completion; delta = counter minus the
 * previous complete counter. */
static void dl_emit_slot(void)
{
	uint8_t line[64];
	uint32_t n = 0u;

	dl_app_str(line, &n, "APXSLOT ctr=");
	for (int i = 4; i >= 0; i--) {
		dl_app_hex(line, &n, dl_ctr[i], 2);
	}
	dl_app_str(line, &n, " span=");
	dl_app_hex(line, &n, dl_asm_span, 4);
	dl_app_str(line, &n, " delta=");
	dl_app_hex(line, &n, dl_asm_delta, 8);
	line[n++] = '\r';
	line[n++] = '\n';
	g4b_evidence_emit_text(line, n);
}

/* Handle one received 38-byte operational frame: clear A0/90 feed the ordered
 * counter assembler; B0 is CCM and carries the type-2 handoff once the counter
 * is known. `crc` is 1 = CRC-OK, 0 = CRC-fail. Only CRC-OK frames advance. */
static void dl_handle_rx(const uint8_t *frame, int crc, uint8_t ch)
{
	uint8_t opcode = frame[0];
	uint8_t cls = opcode & 0xF0u;

	dl_rx_heard++;
	if (crc == 1) { dl_rx_crcok++; } else { dl_rx_crcbad++; }

	if (crc != 1) {
		return; /* do not reconstruct the counter from noise */
	}

	/* Dump the counter/handoff records (a0/90/b0) verbatim and in full; skip the
	 * high-rate d0/40/50/60 so the loop stays fast enough to catch the one-time
	 * bootstrap sequence at the start of pairing. */
	if (cls == DL_CLASS_A0 || cls == DL_CLASS_90 || cls == DL_CLASS_B0) {
		dl_emit_rx(ch, crc, frame, DL_RX_STATLEN);
	}

	switch (cls) {
	case DL_CLASS_A0:
	case DL_CLASS_90:
		if (dl_slot_apply(opcode, &frame[1])) {
			dl_emit_slot();
		}
		break;
	case DL_CLASS_B0:
		if (dl_slot_ready()) {
			int mic = dl_ccm_decrypt(frame);
			const uint8_t *body = &dl_ccm_out[3]; /* skip hdr,len,rfu */
			uint8_t handoff =
				(body[DL_APP_TYPE_OFFSET] == DL_APP_TYPE_NORMAL)
					? 1u : 0u;

			dl_emit_b0(mic, body, DL_RX_STATLEN);
			dl_emit_state(handoff);
		}
		break;
	default:
		break;
	}
}

/* "APXDLSUM legs=.. heard=.. crcok=.. crcbad=.." - periodic listen summary. A
 * zero crcok proves no clean operational frame arrived (wrong mode/address or no
 * bond); a non-zero crcok means the dumped APXRX bytes are real. */
static void dl_emit_sum(uint32_t legs)
{
	uint8_t line[80];
	uint32_t n = 0u;

	dl_app_str(line, &n, "APXDLSUM legs=");
	dl_app_hex(line, &n, legs, 8);
	dl_app_str(line, &n, " heard=");
	dl_app_hex(line, &n, dl_rx_heard, 8);
	dl_app_str(line, &n, " crcok=");
	dl_app_hex(line, &n, dl_rx_crcok, 8);
	dl_app_str(line, &n, " crcbad=");
	dl_app_hex(line, &n, dl_rx_crcbad, 8);
	line[n++] = '\r';
	line[n++] = '\n';
	g4b_evidence_emit_text(line, n);
}

static uint32_t dl_us_since(uint32_t start_cyc)
{
	return k_cyc_to_us_floor32(k_cycle_get_32() - start_cyc);
}

static void dl_radio_off(void)
{
	uint32_t start = k_cycle_get_32();

	NRF_RADIO->SHORTS = 0;
	NRF_RADIO->EVENTS_DISABLED = 0;
	NRF_RADIO->TASKS_DISABLE = 1;
	while (NRF_RADIO->EVENTS_DISABLED == 0) {
		if (dl_us_since(start) > 1000u) {
			break;
		}
	}
}

/* Continuously receive on one channel for window_us, re-arming immediately after
 * each packet (READY_START | END_DISABLE, then TASKS_RXEN) so consecutive frames
 * from the same slot are captured in order - the key to a coherent A0/90/90 set.
 * The seed is fixed at the confirmed learned-link 0x087fc1. */
static void dl_dwell(uint8_t ch, uint32_t window_us)
{
	uint32_t start;

	dl_radio_set_leg(DL_RX_BASE0, DL_RX_PREFIX0, DL_RX_STATLEN);
	NRF_RADIO->CRCINIT = DL_CRCINIT_LINK;
	NRF_RADIO->FREQUENCY = ch;
	NRF_RADIO->PACKETPTR = (uint32_t)dl_rxbuf;
	NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk |
			    RADIO_SHORTS_END_DISABLE_Msk;
	NRF_RADIO->EVENTS_END = 0;
	NRF_RADIO->EVENTS_DISABLED = 0;
	NRF_RADIO->TASKS_RXEN = 1;

	start = k_cycle_get_32();
	while (dl_us_since(start) <= window_us) {
		if (NRF_RADIO->EVENTS_END != 0) {
			int crc = (NRF_RADIO->CRCSTATUS != 0) ? 1 : 0;

			NRF_RADIO->EVENTS_END = 0;
			dl_handle_rx(dl_rxbuf, crc, ch);
			while (NRF_RADIO->EVENTS_DISABLED == 0) {
				/* END_DISABLE completes in a few radio clocks. */
			}
			NRF_RADIO->EVENTS_DISABLED = 0;
			NRF_RADIO->TASKS_RXEN = 1; /* re-arm for the next frame */
		}
	}
	dl_radio_off();
}

/* One transmit leg: drain a queued report if the peer acked the last, else send an
 * idle keepalive to hold the link up. Unused in the passive listen build; kept for
 * the active state machine. */
static __maybe_unused void dl_tx_leg(uint8_t ch)
{
	uint8_t payload[DL_APP_PAYLOAD_BYTES];
	uint8_t ctrl;

	dl_radio_set_leg(DL_TX_BASE0, DL_TX_PREFIX0, DL_TX_STATLEN);
	memset(dl_txbuf, 0, DL_TX_STATLEN);

	if (dl_queue_pop(payload)) {
		ctrl = (uint8_t)(DL_CTRL_DATA |
				 (dl_ctrl_local_seq ? DL_CTRL_LOCAL_SEQ : 0u));
		dl_txbuf[0] = ctrl;
		/* TODO(hw): CCM-encrypt `payload` (direction 1, current counter)
		 * into the 19-byte frame, MIC omitted on air. Placeholder copies
		 * the clear 16-byte payload into [1..16]; [17..18] stay zero. */
		memcpy(&dl_txbuf[1], payload, DL_APP_PAYLOAD_BYTES);
		dl_ctrl_local_seq ^= 1u; /* advance app sequence per notes */
	} else {
		dl_txbuf[0] = DL_CTRL_IDLE;
	}

	(void)g4b_esb_tx(ch, dl_txbuf);
}

static void dl_thread(void *a, void *b, void *c)
{
	uint32_t leg = 0u;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	k_sem_take(&dl_go, K_FOREVER);
	g4b_evidence_emit_text((const uint8_t *)"APXDL start\r\n", 13u);

	dl_slot_reset();

	/* Passive-first (resume step 1): continuously listen and reconstruct the
	 * counter from the receiver's A0/90/90 records, in arrival order. dl_channels
	 * = {76, 2}; the records arrive on ch 2 for this receiver, so dwell there and
	 * sample ch 76 briefly. Emit a summary once per outer pass. */
	for (;;) {
		dl_dwell(dl_channels[1], 200000u);
		dl_dwell(dl_channels[0], 20000u);

		leg++;
		dl_emit_sum(leg);
	}
}

/* Priority 12, same band as the pairing probe: below the USB updater (prio 10)
 * so a reflash still preempts and completes. */
K_THREAD_DEFINE(g4b_dongle_link_tid, 2048, dl_thread, NULL, NULL, NULL,
		K_PRIO_PREEMPT(12), 0, 0);

void g4b_dongle_link_start(void)
{
	k_sem_give(&dl_go);
}
