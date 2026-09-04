/* SPDX-License-Identifier: MIT
 *
 * Experimental direct-register radio probe for the SteelSeries 2.4 GHz dongle.
 * It runs only after radio_g4b.c has disabled Bluetooth and released NRF_RADIO.
 *
 * Static analysis recovered two related configurations. Pairing starts on
 * address 0x8E89BED6 with little-endian packets and whitening disabled, then
 * moves to private address 0x76412900/0x71. The established link uses the
 * private address with big-endian packets and whitening enabled. Both use the
 * 2 Mbit PHY, a three-byte CRC (0x65B, init 0x00FFFFFF), and a static packet
 * length.
 *
 * This file implements the incomplete pairing experiment, not a working dongle
 * transport. It sweeps unresolved lengths and channels while looking for the
 * class-0x80 reply. Release builds leave it disabled.
 */

#include <string.h>

#include <zephyr/kernel.h>

#include <nrfx.h>

#include "evidence_g4b.h"
#include "radio_esb_g4b.h"

/* --- confirmed operational register values ------------------------------- */
#define G4B_ESB_MODE_BLE_2MBIT 4u
/* PAIRING address (from DONGLE firmware RE 272111140 @0x1ac0a): the unpaired
 * dongle listens on BASE0=0x89BED600 / PREFIX0=0x8E, not the operational ESB
 * address 0x76412900/0x71. The dongle RX also runs WHITEEN=0, ENDIAN=little
 * (see esb_set_pcnf1). */
#define G4B_ESB_BASE0          0x89BED600u /* pairing address (op link = 0x76412900) */
#define G4B_ESB_PREFIX0        0x0000008Eu /* AP0 = 0x8E (op link = 0x71)            */
#define G4B_ESB_TXADDRESS      0u
#define G4B_ESB_RXADDRESSES    1u          /* enable logical address 0 only    */
#define G4B_ESB_CRCCNF         0x00000103u /* LEN=3 bytes, SKIPADDR=1          */
#define G4B_ESB_CRCPOLY        0x0000065Bu
#define G4B_ESB_CRCINIT        0x00FFFFFFu /* config bound to 0x8E89BED6 + private */
#define G4B_ESB_TXPOWER_P4DBM  4u          /* +4 dBm (register takes int8 dBm) */

/* Provisional static length: 32-byte NKRO bitmap + a 4-byte link header
 * ([seq][mirror][PID][flags]). The exact header count is a residual - this is a
 * starting value to be corrected once we see the receiver respond. */
#define G4B_ESB_STATLEN_NKRO   36u

bool g4b_esb_hfclk_start(void)
{
	/* HFXO. The 2 Mbit PHY will not lock to the internal RC. */
	if ((NRF_CLOCK->HFCLKSTAT &
	     (CLOCK_HFCLKSTAT_STATE_Msk | CLOCK_HFCLKSTAT_SRC_Msk)) ==
	    ((CLOCK_HFCLKSTAT_STATE_Running << CLOCK_HFCLKSTAT_STATE_Pos) |
	     (CLOCK_HFCLKSTAT_SRC_Xtal << CLOCK_HFCLKSTAT_SRC_Pos))) {
		return true; /* already running on the crystal */
	}

	NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
	NRF_CLOCK->TASKS_HFCLKSTART = 1;

	for (uint32_t i = 0; i < 100000u; i++) {
		if (NRF_CLOCK->EVENTS_HFCLKSTARTED != 0) {
			return true;
		}
	}
	return false;
}

void g4b_esb_set_channel(uint8_t ch)
{
	NRF_RADIO->FREQUENCY = ch; /* 2400 + ch MHz; MAP=Default */
}

void g4b_esb_configure(uint8_t statlen)
{
	/* Power-cycle the peripheral so no Bluetooth-controller state remains.
	 * DATAWHITEIV returns to 0x40, but this pairing setup leaves whitening off. */
	NRF_RADIO->POWER = 0;
	NRF_RADIO->POWER = 1;

	NRF_RADIO->MODE = G4B_ESB_MODE_BLE_2MBIT;
	NRF_RADIO->TXPOWER = G4B_ESB_TXPOWER_P4DBM;

	/* Fast ramp-up (RU=1), DTX=Center: shrinks TX->RX turnaround from ~130us
	 * to ~40us so a quick pairing reply is not missed. Frame content on air is
	 * unchanged, so the dongle sees the same request either way. */
	NRF_RADIO->MODECNF0 = 0x00000201u;

	/* Static-length framing (the config family bound to the 0x8E89BED6 pairing
	 * address AND the private link, per the dongle RE): PCNF0=0 (no on-air S0/
	 * LENGTH), fixed STATLEN in PCNF1. BALEN=3, ENDIAN=little, WHITEEN=0. */
	NRF_RADIO->PCNF0 = 0u;
	NRF_RADIO->PCNF1 = 0x000300FEu | ((uint32_t)statlen << 8);

	NRF_RADIO->BASE0 = G4B_ESB_BASE0;
	NRF_RADIO->PREFIX0 = G4B_ESB_PREFIX0;
	NRF_RADIO->TXADDRESS = G4B_ESB_TXADDRESS;
	NRF_RADIO->RXADDRESSES = G4B_ESB_RXADDRESSES;

	NRF_RADIO->CRCCNF = G4B_ESB_CRCCNF;
	NRF_RADIO->CRCPOLY = G4B_ESB_CRCPOLY;
	NRF_RADIO->CRCINIT = G4B_ESB_CRCINIT;
}

/* --- config read-back evidence ------------------------------------------- */

static uint32_t esb_hex32(uint8_t *out, uint32_t v)
{
	static const char h[] = "0123456789abcdef";

	for (int i = 7; i >= 0; i--) {
		out[i] = (uint8_t)h[v & 0xFu];
		v >>= 4;
	}
	return 8u;
}

static void esb_emit_config(bool hf)
{
	uint8_t line[96];
	uint32_t n = 0u;
	const char *tag = "APXESB cfg hf=";

	for (const char *p = tag; *p; p++) {
		line[n++] = (uint8_t)*p;
	}
	line[n++] = hf ? '1' : '0';

	const char *m = " mode=";
	for (const char *p = m; *p; p++) { line[n++] = (uint8_t)*p; }
	n += esb_hex32(&line[n], NRF_RADIO->MODE);

	const char *b = " base0=";
	for (const char *p = b; *p; p++) { line[n++] = (uint8_t)*p; }
	n += esb_hex32(&line[n], NRF_RADIO->BASE0);

	const char *pf = " prefix0=";
	for (const char *p = pf; *p; p++) { line[n++] = (uint8_t)*p; }
	n += esb_hex32(&line[n], NRF_RADIO->PREFIX0);

	const char *cp = " poly=";
	for (const char *p = cp; *p; p++) { line[n++] = (uint8_t)*p; }
	n += esb_hex32(&line[n], NRF_RADIO->CRCPOLY);

	const char *p1 = " pcnf1=";
	for (const char *p = p1; *p; p++) { line[n++] = (uint8_t)*p; }
	n += esb_hex32(&line[n], NRF_RADIO->PCNF1);

	line[n++] = '\r';
	line[n++] = '\n';
	g4b_evidence_emit_text(line, n);
}

/* --- TX/RX primitives ---------------------------------------------------- */

#define G4B_ESB_BUFSZ 260u /* EasyDMA buffer (RAM); >= MAXLEN(255)+2 so a long BLE
			    * PDU LENGTH field can never overflow the RX buffer */

static uint8_t esb_rxbuf[G4B_ESB_BUFSZ];

static uint32_t esb_us_since(uint32_t start_cyc)
{
	return k_cyc_to_us_floor32(k_cycle_get_32() - start_cyc);
}

static void esb_radio_disable(void)
{
	uint32_t start = k_cycle_get_32();

	/* Clear SHORTS first: otherwise a still-set DISABLED_RXEN turns the
	 * DISABLED we are about to cause straight back into an RX ramp, so the
	 * radio never actually stays disabled and the next TXEN wedges. */
	NRF_RADIO->SHORTS = 0;
	NRF_RADIO->EVENTS_DISABLED = 0;
	NRF_RADIO->TASKS_DISABLE = 1;
	while (NRF_RADIO->EVENTS_DISABLED == 0) {
		if (esb_us_since(start) > 1000u) {
			break; /* reprogrammed on the next call regardless */
		}
	}
}

bool g4b_esb_tx(uint8_t ch, const uint8_t *buf)
{
	uint32_t start;

	NRF_RADIO->FREQUENCY = ch;
	NRF_RADIO->PACKETPTR = (uint32_t)buf;
	NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk |
			    RADIO_SHORTS_END_DISABLE_Msk;
	NRF_RADIO->EVENTS_END = 0;
	NRF_RADIO->EVENTS_DISABLED = 0;
	NRF_RADIO->TASKS_TXEN = 1;

	start = k_cycle_get_32();
	while (NRF_RADIO->EVENTS_DISABLED == 0) {
		if (esb_us_since(start) > 2000u) {
			esb_radio_disable();
			return false;
		}
	}
	return true;
}

int g4b_esb_rx(uint8_t ch, uint8_t *buf, uint32_t timeout_us)
{
	uint32_t start;
	int crc;

	NRF_RADIO->FREQUENCY = ch;
	NRF_RADIO->PACKETPTR = (uint32_t)buf;
	NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk; /* one packet, then idle */
	NRF_RADIO->EVENTS_END = 0;
	NRF_RADIO->EVENTS_CRCOK = 0;
	NRF_RADIO->EVENTS_CRCERROR = 0;
	NRF_RADIO->TASKS_RXEN = 1;

	start = k_cycle_get_32();
	while (NRF_RADIO->EVENTS_END == 0) {
		if (esb_us_since(start) > timeout_us) {
			esb_radio_disable();
			return -1;
		}
	}
	crc = (NRF_RADIO->CRCSTATUS != 0) ? 1 : 0;
	esb_radio_disable();
	return crc;
}

/* --- small text builders (shared by the evidence emitters) --------------- */

static void esb_app_str(uint8_t *o, uint32_t *n, const char *s)
{
	while (*s) { o[(*n)++] = (uint8_t)*s++; }
}

static void esb_app_hex8(uint8_t *o, uint32_t *n, uint8_t b)
{
	static const char h[] = "0123456789abcdef";

	o[(*n)++] = (uint8_t)h[b >> 4];
	o[(*n)++] = (uint8_t)h[b & 0xFu];
}

static void esb_app_u32(uint8_t *o, uint32_t *n, uint32_t v)
{
	char tmp[10];
	int i = 0;

	if (v == 0u) { o[(*n)++] = '0'; return; }
	while (v) { tmp[i++] = (char)('0' + (v % 10u)); v /= 10u; }
	while (i) { o[(*n)++] = (uint8_t)tmp[--i]; }
}

/* Rebuild PCNF1 with a trial static length / whitening bit (BALEN=3, ENDIAN
 * little). Retained as a knob for the ESB (static-length) framing; the BLE-adv
 * pairing path sets PCNF0/PCNF1 directly in g4b_esb_configure(). */
static __maybe_unused void esb_set_pcnf1(uint8_t statlen, bool whiten)
{
	uint32_t p = 0x000300FEu | ((uint32_t)statlen << 8); /* BALEN=3, ENDIAN little */

	if (whiten) {
		p |= (1u << 25); /* WHITEEN (dongle wants it OFF; kept as a knob) */
	}
	NRF_RADIO->PCNF1 = p;
}

#define G4B_ESB_DUMPLEN 60u /* max raw bytes reported per frame (fits esb_rxbuf) */

/* --- active pairing TX + reply listen ------------------------------------ *
 *
 * Passive captures found vendor RF on channels 2,3,4,24,25,26, but did not
 * validate the remaining STATLEN, whitening, or CRC-framing assumptions. The
 * stock keyboard initiates pairing; the dongle listens and sends heartbeats.
 * This code transmits the class-0x40 pairing request built by FUN_00041710. It
 * varies the static payload length and channel, then opens a receive window for
 * the dongle's class-0x80 reply. The host also watches the dongle's USB status,
 * so finding a response does not depend on a correct receive configuration.
 * The first response identifies the frame length and channel; its negotiated
 * configuration can then be decoded.
 */

/* Sweep channels 0..39 and let the repeat detector find the reply channel. */
#define G4B_ESB_NCH 81u	/* full 2.4 GHz band: FREQUENCY 0..80 = 2400..2480 MHz */

/* STATLEN candidates for the pairing REQUEST. RE of the stock builder
 * (FUN_00041528/FUN_0003ed78) puts the request at header(4)+opcode(1)+id(2)
 * [+ device-type body(3)] and STATLEN = uxth(ctx[0x141c]+ctx[0x141a]), a runtime
 * block-load not resolvable statically - so it lands ~7-12. PCNF0=0 (no on-air
 * length field) means TX and the dongle must agree on one static length; sweep. */
/* The pairing address IS the BLE advertising access address, so the dongle
 * listens on the BLE advertising channels 37/38/39 = nRF FREQUENCY 2/26/80.
 * Focusing on these 3 lets us sweep the full STATLEN range (4..40) many times per
 * pairing window (STATLEN = header4 + two runtime body-size fields, not statically
 * knowable). */
static const uint8_t esb_adv_ch[] = { 2u, 26u, 80u };
#define G4B_ESB_SL_MIN 4u
#define G4B_ESB_SL_MAX 40u

/* 2-byte pairing id (stock: ctx[0xe24], runtime). The dongle assigns the pipe
 * index in its 0x80 reply, so 0x0000 for a first request. */
#define G4B_ESB_PAIR_ID0 0x00u
#define G4B_ESB_PAIR_ID1 0x00u
/* Device-type descriptor {02 01 01} (stock const @flash 0x455d4), the body the
 * enumeration path places after the id. */
#define G4B_ESB_DEVTYPE0 0x02u
#define G4B_ESB_DEVTYPE1 0x01u
#define G4B_ESB_DEVTYPE2 0x01u
#define G4B_ESB_CLASS_40 0x40u /* primary pairing-request opcode class */

static uint8_t esb_txbuf[G4B_ESB_BUFSZ];

/* Repeat detector: a real dongle reply is deterministic and recurs on the same
 * channel; ambient noise is unique. Keep the last raw frame per channel and, when
 * a new reception byte-matches it, emit it as a confirmed real frame. */
#define G4B_ESB_RAW 32u
static uint8_t esb_last[G4B_ESB_NCH][G4B_ESB_RAW];
static uint8_t esb_have[G4B_ESB_NCH];

/* Build one class-0x40 pairing request into esb_txbuf, zero-padded to the
 * current static length. Best-effort layout from the RE (to be corrected
 * empirically once the dongle responds):
 *   [0]=seq  [1]=mirror(=[2])  [2]=PID(2-bit)  [3]=flags(0)
 *   [4]=class 0x40  [5]=id0  [6]=id1  [7..]=0
 */
static uint8_t esb_adv_variant;

/* Build a BLE-ADV pairing beacon into esb_txbuf (PCNF0 = S0LEN=1 + LFLEN=8):
 *   [0]  S0     = 0x02  (ADV_NONCONN_IND, TxAdd/RxAdd = 0)
 *   [1]  LENGTH = payload byte count (AdvA + AdvData)
 *   [2..7] AdvA (6-byte advertiser address, LSB first; MSB 0xC0 = random static)
 *   [8..]  AdvData: SteelSeries device-type {02 01 01} [+ 2-byte id, per variant]
 * The nRF sends [0], [1], then LENGTH bytes. PHY (address/CRC/whitening) is the
 * BLE-adv pairing config from g4b_esb_configure(). */
static void esb_build_adv(void)
{
	uint32_t n;

	memset(esb_txbuf, 0, sizeof(esb_txbuf));
	esb_txbuf[0] = 0x02u;                 /* ADV_NONCONN_IND */
	esb_txbuf[2] = 0x91u;                 /* AdvA (LSB first) */
	esb_txbuf[3] = 0x52u;
	esb_txbuf[4] = 0xA3u;
	esb_txbuf[5] = 0xB4u;
	esb_txbuf[6] = 0xC5u;
	esb_txbuf[7] = 0xC0u;                 /* MSB: random-static top bits */
	n = 8u;
	esb_txbuf[n++] = G4B_ESB_DEVTYPE0;    /* device-type descriptor {02 01 01} */
	esb_txbuf[n++] = G4B_ESB_DEVTYPE1;
	esb_txbuf[n++] = G4B_ESB_DEVTYPE2;
	if (esb_adv_variant >= 1u) {
		esb_txbuf[n++] = G4B_ESB_PAIR_ID0; /* + 2-byte pairing id */
		esb_txbuf[n++] = G4B_ESB_PAIR_ID1;
	}
	esb_txbuf[1] = (uint8_t)(n - 2u);     /* LENGTH = AdvA + AdvData */
}

/* "APXPAIRRX ch=NN len=LL crc=C b=<hex>" - a frame heard in the reply window. */
static void esb_dump_reply(uint8_t ch, uint8_t len, int crc, const uint8_t *buf)
{
	uint8_t line[200];
	uint32_t n = 0u;
	uint8_t nb = (len < G4B_ESB_DUMPLEN) ? len : G4B_ESB_DUMPLEN;

	esb_app_str(line, &n, "APXPAIRRX ch=");
	esb_app_u32(line, &n, ch);
	esb_app_str(line, &n, " len=");
	esb_app_u32(line, &n, len);
	esb_app_str(line, &n, " crc=");
	esb_app_u32(line, &n, (uint32_t)crc);
	esb_app_str(line, &n, " b=");
	for (uint8_t i = 0; i < nb; i++) {
		esb_app_hex8(line, &n, buf[i]);
	}
	line[n++] = '\r';
	line[n++] = '\n';
	g4b_evidence_emit_text(line, n);
}

static K_SEM_DEFINE(esb_go, 0, 1);

/* "APXLST len=LL ch=NN end=E crcok=C tx=T" - per (length, channel) listen
 * summary. end = receptions that completed (any CRC); crcok = frames that
 * passed CRC-24. A non-zero crcok proves the whole PHY (address + whitening IV
 * + CRC) is right at that static length, and the dumped APXPAIRRX bytes are a
 * real vendor frame. */
static void esb_emit_lst(uint8_t len, uint8_t ch, uint32_t end, uint32_t crcok,
			 uint32_t tx)
{
	uint8_t line[96];
	uint32_t n = 0u;

	esb_app_str(line, &n, "APXLST len=");
	esb_app_u32(line, &n, len);
	esb_app_str(line, &n, " ch=");
	esb_app_u32(line, &n, ch);
	esb_app_str(line, &n, " end=");
	esb_app_u32(line, &n, end);
	esb_app_str(line, &n, " crcok=");
	esb_app_u32(line, &n, crcok);
	esb_app_str(line, &n, " tx=");
	esb_app_u32(line, &n, tx);
	line[n++] = '\r';
	line[n++] = '\n';
	g4b_evidence_emit_text(line, n);
}

/* "APXTX sl=NN" - marks which request STATLEN the sweep is currently sending, so
 * a PC-side dongle-status change or a GG pairing event can be correlated to it. */
static void esb_emit_tx(uint8_t sl)
{
	uint8_t line[24];
	uint32_t n = 0u;

	esb_app_str(line, &n, "APXTX sl=");
	esb_app_u32(line, &n, sl);
	line[n++] = '\r';
	line[n++] = '\n';
	g4b_evidence_emit_text(line, n);
}

/* Sanity self-test independent of the dongle: sniff AMBIENT BLE advertising on
 * our exact pairing access address (0x89BED600/0x8E) in BLE-1Mbit mode on the
 * advertising channels. Ambient adv is everywhere, so a nonzero count proves our
 * address encoding + RX path actually work; a zero count means the bug is our
 * radio config, not the dongle. CRC is disabled so any address match completes. */
static void esb_ble_adv_selftest(void)
{
	static const uint8_t adv_ch[] = { 2u, 26u, 80u };
	uint32_t rx = 0u;
	uint8_t line[28];
	uint32_t n = 0u;

	NRF_RADIO->POWER = 0;
	NRF_RADIO->POWER = 1;
	NRF_RADIO->MODE = 3u;                          /* Ble_1Mbit (advertising) */
	NRF_RADIO->MODECNF0 = 0x00000201u;
	NRF_RADIO->PCNF0 = 0u;                          /* static length */
	NRF_RADIO->PCNF1 = 0x000300FEu | (37u << 8);    /* BALEN=3, little, STATLEN=37 */
	NRF_RADIO->BASE0 = 0x89BED600u;
	NRF_RADIO->PREFIX0 = 0x8Eu;
	NRF_RADIO->TXADDRESS = 0u;
	NRF_RADIO->RXADDRESSES = 1u;
	NRF_RADIO->CRCCNF = 0u;                          /* CRC off: count address matches */
	NRF_RADIO->PACKETPTR = (uint32_t)esb_rxbuf;

	for (uint16_t pass = 0; pass < 150u; pass++) {
		for (uint8_t i = 0; i < sizeof(adv_ch); i++) {
			uint32_t t0;

			NRF_RADIO->FREQUENCY = adv_ch[i];
			NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk;
			NRF_RADIO->EVENTS_END = 0;
			NRF_RADIO->TASKS_RXEN = 1;
			t0 = k_cycle_get_32();
			while (NRF_RADIO->EVENTS_END == 0) {
				if (esb_us_since(t0) > 1500u) {
					break;
				}
			}
			if (NRF_RADIO->EVENTS_END) {
				rx++;
			}
			esb_radio_disable();
		}
	}

	esb_app_str(line, &n, "APXADV rx=");
	esb_app_u32(line, &n, rx);
	line[n++] = '\r';
	line[n++] = '\n';
	g4b_evidence_emit_text(line, n);
}

/* Class-0x40 pairing request for the PRIVATE-address handshake (Phase B):
 * [4]=opcode 0x40, [5..6]=id, [7..9]=device-type. Sent static-length. */
static void esb_build_class40(void)
{
	memset(esb_txbuf, 0, sizeof(esb_txbuf));
	esb_txbuf[4] = G4B_ESB_CLASS_40;
	esb_txbuf[5] = G4B_ESB_PAIR_ID0;
	esb_txbuf[6] = G4B_ESB_PAIR_ID1;
	esb_txbuf[7] = G4B_ESB_DEVTYPE0;
	esb_txbuf[8] = G4B_ESB_DEVTYPE1;
	esb_txbuf[9] = G4B_ESB_DEVTYPE2;
}

static void esb_pair_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	/* Block until the radio is freed + configured (dongle-mode standdown). */
	k_sem_take(&esb_go, K_FOREVER);

	g4b_evidence_emit_text((const uint8_t *)"APXPAIR start\r\n", 15u);

	/* Prove the address+RX path works (dongle-independent), then restore the
	 * pairing PHY (MODE 2Mbit / pairing address / CRC) for the TX sweep. */
	esb_ble_adv_selftest();
	g4b_esb_configure(G4B_ESB_STATLEN_NKRO);

	/* Two-step pairing test (common PHY: 2Mbit, static length, CRC 0x103/0x65B/
	 * 0x00FFFFFF, no whitening - set by g4b_esb_configure; only BASE0/PREFIX0 +
	 * STATLEN + channel change per phase):
	 *   Phase A - beacon on the adv access address 0x8E89BED6 (ch 37/38/39) to
	 *             register us with the dongle (it does NO content check).
	 *   Phase B - on the private address 0x76412900/0x71, transmit the class-0x40
	 *             request and listen for the dongle's class-0x80 reply.
	 * A CRC-ok reception in Phase B (crcok>0) is the success signal. */
	static const uint8_t beacon_len[] = { 16u, 24u, 31u, 12u };
	static const uint8_t priv_len[] = { 10u, 8u, 12u };
	static const uint8_t priv_ch[] = { 2u, 3u, 4u, 24u, 25u, 26u,
					   40u, 60u, 80u };

	for (;;) {
		uint32_t txok = 0u, end = 0u, crcok = 0u;

		esb_emit_tx(0u); /* heartbeat marker */

		/* Phase A: beacon on the adv access address. */
		esb_adv_variant = 1u;
		for (uint8_t bi = 0; bi < sizeof(beacon_len); bi++) {
			NRF_RADIO->BASE0 = 0x89BED600u;
			NRF_RADIO->PREFIX0 = 0x8Eu;
			NRF_RADIO->PCNF1 = 0x000300FEu |
					   ((uint32_t)beacon_len[bi] << 8);
			for (uint8_t rep = 0; rep < 20u; rep++) {
				for (uint8_t ci = 0u;
				     ci < sizeof(esb_adv_ch); ci++) {
					esb_build_adv();
					if (g4b_esb_tx(esb_adv_ch[ci],
						       esb_txbuf)) {
						txok++;
					}
				}
			}
			k_msleep(1);
		}

		/* Phase B: class-0x40 + reply-listen on the private address. */
		NRF_RADIO->BASE0 = 0x76412900u;
		NRF_RADIO->PREFIX0 = 0x71u;
		for (uint8_t li = 0; li < sizeof(priv_len); li++) {
			NRF_RADIO->PCNF1 = 0x000300FEu |
					   ((uint32_t)priv_len[li] << 8);
			for (uint8_t cyc = 0; cyc < 6u; cyc++) {
				for (uint8_t ci = 0u;
				     ci < sizeof(priv_ch); ci++) {
					uint8_t ch = priv_ch[ci];
					int r;

					esb_build_class40();
					if (g4b_esb_tx(ch, esb_txbuf)) {
						txok++;
					}
					r = g4b_esb_rx(ch, esb_rxbuf, 1200u);
					if (r >= 0) {
						end++;
						if (r == 1 ||
						    (esb_have[ch] &&
						     memcmp(esb_last[ch],
							    esb_rxbuf,
							    G4B_ESB_RAW) == 0)) {
							crcok++;
							esb_dump_reply(
								ch, G4B_ESB_RAW,
								r, esb_rxbuf);
						} else {
							memcpy(esb_last[ch],
							       esb_rxbuf,
							       G4B_ESB_RAW);
							esb_have[ch] = 1u;
						}
					}
				}
				k_msleep(1);
			}
		}

		/* end = private-addr receptions, crcok = CRC-ok/repeat (a real
		 * class-0x80 reply = success), txok = beacons+requests sent. */
		esb_emit_lst(0u, 0u, end, crcok, txok);
	}
}

/* Priority 12 - BELOW the USB updater thread (prio 10) so a reflash preempts the
 * pairing sweep and completes, and below the keyboard/USB threads too. */
K_THREAD_DEFINE(g4b_esb_probe_tid, 2048, esb_pair_thread, NULL, NULL, NULL,
		K_PRIO_PREEMPT(12), 0, 0);

void g4b_esb_on_radio_free(void)
{
	bool hf = g4b_esb_hfclk_start();

	g4b_esb_configure(G4B_ESB_STATLEN_NKRO);
	g4b_esb_set_channel(0u);

	/* Read the registers back and report, so we can confirm on hardware that
	 * the standdown really freed NRF_RADIO and our config sticks. */
	esb_emit_config(hf);

	/* Release the pairing-TX thread now the radio is ours and configured. */
	k_sem_give(&esb_go);
}
