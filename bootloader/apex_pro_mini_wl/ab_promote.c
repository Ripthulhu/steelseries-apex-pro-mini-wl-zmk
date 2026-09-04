/* SPDX-License-Identifier: MIT
 *
 * Bootloader-side A/B rollback.
 *
 * A validated image is restored from external NOR after the configured failure
 * threshold. The descriptor, target bounds, and image CRC are checked before
 * internal flash is erased. A successful copy is verified before the
 * bootloader settings page is cleared.
 *
 * The NOR reader is single-owner and read-only. This module is compiled only
 * when APEX_ENABLE_AB_ROLLBACK is defined.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "nrf.h"
#include "nrf_gpio.h"
#include "flash_nrf5x.h"
#include "dfu_types.h"      /* BOOTLOADER_SETTINGS_ADDRESS, CODE_PAGE_SIZE */
#include "ab_promote.h"

/* NOR layout; keep in sync with src/ab_rollback_g4b.c. */
#define AB_HDR_ADDR     0x69000u
#define AB_TALLY_ADDR   0x6A000u
#define AB_SECTOR       0x1000u
#define AB_BIMG_ADDR    0x8B000u

#define AB_APP_BASE     0x1000u    /* only legal promotion target */
#ifndef APEX_AB_APP_MAXLEN
#define APEX_AB_APP_MAXLEN 0x71000u
#endif
#define AB_APP_MAXLEN   APEX_AB_APP_MAXLEN
#define AB_MAGIC        0x41423447u
#define AB_VERSION      2u         /* v2: full-size B at NOR 0x8B000 */
#define AB_FLAG_PROMOTE 0x1u

#define COREDUMP_BASE       0xFC000u
#define COREDUMP_SLOT_SIZE  0x1000u
#define COREDUMP_SLOT_COUNT 4u
#define COREDUMP_MAGIC      0x43585041u /* "APXC" */
#define COREDUMP_VERSION    1u
#define COREDUMP_SIZE       248u

enum ab_boot_action {
    AB_BOOT_UNARMED,
    AB_BOOT_WAITING,
    AB_BOOT_BAD_IMAGE,
    AB_BOOT_CURRENT,
    AB_BOOT_COPY_FAILED,
    AB_BOOT_RESTORED,
};

struct ab_boot_status {
    enum ab_boot_action action;
    uint32_t resetreas;
    uint32_t failures;
    uint32_t threshold;
    uint32_t image_len;
    bool tally_readable;
    bool crash_present;
    uint16_t crash_seq;
    uint32_t crash_reason;
    uint32_t crash_pc;
};

static struct ab_boot_status boot_status;
static uint8_t newest_crash[COREDUMP_SIZE];

/* FM25Q08A on SPIM0 with a software-controlled chip select. */
#define NOR_SCK   27u
#define NOR_MOSI  0u
#define NOR_MISO  1u
#define NOR_CS    26u
#define NOR_CMD_READ 0x03u

struct ab_header {
    uint32_t magic;
    uint32_t version;
    uint32_t b_base;
    uint32_t b_len;
    uint32_t b_crc32;
    uint32_t nor_b_off;
    uint32_t fail_thresh;
    uint32_t flags;
    uint32_t hdr_crc32;
};

/* EasyDMA needs RAM buffers. */
static uint8_t nor_tx[4];
static uint8_t nor_rx[4 + 256];

static void nor_open(void)
{
    nrf_gpio_pin_set(NOR_CS);
    nrf_gpio_cfg_output(NOR_CS);
    nrf_gpio_pin_clear(NOR_SCK);
    nrf_gpio_cfg_output(NOR_SCK);
    nrf_gpio_pin_clear(NOR_MOSI);
    nrf_gpio_cfg_output(NOR_MOSI);
    nrf_gpio_cfg_input(NOR_MISO, NRF_GPIO_PIN_NOPULL);

    NRF_SPIM0->PSEL.SCK  = NOR_SCK;
    NRF_SPIM0->PSEL.MOSI = NOR_MOSI;
    NRF_SPIM0->PSEL.MISO = NOR_MISO;
    NRF_SPIM0->PSEL.CSN  = 0xFFFFFFFFu; /* CS bit-banged, not by SPIM */
    NRF_SPIM0->FREQUENCY = 0x20000000u; /* 2 Mbit/s, conservative */
    NRF_SPIM0->CONFIG = 0u;             /* mode 0, MSB first */
    NRF_SPIM0->ORC = 0xFFu;
    NRF_SPIM0->INTENCLR = 0xFFFFFFFFu;
    NRF_SPIM0->ENABLE = 7u;
    __DSB();
}

static void nor_close(void)
{
    NRF_SPIM0->ENABLE = 0u;
    __DSB();
    NRF_SPIM0->PSEL.SCK  = 0xFFFFFFFFu;
    NRF_SPIM0->PSEL.MOSI = 0xFFFFFFFFu;
    NRF_SPIM0->PSEL.MISO = 0xFFFFFFFFu;
    nrf_gpio_cfg_input(NOR_SCK, NRF_GPIO_PIN_NOPULL);
    nrf_gpio_cfg_input(NOR_MOSI, NRF_GPIO_PIN_NOPULL);
    nrf_gpio_cfg_input(NOR_CS, NRF_GPIO_PIN_NOPULL);
}

/* One 0x03 read, CS held low across it. len <= 256. */
static bool nor_read(uint32_t addr, uint8_t *out, uint32_t len)
{
    uint32_t guard = 0x200000u;

    nor_tx[0] = NOR_CMD_READ;
    nor_tx[1] = (uint8_t)(addr >> 16);
    nor_tx[2] = (uint8_t)(addr >> 8);
    nor_tx[3] = (uint8_t)addr;

    nrf_gpio_pin_clear(NOR_CS);
    __DSB();
    NRF_SPIM0->TXD.PTR = (uint32_t)nor_tx;
    NRF_SPIM0->TXD.MAXCNT = 4u;
    NRF_SPIM0->RXD.PTR = (uint32_t)nor_rx;
    NRF_SPIM0->RXD.MAXCNT = 4u + len;
    NRF_SPIM0->EVENTS_END = 0u;
    __DSB();
    NRF_SPIM0->TASKS_START = 1u;
    while (NRF_SPIM0->EVENTS_END == 0u && guard != 0u) {
        guard--;
    }
    nrf_gpio_pin_set(NOR_CS);
    __DSB();

    /* EVENTS_END is the authoritative completion signal. */
    if (NRF_SPIM0->EVENTS_END == 0u) {
        return false;
    }
    if (out != NULL) {
        memcpy(out, nor_rx + 4, len);
    }
    return true;
}

/* IEEE 802.3 CRC32 (zlib-compatible), streamable: seed 0, chain the return. */
static uint32_t crc32_step(uint32_t crc, const uint8_t *p, uint32_t n)
{
    crc = ~crc;
    for (uint32_t i = 0u; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

static bool ab_read_header(struct ab_header *h)
{
    if (!nor_read(AB_HDR_ADDR, (uint8_t *)h, sizeof(*h))) {
        return false;
    }
    if (h->magic != AB_MAGIC) {
        return false;
    }
    if (h->version != AB_VERSION) {
        return false;
    }
    if (crc32_step(0u, (const uint8_t *)h, 32u) != h->hdr_crc32) {
        return false;
    }
    if (h->flags != AB_FLAG_PROMOTE || h->fail_thresh == 0u) {
        return false;
    }
    /* Validate every target bound before internal flash can be erased. */
    if (h->b_base != AB_APP_BASE) {
        return false;
    }
    if (h->b_len == 0u || h->b_len > AB_APP_MAXLEN) {
        return false;
    }
    if (h->nor_b_off != AB_BIMG_ADDR) {
        return false;
    }
    return true;
}

static bool ab_tally_fails(uint32_t *fails_out)
{
    uint8_t buf[256];
    uint32_t fails = 0u;

    for (uint32_t off = 0u; off < AB_SECTOR; off += sizeof(buf)) {
        if (!nor_read(AB_TALLY_ADDR + off, buf, sizeof(buf))) {
            return false;
        }
        for (uint32_t i = 0u; i < sizeof(buf); i++) {
            if (buf[i] == 0x00u) {
                fails++;
            }
        }
    }
    *fails_out = fails;
    return true;
}

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void ab_scan_coredumps(void)
{
    uint8_t record[COREDUMP_SIZE];
    bool found = false;
    uint16_t best_seq = 0u;

    for (uint32_t slot = 0u; slot < COREDUMP_SLOT_COUNT; slot++) {
        if (!nor_read(COREDUMP_BASE + slot * COREDUMP_SLOT_SIZE,
                      record, sizeof(record))) {
            continue;
        }
        if (get_le32(record) != COREDUMP_MAGIC ||
            get_le16(record + 4u) != COREDUMP_VERSION ||
            crc32_step(0u, record, COREDUMP_SIZE - 4u) !=
                get_le32(record + COREDUMP_SIZE - 4u)) {
            continue;
        }

        uint16_t seq = get_le16(record + 6u);
        if (!found || (uint16_t)(seq - best_seq) < 0x8000u) {
            found = true;
            best_seq = seq;
            boot_status.crash_seq = seq;
            boot_status.crash_reason = get_le32(record + 8u);
            boot_status.crash_pc = get_le32(record + 40u);
            memcpy(newest_crash, record, sizeof(newest_crash));
        }
    }
    boot_status.crash_present = found;
}

void ab_promote_check(void)
{
    struct ab_header h;
    uint8_t buf[256];
    uint32_t crc = 0u;

    memset(&boot_status, 0, sizeof(boot_status));
    boot_status.action = AB_BOOT_UNARMED;
    boot_status.resetreas = NRF_POWER->RESETREAS;

    nor_open();
    ab_scan_coredumps();

    if (!ab_read_header(&h)) {
        nor_close();
        return; /* Descriptor is absent or invalid. */
    }
    boot_status.threshold = h.fail_thresh;
    boot_status.image_len = h.b_len;
    boot_status.tally_readable = ab_tally_fails(&boot_status.failures);
    if (!boot_status.tally_readable) {
        nor_close();
        return;
    }
    if (boot_status.failures < h.fail_thresh) {
        boot_status.action = AB_BOOT_WAITING;
        nor_close();
        return; /* Failure threshold has not been reached. */
    }

    /* Verify the staged image before touching internal flash. */
    for (uint32_t off = 0u; off < h.b_len; ) {
        uint32_t n = (h.b_len - off < 256u) ? (h.b_len - off) : 256u;

        if (!nor_read(AB_BIMG_ADDR + off, buf, n)) {
            nor_close();
            return;
        }
        crc = crc32_step(crc, buf, n);
        off += n;
    }
    if (crc != h.b_crc32) {
        boot_status.action = AB_BOOT_BAD_IMAGE;
        nor_close();
        return; /* Preserve the current application if the staged image is corrupt. */
    }

    /* A failed fallback leaves the tally over threshold. If the application is
     * already the validated B image, do not erase and rewrite it on every boot;
     * continue into the normal DFU/application decision instead. */
    if (crc32_step(0u, (const uint8_t *)AB_APP_BASE, h.b_len) == h.b_crc32) {
        boot_status.action = AB_BOOT_CURRENT;
        nor_close();
        return;
    }

    /* Erase the pages occupied by the validated image, then copy it. */
    {
        uint32_t erase_len = (h.b_len + (AB_SECTOR - 1u)) & ~(AB_SECTOR - 1u);

        flash_nrf5x_erase(AB_APP_BASE, erase_len);
        uint32_t off;

        for (off = 0u; off < h.b_len; ) {
            uint32_t n = (h.b_len - off < 256u) ? (h.b_len - off) : 256u;

            if (!nor_read(AB_BIMG_ADDR + off, buf, n)) {
                break; /* Flush completed writes and retry on the next boot. */
            }
            flash_nrf5x_write(AB_APP_BASE + off, buf, n, false);
            off += n;
        }
        flash_nrf5x_flush(false);
        if (off != h.b_len) {
            boot_status.action = AB_BOOT_COPY_FAILED;
        }
    }

    nor_close();

    /* Clear the stale bank metadata only after verifying the internal copy. */
    if (crc32_step(0u, (const uint8_t *)AB_APP_BASE, h.b_len) == h.b_crc32) {
        boot_status.action = AB_BOOT_RESTORED;
        flash_nrf5x_erase(BOOTLOADER_SETTINGS_ADDRESS, CODE_PAGE_SIZE);
#if defined(APEX_AB_RESTORED_MAGIC)
        /* Permit one erased-settings fallback for this verified restore. */
        NRF_POWER->GPREGRET = APEX_AB_RESTORED_MAGIC;
        __DSB();
#endif
    }
}

static void text_char(char *text, uint32_t capacity, uint32_t *pos, char c)
{
    if (*pos + 1u < capacity) {
        text[(*pos)++] = c;
        text[*pos] = '\0';
    }
}

static void text_str(char *text, uint32_t capacity, uint32_t *pos, const char *s)
{
    while (*s != '\0') {
        text_char(text, capacity, pos, *s++);
    }
}

static void text_dec(char *text, uint32_t capacity, uint32_t *pos, uint32_t value)
{
    char digits[10];
    uint32_t count = 0u;

    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    while (count != 0u) {
        text_char(text, capacity, pos, digits[--count]);
    }
}

static void text_hex32(char *text, uint32_t capacity, uint32_t *pos, uint32_t value)
{
    static const char hex[] = "0123456789abcdef";

    text_str(text, capacity, pos, "0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        text_char(text, capacity, pos, hex[(value >> shift) & 0xFu]);
    }
}

void ab_promote_info_append(char *text, uint32_t capacity)
{
    static const char *const action_names[] = {
        "not armed", "waiting", "staged image invalid",
        "recovery image already running", "restore interrupted", "restored"
    };
    uint32_t pos = (uint32_t)strlen(text);

    text_str(text, capacity, &pos, "Bootloader update: supported\r\nReset: ");
    text_hex32(text, capacity, &pos, boot_status.resetreas);
    text_str(text, capacity, &pos, "\r\nA/B: ");
    text_str(text, capacity, &pos, action_names[boot_status.action]);
    if (boot_status.threshold != 0u) {
        text_str(text, capacity, &pos, ", failed boots ");
        if (boot_status.tally_readable) {
            text_dec(text, capacity, &pos, boot_status.failures);
        } else {
            text_str(text, capacity, &pos, "unknown");
        }
        text_char(text, capacity, &pos, '/');
        text_dec(text, capacity, &pos, boot_status.threshold);
        text_str(text, capacity, &pos, ", image ");
        text_dec(text, capacity, &pos, boot_status.image_len);
        text_str(text, capacity, &pos, " bytes");
    }
    text_str(text, capacity, &pos, "\r\nCrash: ");
    if (!boot_status.crash_present) {
        text_str(text, capacity, &pos, "none\r\n");
        return;
    }
    text_str(text, capacity, &pos, "sequence ");
    text_dec(text, capacity, &pos, boot_status.crash_seq);
    text_str(text, capacity, &pos, ", reason ");
    text_hex32(text, capacity, &pos, boot_status.crash_reason);
    text_str(text, capacity, &pos, ", PC ");
    text_hex32(text, capacity, &pos, boot_status.crash_pc);
    text_str(text, capacity, &pos, "\r\n");
}

uint32_t ab_promote_crash_file(const uint8_t **data)
{
    *data = newest_crash;
    return boot_status.crash_present ? (uint32_t)sizeof(newest_crash) : 0u;
}
