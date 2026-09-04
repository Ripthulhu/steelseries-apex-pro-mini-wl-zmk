/* SPDX-License-Identifier: MIT
 *
 * Direct-register access to the external FM25Q08A SPI-NOR (1 MiB) over SPIM0.
 * The module provides read-only evidence dumps, legacy staged-image helpers,
 * and the generic flash operations used by NVS, A/B fallback, and coredumps.
 *
 * SPIM0 drives SCK/MOSI/MISO (P0.27/00/01), mirroring the SPIM3 register setup
 * in spim_g4b.c. The flash's chip select is P0.26 and is NOT wired to SPIM0's
 * CSN (stock bit-bangs it), so it is driven through the sanctioned pin
 * helpers - held low across each command+read, idle high otherwise.
 */

#include <string.h>

#include <zephyr/kernel.h>

#include <errno.h>

#include <nrfx.h>

#include "evidence_g4b.h"
#include "pins_g4b.h"
#include "spim_g4b.h"    /* g4b_extbus_lock/unlock/wait_replay - shared bus guard */
#include "spinor_g4b.h"
#include "nor_layout_g4b.h"

#define G4B_NOR_SCK   27u
#define G4B_NOR_MOSI  0u
#define G4B_NOR_MISO  1u
#define G4B_NOR_CS    26u          /* bit-banged chip select */
#define G4B_NOR_SIZE  G4B_NOR_TOTAL_SIZE

#define G4B_NOR_CMD_JEDEC 0x9Fu
#define G4B_NOR_CMD_READ  0x03u

#define G4B_NOR_SPIM_M2   0x20000000u /* 2 Mbit/s, conservative; UART-bound anyway */
#define G4B_NOR_WAIT_CYC  (64000000u / 10u) /* ~100 ms per transfer, bounded */

/* EasyDMA needs RAM buffers. TX carries the command+address; the flash's reply
 * lands in RX after those same bytes, so the payload is rxbuf[cmdlen..]. Static,
 * not on the stack - one holds a whole 4 KiB chunk plus the 4-byte command. */
static uint8_t nor_txbuf[4];
static uint8_t nor_rxbuf[4 + G4B_S5_CHUNK];

#if IS_ENABLED(CONFIG_APEX_G4B_SPINOR_DUMP)
static struct g4b_dump_chunk nor_chunk;

/* Same CRC32 the S5 dump uses (poly 0xEDB88320, running, ~final), so the host
 * decoder validates NOR chunks with no change. */
static uint32_t nor_crc32(const uint8_t *data, uint32_t length, uint32_t crc)
{
    for (uint32_t i = 0u; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return crc;
}
#endif /* SPINOR_DUMP */

static void nor_enable(void)
{
    /* CS high (deselected) before it is ever driven, then an output. */
    g4b_pin_set(G4B_PORT0, (enum g4b_pin)G4B_NOR_CS);
    g4b_pin_cfg(G4B_PORT0, (enum g4b_pin)G4B_NOR_CS, G4B_CNF_EN_OUT);

    g4b_pin_clr(G4B_PORT0, (enum g4b_pin)G4B_NOR_SCK);
    g4b_pin_cfg(G4B_PORT0, (enum g4b_pin)G4B_NOR_SCK, G4B_CNF_SCK_OUT);
    g4b_pin_clr(G4B_PORT0, (enum g4b_pin)G4B_NOR_MOSI);
    g4b_pin_cfg(G4B_PORT0, (enum g4b_pin)G4B_NOR_MOSI, G4B_CNF_MOSI_OUT);
    g4b_pin_cfg(G4B_PORT0, (enum g4b_pin)G4B_NOR_MISO, G4B_CNF_MISO_IN);

    NRF_SPIM0->PSEL.SCK = G4B_NOR_SCK;
    NRF_SPIM0->PSEL.MOSI = G4B_NOR_MOSI;
    NRF_SPIM0->PSEL.MISO = G4B_NOR_MISO;
    NRF_SPIM0->PSEL.CSN = 0xFFFFFFFFu; /* CS is bit-banged, not by SPIM */
    NRF_SPIM0->FREQUENCY = G4B_NOR_SPIM_M2;
    NRF_SPIM0->CONFIG = 0u;             /* mode 0, MSB first */
    NRF_SPIM0->ORC = 0xFFu;             /* MOSI idle high during the read phase */
    NRF_SPIM0->SHORTS = 0u;
    NRF_SPIM0->INTENCLR = 0xFFFFFFFFu;
    NRF_SPIM0->ENABLE = 7u;
    __DSB();
}

static void nor_disable(void)
{
    NRF_SPIM0->ENABLE = 0u;
    __DSB();
    NRF_SPIM0->PSEL.SCK = 0xFFFFFFFFu;
    NRF_SPIM0->PSEL.MOSI = 0xFFFFFFFFu;
    NRF_SPIM0->PSEL.MISO = 0xFFFFFFFFu;
    /* Release EVERY pin this driver drove back to a plain input, so nothing is
     * left driving the flash's bus between ops. SPIM disconnect alone is not
     * enough: nor_enable() left P0.27(SCK)/P0.00(MOSI) configured as OUTPUTs in
     * their PIN_CNF, and disconnecting PSEL hands the pin back to GPIO still an
     * output. Restoring them (plus the bit-banged CS) to inputs leaves instance-0
     * fully idle and the pins high-Z - clean symmetry for open/close-per-op.
     * (P0.00/P0.01 double as the LFXO's XL1/XL2; we run LFCLK on the internal RC
     * so nothing else wants them, but leaving them undriven is still correct.) */
    g4b_pin_cfg(G4B_PORT0, (enum g4b_pin)G4B_NOR_SCK, G4B_CNF_IN_NOPULL);
    g4b_pin_cfg(G4B_PORT0, (enum g4b_pin)G4B_NOR_MOSI, G4B_CNF_IN_NOPULL);
    g4b_pin_cfg(G4B_PORT0, (enum g4b_pin)G4B_NOR_CS, G4B_CNF_IN_NOPULL);
}

/* One command+read transaction with CS held low across it. Clocks cmdlen+outlen
 * bytes: the first cmdlen are the command/address from nor_txbuf, the rest are
 * ORC while the flash shifts data out onto MISO into nor_rxbuf. */
static bool nor_xfer(uint32_t cmdlen, uint8_t *out, uint32_t outlen)
{
    uint32_t start;
    bool ok = true;

    g4b_pin_clr(G4B_PORT0, (enum g4b_pin)G4B_NOR_CS); /* select */
    __DSB();

    NRF_SPIM0->TXD.PTR = (uint32_t)nor_txbuf;
    NRF_SPIM0->TXD.MAXCNT = cmdlen;
    NRF_SPIM0->RXD.PTR = (uint32_t)nor_rxbuf;
    NRF_SPIM0->RXD.MAXCNT = cmdlen + outlen;
    NRF_SPIM0->TXD.LIST = 0u;
    NRF_SPIM0->RXD.LIST = 0u;
    NRF_SPIM0->EVENTS_END = 0u;
    __DSB();
    NRF_SPIM0->TASKS_START = 1u;

    start = g4b_cyccnt();
    while (NRF_SPIM0->EVENTS_END == 0u) {
        if ((g4b_cyccnt() - start) > G4B_NOR_WAIT_CYC) {
            ok = false;
            break;
        }
    }

    g4b_pin_set(G4B_PORT0, (enum g4b_pin)G4B_NOR_CS); /* deselect */
    __DSB();

    if (ok && out != NULL) {
        memcpy(out, nor_rxbuf + cmdlen, outlen);
    }
    return ok;
}

static bool nor_read(uint32_t addr, uint8_t *out, uint32_t len)
{
    /* nor_xfer DMAs cmdlen+outlen bytes into nor_rxbuf[4 + G4B_S5_CHUNK], so one
     * transfer's payload must not exceed G4B_S5_CHUNK. Chunk larger reads: a
     * flash_read of a >4 KiB buffer through the LittleFS/flash API can request
     * more than the buffer holds, which would otherwise run EasyDMA past it. The
     * SPI-NOR READ (0x03) is restarted at the new address each chunk, so the
     * bytes returned are identical to one long read. */
    while (len > 0u) {
        uint32_t n = (len < G4B_S5_CHUNK) ? len : G4B_S5_CHUNK;

        nor_txbuf[0] = G4B_NOR_CMD_READ;
        nor_txbuf[1] = (uint8_t)(addr >> 16);
        nor_txbuf[2] = (uint8_t)(addr >> 8);
        nor_txbuf[3] = (uint8_t)addr;
        if (!nor_xfer(4u, out, n)) {
            return false;
        }
        addr += n;
        out += n;
        len -= n;
    }
    return true;
}

/* --- WRITE path. Only compiled where a write build asks for it. Erase/program
 * on this chip are standard SPI-NOR: 0x06 WREN, 0x20 4 KiB sector erase, 0x02
 * page program (<=256 B, must not cross a 256 B page), 0x05 status with WIP in
 * bit 0. Every erase/program waits for WIP to clear, bounded, before returning. */
#if IS_ENABLED(CONFIG_APEX_G4B_SPINOR_WRITETEST) || \
    IS_ENABLED(CONFIG_APEX_G4B_SPINOR_WRITE) || \
    IS_ENABLED(CONFIG_APEX_G4B_COREDUMP)
#define G4B_NOR_CMD_WREN   0x06u
#define G4B_NOR_CMD_RDSR   0x05u
#define G4B_NOR_CMD_SE4K   0x20u
#define G4B_NOR_CMD_PP     0x02u
#define G4B_NOR_SR_WIP     0x01u
#define G4B_NOR_ERASE_CYC  (64000000u)       /* up to 1 s for a sector erase */
#define G4B_NOR_PROG_CYC   (64000000u / 10u) /* up to 100 ms for a page program */

static uint8_t nor_pgbuf[4 + 256];

/* Send len command/data bytes with CS held, no read phase. */
static bool nor_tx(const uint8_t *tx, uint32_t len)
{
    uint32_t start;
    bool ok = true;

    g4b_pin_clr(G4B_PORT0, (enum g4b_pin)G4B_NOR_CS);
    __DSB();
    NRF_SPIM0->TXD.PTR = (uint32_t)tx;
    NRF_SPIM0->TXD.MAXCNT = len;
    NRF_SPIM0->RXD.PTR = (uint32_t)nor_rxbuf;
    NRF_SPIM0->RXD.MAXCNT = 0u;
    NRF_SPIM0->TXD.LIST = 0u;
    NRF_SPIM0->RXD.LIST = 0u;
    NRF_SPIM0->EVENTS_END = 0u;
    __DSB();
    NRF_SPIM0->TASKS_START = 1u;
    start = g4b_cyccnt();
    while (NRF_SPIM0->EVENTS_END == 0u) {
        if ((g4b_cyccnt() - start) > G4B_NOR_WAIT_CYC) {
            ok = false;
            break;
        }
    }
    g4b_pin_set(G4B_PORT0, (enum g4b_pin)G4B_NOR_CS);
    __DSB();
    return ok;
}

static uint8_t nor_status(void)
{
    uint8_t st = 0xFFu;

    nor_txbuf[0] = G4B_NOR_CMD_RDSR;
    (void)nor_xfer(1u, &st, 1u);
    return st;
}

static bool nor_wait_wip(uint32_t max_cyc)
{
    uint32_t start = g4b_cyccnt();

    while ((nor_status() & G4B_NOR_SR_WIP) != 0u) {
        if ((g4b_cyccnt() - start) > max_cyc) {
            return false;
        }
    }
    return true;
}

static void nor_wren(void)
{
    uint8_t c = G4B_NOR_CMD_WREN;

    (void)nor_tx(&c, 1u);
}

static bool nor_sector_erase(uint32_t addr)
{
    uint8_t cmd[4] = { G4B_NOR_CMD_SE4K, (uint8_t)(addr >> 16),
                       (uint8_t)(addr >> 8), (uint8_t)addr };

    nor_wren();
    if (!nor_tx(cmd, 4u)) {
        return false;
    }
    return nor_wait_wip(G4B_NOR_ERASE_CYC);
}

static bool nor_page_program(uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (len > 256u) {
        return false;
    }
    nor_pgbuf[0] = G4B_NOR_CMD_PP;
    nor_pgbuf[1] = (uint8_t)(addr >> 16);
    nor_pgbuf[2] = (uint8_t)(addr >> 8);
    nor_pgbuf[3] = (uint8_t)addr;
    memcpy(nor_pgbuf + 4, data, len);

    nor_wren();
    if (!nor_tx(nor_pgbuf, 4u + len)) {
        return false;
    }
    return nor_wait_wip(G4B_NOR_PROG_CYC);
}
#endif /* WRITETEST || WRITE || COREDUMP */

#if IS_ENABLED(CONFIG_APEX_G4B_SPINOR_WRITE)
/* Staged Nordic firmware file (fs=3/file=11): a raw byte copy of vendor.bin at
 * external offset 0x014000, which the loader applies to the live app slot on
 * reset. STAGE_END rounds the 307,200-byte image up to a 4 KiB sector. */
#define G4B_STAGE_BASE 0x014000u
#define G4B_STAGE_SIZE 0x4B000u              /* 307200, the vendor.bin size */
#define G4B_STAGE_END  (G4B_STAGE_BASE + G4B_STAGE_SIZE)

void g4b_spinor_open(void)  { nor_enable(); }
void g4b_spinor_close(void) { nor_disable(); }

/* Erase exactly the staged-firmware region and nothing else (not the config
 * profiles below it, not the free tail above it). Assumes the bus is open. */
bool g4b_spinor_stage_erase(void)
{
    for (uint32_t a = G4B_STAGE_BASE; a < G4B_STAGE_END; a += 0x1000u) {
        if (!nor_sector_erase(a)) {
            return false;
        }
    }
    return true;
}

/* Program up to a chunk into the staged file at image offset `off`, splitting at
 * 256-byte page boundaries (a page-program must not cross one). Assumes the
 * region was erased and the bus is open. */
bool g4b_spinor_stage_write(uint32_t off, const uint8_t *data, uint32_t len)
{
    uint32_t addr = G4B_STAGE_BASE + off;

    if ((uint64_t)off + len > G4B_STAGE_SIZE) {
        return false; /* refuse to write past the staged region (64-bit: no wrap) */
    }
    while (len > 0u) {
        uint32_t page_left = 256u - (addr & 0xFFu);
        uint32_t n = (len < page_left) ? len : page_left;

        if (!nor_page_program(addr, data, n)) {
            return false;
        }
        addr += n;
        data += n;
        len -= n;
    }
    return true;
}

/* Read back from the staged file, for host-side verification. Bus open. */
bool g4b_spinor_stage_read(uint32_t off, uint8_t *out, uint32_t len)
{
    if ((uint64_t)off + len > G4B_STAGE_SIZE) {
        return false;
    }
    return nor_read(G4B_STAGE_BASE + off, out, len);
}
#endif /* SPINOR_WRITE */

#if IS_ENABLED(CONFIG_APEX_G4B_SPINOR_FLASHDEV)
/* Generic whole-device backing for the Zephyr flash driver (flash_spinor_g4b.c).
 * Offsets are absolute within the 1 MiB device. 4 KiB erase sectors, 256 B
 * page-program. Returns 0 or a negative errno.
 *
 * TWO invariants, both required, both explained at the g4b_extbus_* note in
 * spim_g4b.h:
 *   1. Open/close SPIM0 around EACH op (nor_enable/nor_disable) - never hold the
 *      bus or the pins between ops. The always-on variant permanently claimed
 *      instance-0 and left P0.27/P0.00 driven.
 *   2. Every op takes the BUS-WIDE g4b_extbus lock (shared with the SPIM3
 *      scanner exchange), NOT a SPIM0-only mutex. A SPIM0-only lock serialises
 *      NVS-vs-NVS but does nothing to stop an NVS op preempting the g4b thread
 *      mid SPIM3 exchange - which was the actual keyboard-stall root cause.
 * WRITES and ERASES additionally wait out the boot replay window (a ~1 s erase
 * in a replay-frame gap desyncs the scanner); READS never wait (settings_load
 * runs on main before the replay; gating a read could deadlock main). */

int g4b_spinor_dev_init(void)
{
    /* Do NOT hold the bus open; each op opens+closes it. */
    return 0;
}

int g4b_spinor_dev_read(uint32_t addr, void *out, uint32_t len)
{
    bool ok;

    if ((uint64_t)addr + len > G4B_NOR_SIZE) {
        return -EINVAL;
    }
    g4b_extbus_lock();
    nor_enable();
    ok = nor_read(addr, out, len);
    nor_disable();
    g4b_extbus_unlock();
    return ok ? 0 : -EIO;
}

int g4b_spinor_dev_erase(uint32_t addr, uint32_t size)
{
    bool ok = true;

    if ((addr & 0xFFFu) || (size & 0xFFFu) ||
        (uint64_t)addr + size > G4B_NOR_SIZE) {
        return -EINVAL;
    }
    g4b_extbus_wait_replay();
    /* Lock/enable/erase/disable/unlock PER SECTOR rather than once around the
     * whole range. A single 4 KiB erase can busy-wait up to ~1 s; holding the
     * bus lock across a multi-sector erase (e.g. NVS first-format or a big GC)
     * would block the scanner for seconds. Releasing between sectors lets a
     * pending SPIM3 exchange run in the gaps, bounding the scanner's worst-case
     * stall to one sector. */
    for (uint32_t a = addr; a < addr + size; a += 0x1000u) {
        g4b_extbus_lock();
        nor_enable();
        ok = nor_sector_erase(a);
        nor_disable();
        g4b_extbus_unlock();
        if (!ok) {
            break;
        }
    }
    return ok ? 0 : -EIO;
}

int g4b_spinor_dev_program(uint32_t addr, const void *data, uint32_t len)
{
    const uint8_t *p = data;
    bool ok = true;

    if ((uint64_t)addr + len > G4B_NOR_SIZE) {
        return -EINVAL;
    }
    g4b_extbus_wait_replay();
    /* Page-program is bounded (<=256 B, ~100 us to a few ms), so one lock span
     * for the whole write is fine - it never approaches the erase's ~1 s. */
    g4b_extbus_lock();
    nor_enable();
    while (len > 0u) {
        uint32_t page_left = 256u - (addr & 0xFFu);
        uint32_t n = (len < page_left) ? len : page_left;

        if (!nor_page_program(addr, p, n)) {
            ok = false;
            break;
        }
        addr += n;
        p += n;
        len -= n;
    }
    nor_disable();
    g4b_extbus_unlock();
    return ok ? 0 : -EIO;
}
#endif /* SPINOR_FLASHDEV */

#if IS_ENABLED(CONFIG_APEX_G4B_SPINOR_DUMP)
static void nor_emit_jedec(const uint8_t id[3])
{
    static const char hexd[] = "0123456789abcdef";
    const char *tag = "APXN jedec=";
    uint8_t line[24];
    uint32_t n = 0u;

    while (*tag != '\0') {
        line[n++] = (uint8_t)*tag++;
    }
    for (int i = 0; i < 3; i++) {
        line[n++] = (uint8_t)hexd[id[i] >> 4];
        line[n++] = (uint8_t)hexd[id[i] & 0x0Fu];
    }
    line[n++] = (uint8_t)13;
    line[n++] = (uint8_t)10;
    g4b_evidence_emit_text(line, n);
}

void g4b_spinor_dump(void)
{
    uint8_t id[3] = { 0u, 0u, 0u };
    uint16_t seq = 0u;

    nor_enable();

    /* JEDEC ID first: A1 40 14 confirms the FM25Q08A and that SPIM0 reads it,
     * before committing 90 s of UART to the full dump. */
    nor_txbuf[0] = G4B_NOR_CMD_JEDEC;
    (void)nor_xfer(1u, id, 3u);
    nor_emit_jedec(id);

    g4b_evidence_dump_open();
    for (uint32_t addr = 0u; addr < G4B_NOR_SIZE; addr += G4B_S5_CHUNK) {
        uint32_t crc = 0xFFFFFFFFu;

        nor_chunk.magic = G4B_S5_MAGIC;
        nor_chunk.version = G4B_EVIDENCE_VERSION_S5;
        nor_chunk.seq = seq++;
        nor_chunk.addr = addr;
        nor_chunk.length = G4B_S5_CHUNK;
        if (!nor_read(addr, nor_chunk.data, G4B_S5_CHUNK)) {
            /* Mark a failed read distinctively rather than emitting stale RAM. */
            memset(nor_chunk.data, 0xEE, G4B_S5_CHUNK);
        }

        crc = nor_crc32((const uint8_t *)&nor_chunk.addr, sizeof(nor_chunk.addr), crc);
        crc = nor_crc32((const uint8_t *)&nor_chunk.length, sizeof(nor_chunk.length), crc);
        crc = nor_crc32(nor_chunk.data, G4B_S5_CHUNK, crc);
        nor_chunk.crc32 = ~crc;

        g4b_evidence_dump_chunk(&nor_chunk);
    }
    g4b_evidence_dump_close();

    nor_disable();
}
#endif /* SPINOR_DUMP */

#if IS_ENABLED(CONFIG_APEX_G4B_SPINOR_WRITETEST)
/* Prove the SPIM0 WRITE path on a spare, blank region (0x080000) - nothing on
 * the device reads it, and the sector is erased again at the end, so this
 * touches no config, no profiles, and NOT the staged firmware. Erase a sector,
 * program 256 known bytes, read them back, and report the match over UART.
 * A clean pass (e=1 p=1 match=256) is the go-ahead for the real staged-file
 * write; it means erase + program + read-back all work on this chip. */
void g4b_spinor_write_test(void)
{
    const uint32_t addr = G4B_NOR_RAW_TEST_ADDR;
    static const char hexd[] = "0123456789abcdef";
    static uint8_t pattern[256];
    static uint8_t rb[256];
    uint8_t before[4] = {0}, erased[4] = {0};
    uint8_t line[64];
    uint32_t n = 0u, match = 0u;
    const char *tag;
    bool e, p;

    for (uint32_t i = 0u; i < 256u; i++) {
        pattern[i] = (uint8_t)(0xA5u ^ (uint8_t)i);
    }

    nor_enable();
    nor_read(addr, before, 4u);
    e = nor_sector_erase(addr);
    nor_read(addr, erased, 4u);
    p = nor_page_program(addr, pattern, 256u);
    nor_read(addr, rb, 256u);
    (void)nor_sector_erase(addr); /* restore the spare sector to blank (0xFF) */
    nor_disable();

    for (uint32_t i = 0u; i < 256u; i++) {
        if (rb[i] == pattern[i]) {
            match++;
        }
    }

    tag = "APXW e=";
    while (*tag != '\0') { line[n++] = (uint8_t)*tag++; }
    line[n++] = e ? '1' : '0';
    tag = " p=";
    while (*tag != '\0') { line[n++] = (uint8_t)*tag++; }
    line[n++] = p ? '1' : '0';
    tag = " match=";
    while (*tag != '\0') { line[n++] = (uint8_t)*tag++; }
    if (match >= 100u) { line[n++] = (uint8_t)('0' + match / 100u); }
    if (match >= 10u) { line[n++] = (uint8_t)('0' + (match / 10u) % 10u); }
    line[n++] = (uint8_t)('0' + match % 10u);
    tag = " before=";
    while (*tag != '\0') { line[n++] = (uint8_t)*tag++; }
    line[n++] = (uint8_t)hexd[before[0] >> 4]; line[n++] = (uint8_t)hexd[before[0] & 0xF];
    tag = " erased=";
    while (*tag != '\0') { line[n++] = (uint8_t)*tag++; }
    line[n++] = (uint8_t)hexd[erased[0] >> 4]; line[n++] = (uint8_t)hexd[erased[0] & 0xF];
    tag = " rb0=";
    while (*tag != '\0') { line[n++] = (uint8_t)*tag++; }
    line[n++] = (uint8_t)hexd[rb[0] >> 4]; line[n++] = (uint8_t)hexd[rb[0] & 0xF];
    line[n++] = (uint8_t)13;
    line[n++] = (uint8_t)10;
    g4b_evidence_emit_text(line, n);
}
#endif /* SPINOR_WRITETEST */

#if IS_ENABLED(CONFIG_APEX_G4B_COREDUMP)
/* --- Crash/coredump ring backing. See src/coredump_g4b.c and spinor_g4b.h.
 *
 * g4b_spinor_fault_write() is the ONLY SPIM0 path that runs in fault context.
 * It is lock-free on purpose: an exception handler must not take the g4b_extbus
 * K_MUTEX (a k_mutex_lock can sleep and would fault or deadlock), nor wait the
 * replay gate. Everything it calls - nor_enable/nor_disable, nor_wait_wip,
 * nor_sector_erase, nor_page_program - is a bounded busy-wait over registers and
 * the sanctioned pin helpers, with no kernel object in sight. The interrupted
 * thread (possibly an NVS op mid-transfer on this same bus) is suspended, so we
 * own SPIM0; nor_enable() re-initialises the peripheral from scratch, abandoning
 * any half-done transfer, and the WIP preflight below waits out a NOR erase or
 * program the interrupted op may have left in flight before we issue our own. */
bool g4b_spinor_fault_write(uint32_t addr, const uint8_t *rec, uint32_t len)
{
    bool ok;

    if (rec == NULL || len == 0u || len > 256u) {
        return false;
    }

    nor_enable();

    /* PREFLIGHT the WIP bit. A settings save may have been mid page-program or
     * mid sector-erase on this chip when the fault hit; issuing WREN + erase on
     * top of a busy device is ignored and would corrupt the slot. Wait it out
     * (bounded to the erase ceiling) before touching anything. */
    if (!nor_wait_wip(G4B_NOR_ERASE_CYC)) {
        nor_disable();
        return false;
    }

    ok = nor_sector_erase(addr);          /* one 4 KiB slot; waits WIP */
    if (ok) {
        ok = nor_page_program(addr, rec, len); /* <=256 B, one page; waits WIP */
    }

    nor_disable();
    return ok;
}

/* Boot-time ring read. NOT for fault context: it takes the shared external-bus
 * lock (a no-op inline unless CONFIG_APEX_G4B_SPINOR_FLASHDEV is on, a real
 * K_MUTEX when it is) so a concurrent NVS op on the g4b thread cannot overlap
 * the SPIM0 transaction. Used by the coredump scan and the boot emit. */
bool g4b_spinor_coredump_read(uint32_t addr, uint8_t *out, uint32_t len)
{
    bool ok;

    if (out == NULL || (uint64_t)addr + len > G4B_NOR_SIZE) {
        return false;
    }

    g4b_extbus_lock();
    nor_enable();
    ok = nor_read(addr, out, len);
    nor_disable();
    g4b_extbus_unlock();
    return ok;
}
#endif /* COREDUMP */
