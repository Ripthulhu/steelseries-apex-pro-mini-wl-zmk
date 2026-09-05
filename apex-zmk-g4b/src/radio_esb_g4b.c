/* SPDX-License-Identifier: MIT
 *
 * Experimental direct-register radio probe for the SteelSeries 2.4 GHz dongle.
 * It runs only after radio_g4b.c has disabled Bluetooth and released NRF_RADIO.
 *
 * Static analysis recovered two related configurations. Pairing starts on
 * address 0x8E89BED6 with big-endian packets and whitening enabled, then
 * moves to private address 0x76412900/0x71. The established link uses the
 * private address with big-endian packets and whitening enabled. Both use the
 * 2 Mbit PHY, a three-byte CRC (0x65B, init 0x00FFFFFF), and a static packet
 * length.
 *
 * This file implements the incomplete pairing experiment, not a working dongle
 * transport. Release builds leave it disabled.
 */

#include <string.h>

#include <zephyr/kernel.h>

#include <nrfx.h>

#include "evidence_g4b.h"
#include "radio_esb_g4b.h"
#if IS_ENABLED(CONFIG_APEX_G4B_DONGLE_LINK)
#include "dongle_link_g4b.h"
#endif

/* --- confirmed operational register values ------------------------------- */
#define G4B_ESB_MODE_BLE_2MBIT 4u
/* Pairing address from dongle firmware 272111140 at 0x1ac0a. The pairing
 * setup changes BASE0/PREFIX0 but retains the normal link packet format. */
#define G4B_ESB_BASE0          0x89BED600u /* pairing address (op link = 0x76412900) */
#define G4B_ESB_PREFIX0        0x0000008Eu /* AP0 = 0x8E (op link = 0x71)            */
#define G4B_ESB_TXADDRESS      0u
#define G4B_ESB_RXADDRESSES    1u          /* enable logical address 0 only    */
#define G4B_ESB_CRCCNF         0x00000103u /* LEN=3 bytes, SKIPADDR=1          */
#define G4B_ESB_CRCPOLY        0x0000065Bu
#define G4B_ESB_CRCINIT        0x00FFFFFFu /* config bound to 0x8E89BED6 + private */
#define G4B_ESB_DATAWHITEIV    0x40u
#define G4B_ESB_TXPOWER_P4DBM  4u          /* +4 dBm (register takes int8 dBm) */

/* Provisional static length: 32-byte NKRO bitmap + a 4-byte link header
 * ([seq][mirror][PID][flags]). The exact header count is a residual - a starting
 * value to be corrected once the receiver responds. */
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
	/* Power-cycle the peripheral so no Bluetooth-controller state remains. */
	NRF_RADIO->POWER = 0;
	NRF_RADIO->POWER = 1;

	NRF_RADIO->MODE = G4B_ESB_MODE_BLE_2MBIT;
	NRF_RADIO->TXPOWER = G4B_ESB_TXPOWER_P4DBM;

	/* Fast ramp-up (RU=1), DTX=Center: shrinks TX->RX turnaround from ~130us
	 * to ~40us so a quick pairing reply is not missed. Frame content on air is
	 * unchanged, so the dongle sees the same request either way. */
	NRF_RADIO->MODECNF0 = 0x00000201u;

	/* Static-length framing: no S0/LENGTH field, four-byte address,
	 * big-endian bit order, and whitening enabled. */
	NRF_RADIO->PCNF0 = 0u;
	NRF_RADIO->PCNF1 = 0x030300FEu | ((uint32_t)statlen << 8);

	NRF_RADIO->BASE0 = G4B_ESB_BASE0;
	NRF_RADIO->PREFIX0 = G4B_ESB_PREFIX0;
	NRF_RADIO->TXADDRESS = G4B_ESB_TXADDRESS;
	NRF_RADIO->RXADDRESSES = G4B_ESB_RXADDRESSES;

	NRF_RADIO->CRCCNF = G4B_ESB_CRCCNF;
	NRF_RADIO->CRCPOLY = G4B_ESB_CRCPOLY;
	NRF_RADIO->CRCINIT = G4B_ESB_CRCINIT;
	NRF_RADIO->DATAWHITEIV = G4B_ESB_DATAWHITEIV;
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

static uint8_t esb_rxbuf[G4B_ESB_BUFSZ] __aligned(4);

static uint32_t esb_us_since(uint32_t start_cyc)
{
	return k_cyc_to_us_floor32(k_cycle_get_32() - start_cyc);
}

static void esb_radio_disable(void)
{
	uint32_t start = k_cycle_get_32();

	/* Clear SHORTS first: a still-set DISABLED_RXEN turns this DISABLED straight
	 * back into an RX ramp, so the radio never stays disabled and the next TXEN
	 * wedges. */
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

/* --- active pairing TX + reply listen ------------------------------------ */

#define G4B_ESB_PAIR_TX_STATLEN 19u
#define G4B_ESB_PAIR_RX_STATLEN 38u
#define G4B_ESB_NCH          81u
#define G4B_ESB_CLASS_40     0x40u
#define G4B_ESB_CLASS_50     0x50u
#define G4B_ESB_CLASS_60     0x60u

/* The development receiver retains this ID in its pairing record. Pair mode
 * does not erase it, so the first packet must also use its derived CRC seed. */
#define G4B_ESB_PAIR_ID      0x00007530u
#define G4B_ESB_PAIR_AUX     0x0000u
/* Pairing starts before any network ID is learned, and the receiver seeds its
 * first records with CRCINIT=1, not the operational-link 0x00FFFFFF (confirmed
 * by the live trace in work/DONGLE_RE_2026-09-04.md). */
#define G4B_ESB_PAIR_INITIAL_CRCINIT 1u

/* First prove that the recovered PHY can hear the receiver's autonomous
 * pairing transmission. This is deliberately a run-time branch so the active
 * handshake code remains in the same test image for the next experiment. */
#define G4B_ESB_PASSIVE_PAIR_SCAN 1u
#define G4B_ESB_PAIR_FIRST_CH     2u
#define G4B_ESB_PAIR_LAST_CH      80u
#define G4B_ESB_PAIR_DWELL_US     20000u

#define G4B_ESB_DUMPLEN 60u

static uint8_t esb_txbuf[G4B_ESB_BUFSZ] __aligned(4);

static void esb_set_pair_phy(uint32_t crcinit, uint8_t statlen)
{
	esb_radio_disable();
	NRF_RADIO->PCNF0 = 0u;
	NRF_RADIO->PCNF1 = 0x030300FEu |
			   ((uint32_t)statlen << 8);
	NRF_RADIO->BASE0 = G4B_ESB_BASE0;
	NRF_RADIO->PREFIX0 = G4B_ESB_PREFIX0;
	NRF_RADIO->CRCINIT = crcinit;
	NRF_RADIO->DATAWHITEIV = G4B_ESB_DATAWHITEIV;
}

/* The receiver folds the learned 32-bit ID into CRCINIT after class 0x40 and
 * again after class 0x50. A zero result is replaced by one. */
static uint32_t esb_pair_crcinit(uint32_t id)
{
	uint32_t crc = (id ^ (id >> 8)) & 0x00FFFFFFu;

	return (crc != 0u) ? crc : 1u;
}

static void esb_build_pair(uint8_t opcode, uint16_t value)
{
	/* The stock builder supplies a three-byte body to a device slot whose
	 * configured static radio length is 16; the remaining bytes stay zero. */
	memset(esb_txbuf, 0, G4B_ESB_PAIR_TX_STATLEN);
	esb_txbuf[0] = opcode;
	esb_txbuf[1] = (uint8_t)value;
	esb_txbuf[2] = (uint8_t)(value >> 8);
}

/* "APXPAIRRX ch=NN len=LL crc=C rxcrc=N b=<hex>" - one frame heard in the
 * reply window. CRC-bad frames are useful while identifying the exact static
 * length, but they are never counted as valid replies. */
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
	esb_app_str(line, &n, " rxcrc=");
	esb_app_u32(line, &n, NRF_RADIO->RXCRC);
	esb_app_str(line, &n, " b=");
	for (uint8_t i = 0; i < nb; i++) {
		esb_app_hex8(line, &n, buf[i]);
	}
	line[n++] = '\r';
	line[n++] = '\n';
	g4b_evidence_emit_text(line, n);
}

static K_SEM_DEFINE(esb_go, 0, 1);

/* "APXLST len=LL gap=NN end=E crcok=C tx=T" - one pairing sweep. */
static void esb_emit_lst(uint8_t len, uint32_t gap, uint32_t end, uint32_t crcok,
			 uint32_t tx)
{
	uint8_t line[96];
	uint32_t n = 0u;

	esb_app_str(line, &n, "APXLST len=");
	esb_app_u32(line, &n, len);
	esb_app_str(line, &n, " gap=");
	esb_app_u32(line, &n, gap);
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

static void esb_emit_scan(uint32_t pass, uint32_t address, uint32_t end,
			  uint32_t crcok)
{
	uint8_t line[96];
	uint32_t n = 0u;

	esb_app_str(line, &n, "APXSCAN pass=");
	esb_app_u32(line, &n, pass);
	esb_app_str(line, &n, " addr=");
	esb_app_u32(line, &n, address);
	esb_app_str(line, &n, " end=");
	esb_app_u32(line, &n, end);
	esb_app_str(line, &n, " crcok=");
	esb_app_u32(line, &n, crcok);
	line[n++] = '\r';
	line[n++] = '\n';
	g4b_evidence_emit_text(line, n);
}

static void esb_pair_listen_dwell(uint8_t ch, uint32_t timeout_us,
				  uint32_t crcinit,
				  uint32_t *address, uint32_t *end,
				  uint32_t *crcok)
{
	uint32_t start;

	esb_set_pair_phy(crcinit,
			 G4B_ESB_PAIR_RX_STATLEN);
	memset(esb_rxbuf, 0, G4B_ESB_PAIR_RX_STATLEN);
	NRF_RADIO->FREQUENCY = ch;
	NRF_RADIO->PACKETPTR = (uint32_t)esb_rxbuf;
	NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk |
			    RADIO_SHORTS_END_DISABLE_Msk;
	NRF_RADIO->EVENTS_ADDRESS = 0;
	NRF_RADIO->EVENTS_END = 0;
	NRF_RADIO->EVENTS_DISABLED = 0;
	NRF_RADIO->TASKS_RXEN = 1;

	start = k_cycle_get_32();
	while (esb_us_since(start) <= timeout_us) {
		if (NRF_RADIO->EVENTS_ADDRESS != 0) {
			NRF_RADIO->EVENTS_ADDRESS = 0;
			(*address)++;
		}

		if (NRF_RADIO->EVENTS_END != 0) {
			int crc = (NRF_RADIO->CRCSTATUS != 0) ? 1 : 0;

			NRF_RADIO->EVENTS_END = 0;
			(*end)++;
			if (crc != 0) {
				(*crcok)++;
			}
			esb_dump_reply(ch, G4B_ESB_PAIR_RX_STATLEN, crc,
				       esb_rxbuf);

			while (NRF_RADIO->EVENTS_DISABLED == 0) {
				/* END_DISABLE completes within a few radio clocks. */
			}
			NRF_RADIO->EVENTS_DISABLED = 0;
			NRF_RADIO->TASKS_RXEN = 1;
		}
	}

	esb_radio_disable();
}

static int esb_pair_transaction(uint8_t ch, uint16_t id_low,
				uint16_t id_high, uint32_t crc_after_40,
				uint32_t crc_after_50, uint32_t gap_us,
				uint32_t *txok)
{
	int rx;

	/* All three operations use the public pairing address. The receiver changes
	 * CRCINIT as soon as it accepts each half, so do the same without another
	 * disable/reconfigure cycle between the two transmissions. */
	NRF_RADIO->CRCINIT = G4B_ESB_PAIR_INITIAL_CRCINIT;
	NRF_RADIO->PCNF1 = 0x030300FEu |
			   ((uint32_t)G4B_ESB_PAIR_TX_STATLEN << 8);
	esb_build_pair(G4B_ESB_CLASS_40, id_low);
	if (g4b_esb_tx(ch, esb_txbuf)) {
		(*txok)++;
	}

	NRF_RADIO->CRCINIT = crc_after_40;
	esb_build_pair(G4B_ESB_CLASS_50, id_high);
	k_busy_wait(gap_us);
	if (g4b_esb_tx(ch, esb_txbuf)) {
		(*txok)++;
	}

	NRF_RADIO->CRCINIT = crc_after_50;
	NRF_RADIO->PCNF1 = 0x030300FEu |
			   ((uint32_t)G4B_ESB_PAIR_RX_STATLEN << 8);
	memset(esb_rxbuf, 0, G4B_ESB_PAIR_RX_STATLEN);
	rx = g4b_esb_rx(ch, esb_rxbuf, 2000u);
	if (rx >= 0) {
		esb_dump_reply(ch, G4B_ESB_PAIR_RX_STATLEN, rx, esb_rxbuf);
	}
	return rx;
}

static void esb_pair_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	/* Block until the radio is freed + configured (dongle-mode standdown). */
	k_sem_take(&esb_go, K_FOREVER);

	g4b_evidence_emit_text((const uint8_t *)"APXPAIR start\r\n", 15u);

	const uint16_t id_low = (uint16_t)G4B_ESB_PAIR_ID;
	const uint16_t id_high = (uint16_t)(G4B_ESB_PAIR_ID >> 16);
	const uint32_t crc_after_40 = esb_pair_crcinit(id_low);
	const uint32_t crc_after_50 = esb_pair_crcinit(G4B_ESB_PAIR_ID);
	uint32_t pass = 0u;

	g4b_esb_configure(G4B_ESB_PAIR_RX_STATLEN);

	if (G4B_ESB_PASSIVE_PAIR_SCAN != 0u) {
		/* CRCINIT candidates the receiver walks during pairing: the
		 * confirmed initial seed 1 (before any network ID is learned),
		 * then folded-ID candidates for after the 0x40/0x50 halves are
		 * received. See work/DONGLE_RE_2026-09-04.md. */
		static const uint32_t scan_crcinit[] = {
			1u, 0x00007545u, 0x00DA2075u,
		};

		for (;;) {
			uint32_t address = 0u, end = 0u, crcok = 0u;
			uint32_t crcinit = scan_crcinit[pass %
				(sizeof(scan_crcinit) / sizeof(scan_crcinit[0]))];

			for (uint8_t ch = G4B_ESB_PAIR_FIRST_CH;
			     ch <= G4B_ESB_PAIR_LAST_CH; ch++) {
				esb_pair_listen_dwell(ch, G4B_ESB_PAIR_DWELL_US,
						      crcinit,
						      &address, &end, &crcok);
			}

			pass++;
			esb_emit_scan(pass, address, end, crcok);
		}
	}

	static const uint16_t gap_us[] = {
		0u, 40u, 80u, 160u, 320u, 640u, 1000u,
	};

	for (;;) {
		uint32_t txok = 0u, end = 0u, crcok = 0u;
		uint32_t gap = gap_us[pass %
			(sizeof(gap_us) / sizeof(gap_us[0]))];

		for (uint8_t ch = G4B_ESB_PAIR_FIRST_CH;
		     ch <= G4B_ESB_PAIR_LAST_CH; ch++) {
			int r;

			r = esb_pair_transaction(ch, id_low, id_high,
						 crc_after_40, crc_after_50,
						 gap, &txok);
			if (r >= 0) { end++; }
			if (r == 1) { crcok++; }
		}

		pass++;
		esb_emit_lst(G4B_ESB_PAIR_TX_STATLEN, gap,
			     end, crcok, txok);
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

	/* Read the registers back and report: confirms on hardware that the standdown
	 * freed NRF_RADIO and the config sticks. */
	esb_emit_config(hf);

#if IS_ENABLED(CONFIG_APEX_G4B_DONGLE_LINK)
	/* Operational-link build: the receiver is already bonded, so skip the
	 * pairing probe and go straight to the A0/90/90 counter exchange and the
	 * type-2 B0 handoff. The pairing thread stays parked on esb_go. */
	g4b_dongle_link_start();
#else
	/* Release the pairing-TX thread now the radio is free and configured. */
	k_sem_give(&esb_go);
#endif
}
