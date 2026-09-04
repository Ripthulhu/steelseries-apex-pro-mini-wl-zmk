/* SPDX-License-Identifier: MIT
 *
 * A/B auto-rollback - APP-SIDE half. Maintains a boot-health tally on the
 * external SPI-NOR and stages a last-known-good image B in the NOR tail, so a
 * bootloader-side promote (ab_promote.c, SWD-flashed separately) can restore B
 * after N unhealthy boots.
 *
 * Bus discipline: every access is a g4b_spinor_dev_* call. Those already take
 * the bus-wide g4b_extbus lock (shared with the SPIM3 scanner exchange), wait
 * out the boot replay on writes/erases, and open/close SPIM0 per op - see the
 * long note at the top of the SPINOR_FLASHDEV block in spinor_g4b.c. So this
 * file adds NO new register or pin access, and needs no lock of its own.
 *
 * NOR tail layout (absolute offsets in the 1 MiB FM25Q08A). All of it is clear
 * of the ZMK NVS partition (0x60000-0x68000), the provision marker (0x68000),
 * the LittleFS partition (0x6B000-0x80000), the diagnostic scratch sectors
 * (0x80000, 0x81000) and the coredump ring (0xFC000), as defined by
 * nor_layout_g4b.h:
 *
 *   0x69000  0x1000    A/B descriptor sector (struct ab_header, committed last)
 *   0x6A000  0x1000    boot tally sector (append-only 0x00 = unhealthy boots)
 *   0x8B000  0x71000   raw image B (exactly the app-slot capacity; ends 0xFC000)
 */

#include <stddef.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/crc.h>

#include "spinor_g4b.h"
#include "ab_rollback_g4b.h"
#include "nor_layout_g4b.h"
#include "pins_g4b.h"          /* g4b_attn_isr_fires() for the APXISR line */

#if IS_ENABLED(CONFIG_APEX_G4B_UART_EVIDENCE)
#include "evidence_g4b.h"
#endif

#define AB_HDR_ADDR    G4B_NOR_AB_HEADER_ADDR
#define AB_TALLY_ADDR  G4B_NOR_AB_TALLY_ADDR
#define AB_SECTOR      G4B_NOR_SECTOR_SIZE
#define AB_BIMG_ADDR   G4B_NOR_AB_IMAGE_ADDR

#define AB_APP_BASE    G4B_INTERNAL_APP_ADDR
#define AB_APP_MAXLEN  G4B_INTERNAL_APP_SIZE

#define AB_MAGIC       0x41423447u /* "G4BA" little-endian */
#define AB_VERSION     2u          /* v2: full-size B at 0x8B000 */
#define AB_FLAG_PROMOTE BIT(0)
#define AB_DEFAULT_THRESH 3u       /* promote after N consecutive unhealthy boots */

/* Wire-format descriptor. Read byte-for-byte by the bootloader (ab_promote.c),
 * so its layout is frozen: nine LE uint32, hdr_crc32 covers the first eight. */
struct ab_header {
    uint32_t magic;
    uint32_t version;
    uint32_t b_base;      /* expected internal application address */
    uint32_t b_len;       /* limited to the internal application slot */
    uint32_t b_crc32;     /* IEEE CRC32 over b_len bytes of B in NOR */
    uint32_t nor_b_off;   /* where B lives in NOR (AB_BIMG_ADDR) */
    uint32_t fail_thresh; /* promote when the tally reaches this */
    uint32_t flags;       /* AB_FLAG_PROMOTE once B is known-good */
    uint32_t hdr_crc32;   /* IEEE CRC32 over the 32 bytes above */
};
BUILD_ASSERT(sizeof(struct ab_header) == 36u, "ab_header wire layout changed");
BUILD_ASSERT(G4B_NOR_AB_IMAGE_SIZE == G4B_INTERNAL_APP_SIZE,
             "image B must mirror the whole internal app slot");
BUILD_ASSERT(G4B_NOR_AB_IMAGE_ADDR + G4B_NOR_AB_IMAGE_SIZE ==
                 G4B_NOR_COREDUMP_ADDR,
             "image B must end exactly where the coredump ring begins");
BUILD_ASSERT(G4B_NOR_COREDUMP_ADDR + G4B_NOR_COREDUMP_SIZE ==
                 G4B_NOR_TOTAL_SIZE,
             "coredump ring must end at the end of NOR");

/* --- boot tally ---------------------------------------------------------- */

void g4b_ab_boot_pending(void)
{
    uint8_t buf[256];
    uint32_t idx = AB_SECTOR; /* sentinel: no free byte found */

    for (uint32_t off = 0u; off < AB_SECTOR && idx == AB_SECTOR; off += sizeof(buf)) {
        if (g4b_spinor_dev_read(AB_TALLY_ADDR + off, buf, sizeof(buf)) != 0) {
            return; /* best-effort: a read error just skips this boot's mark */
        }
        for (uint32_t i = 0u; i < sizeof(buf); i++) {
            if (buf[i] == 0xFFu) {
                idx = off + i;
                break;
            }
        }
    }
    if (idx == AB_SECTOR) {
        return; /* sector full: 4096 unhealthy boots, promote is already due */
    }
    {
        uint8_t zero = 0x00u;
        (void)g4b_spinor_dev_program(AB_TALLY_ADDR + idx, &zero, 1u);
    }
}

#if IS_ENABLED(CONFIG_APEX_G4B_AB_AUTOSTAGE)
static K_SEM_DEFINE(ab_autostage_sem, 0, 1);
#endif

void g4b_ab_mark_healthy(void)
{
    static bool done;

    if (done) {
        return;
    }
    /* One 4 KiB erase returns the tally to "zero unhealthy boots". This is the
     * only self-managed NOR op on the g4b thread; g4b_spinor_dev_erase releases
     * the bus per-sector, so a pending SPIM3 exchange still runs in the gap. */
    if (g4b_spinor_dev_erase(AB_TALLY_ADDR, AB_SECTOR) != 0) {
        return;
    }
    done = true;

#if IS_ENABLED(CONFIG_APEX_G4B_AB_AUTOSTAGE)
    /* Kick the auto-stage thread (once) so B tracks whatever good firmware just
     * booted healthy. Off the g4b thread: staging is a ~20 s NOR erase+copy and
     * must not run here. */
    k_sem_give(&ab_autostage_sem);
#endif
}

/* --- host-driven staging of image B ------------------------------------- */

bool g4b_ab_stage_erase(void)
{
    for (uint32_t a = 0u; a < AB_APP_MAXLEN; a += AB_SECTOR) {
        if (g4b_spinor_dev_erase(AB_BIMG_ADDR + a, AB_SECTOR) != 0) {
            return false;
        }
    }
    return true;
}

bool g4b_ab_stage_write(uint32_t off, const uint8_t *data, uint32_t len)
{
    if ((uint64_t)off + len > AB_APP_MAXLEN) {
        return false; /* refuse to write past the B slot */
    }
    return g4b_spinor_dev_program(AB_BIMG_ADDR + off, data, len) == 0;
}

bool g4b_ab_commit(uint32_t b_len)
{
    struct ab_header h;
    uint8_t buf[256];
    uint32_t crc = 0u;

    if (b_len == 0u || b_len > AB_APP_MAXLEN) {
        return false; /* fence: never arm a descriptor that overruns the slot */
    }

    /* CRC32 the freshly written B by reading it back from NOR (proves the write
     * landed, not just what the host sent). Streamed in 256 B blocks. */
    for (uint32_t off = 0u; off < b_len; ) {
        uint32_t n = MIN((uint32_t)sizeof(buf), b_len - off);

        if (g4b_spinor_dev_read(AB_BIMG_ADDR + off, buf, n) != 0) {
            return false;
        }
        crc = crc32_ieee_update(crc, buf, n);
        off += n;
    }

    h.magic = AB_MAGIC;
    h.version = AB_VERSION;
    h.b_base = AB_APP_BASE;
    h.b_len = b_len;
    h.b_crc32 = crc;
    h.nor_b_off = AB_BIMG_ADDR;
    h.fail_thresh = AB_DEFAULT_THRESH;
    h.flags = AB_FLAG_PROMOTE;
    h.hdr_crc32 = crc32_ieee((const uint8_t *)&h,
                             offsetof(struct ab_header, hdr_crc32));

    /* Clear the tally as part of arming: a freshly staged B
     * must start from a known-zero failure count, so a stale non-0xFF tally
     * can't trigger a spurious promote the moment the descriptor goes valid.
     * Same extbus-locked per-op discipline, so it stays scanner-safe. */
    if (g4b_spinor_dev_erase(AB_TALLY_ADDR, AB_SECTOR) != 0) {
        return false;
    }

    /* Descriptor written LAST, and only after B is fully in NOR and the tally is
     * clean - the fence that keeps a half-staged B from ever looking promotable. */
    if (g4b_spinor_dev_erase(AB_HDR_ADDR, AB_SECTOR) != 0) {
        return false;
    }
    return g4b_spinor_dev_program(AB_HDR_ADDR, &h, sizeof(h)) == 0;
}

bool g4b_ab_disarm(void)
{
    return g4b_spinor_dev_erase(AB_HDR_ADDR, AB_SECTOR) == 0;
}

/* --- auto-staging: keep B tracking the current good firmware ---------------- */
#if IS_ENABLED(CONFIG_APEX_G4B_AB_AUTOSTAGE)
#define AB_APP_ADDR G4B_INTERNAL_APP_ADDR

/* Exact image length from the linker (bytes of text+rodata+data-init at
 * 0x1000). Using this, not a scan of the slot tail: a UF2 flash and the promote
 * only erase the pages they write, so stale bytes from a previous firmware can
 * sit past the image and defeat a 0xFF tail-scan. _flash_used is content-
 * independent and exactly covers the running image. */
extern char _flash_used[];

static uint32_t ab_app_used_len(void)
{
    uint32_t len = (uint32_t)_flash_used;

    if (len == 0u || len > AB_APP_MAXLEN) {
        return 0u; /* implausible - skip autostage rather than stage garbage */
    }
    return len;
}

/* True if B already holds exactly this app (same len + CRC) - then no re-stage.
 * The app CRC (crc32_ieee over len bytes) is computed the same way g4b_ab_commit
 * derives B's b_crc32 (chained crc32_ieee_update from 0), so they match when B
 * is a copy of the running app. */
static bool ab_app_already_b(uint32_t len)
{
    struct ab_header h;
    uint32_t crc = crc32_ieee((const uint8_t *)AB_APP_ADDR, len);

    if (g4b_spinor_dev_read(AB_HDR_ADDR, &h, sizeof(h)) != 0) {
        return false;
    }
    if (h.magic != AB_MAGIC || h.version != AB_VERSION ||
        h.hdr_crc32 != crc32_ieee((const uint8_t *)&h,
                                  offsetof(struct ab_header, hdr_crc32))) {
        return false;
    }
    return h.b_base == AB_APP_BASE && h.b_len == len && h.b_crc32 == crc &&
           h.nor_b_off == AB_BIMG_ADDR && h.flags == AB_FLAG_PROMOTE;
}

/* Copy the running app into B so the rollback target tracks the firmware that
 * just booted healthy. Runs on its own low-priority thread, once per boot after
 * HEALTHY, and ONLY when the app differs from B - so a normal boot does nothing
 * and a freshly-flashed firmware self-stages exactly once. Every NOR op is
 * per-op extbus-locked, so the SPIM3 scanner interleaves and the keyboard keeps
 * working through the ~20 s erase+copy. */
static void ab_autostage_thread(void *a, void *b, void *c)
{
    uint32_t len;

    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    k_sem_take(&ab_autostage_sem, K_FOREVER);

    len = ab_app_used_len();
    if (len == 0u || len > AB_APP_MAXLEN) {
        return;
    }
    if (ab_app_already_b(len)) {
        return; /* B already == this firmware; nothing to do */
    }

    if (!g4b_ab_stage_erase()) {
        return;
    }
    for (uint32_t off = 0u; off < len; ) {
        uint32_t n = MIN(256u, len - off);

        if (!g4b_ab_stage_write(off, (const uint8_t *)(AB_APP_ADDR + off), n)) {
            return; /* leave B erased + un-committed -> reads as not-armed */
        }
        off += n;
        k_yield(); /* let the scanner run freely between chunks */
    }
    (void)g4b_ab_commit(len);
}

K_THREAD_DEFINE(g4b_ab_autostage_tid, 2048, ab_autostage_thread, NULL, NULL, NULL,
                K_PRIO_PREEMPT(13), 0, 0);
#endif /* CONFIG_APEX_G4B_AB_AUTOSTAGE */

/* Mark THIS boot pending as early as possible - before the g4b thread starts
 * (it is delayed CONFIG_APEX_G4B_THREAD_DELAY_MS), so a hang anywhere in ZMK's
 * own init is still counted. At APPLICATION SYS_INIT time the g4b thread is not
 * running, so the shared bus lock and replay gate inside g4b_spinor_dev_* are
 * uncontended - exactly the conditions fs_provision() already relies on. The
 * tally sector (0x6A000) is independent of the NVS partition and its provision
 * marker, so ordering against fs_provision does not matter. */
static int g4b_ab_boot_pending_init(void)
{
    g4b_ab_boot_pending();
#if IS_ENABLED(CONFIG_APEX_G4B_AB_CRASHTEST)
    /* TEST-ONLY: fault immediately after marking this boot pending, so the tally
     * accumulates and (after fail_thresh boots) the bootloader promote fires and
     * restores image B. Never enable in a shipping build. The coredump handler
     * captures it and resets, so each boot cleanly adds one unhealthy mark. */
    *(volatile uint32_t *)0xFFFFFFF0u = 0xDEADBEEFu;
#endif
    return 0;
}

SYS_INIT(g4b_ab_boot_pending_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* --- observability: emit the tally + descriptor state over evidence ------- */
#if IS_ENABLED(CONFIG_APEX_G4B_UART_EVIDENCE) && \
    (IS_ENABLED(CONFIG_APEX_G4B_UART_EMIT) || \
     IS_ENABLED(CONFIG_APEX_G4B_EVIDENCE_USB))
static uint32_t ab_puthex(uint8_t *line, uint32_t n, uint32_t v)
{
    static const char hexd[] = "0123456789abcdef";

    for (int shift = 28; shift >= 0; shift -= 4) {
        line[n++] = (uint8_t)hexd[(v >> shift) & 0xFu];
    }
    return n;
}

static uint32_t ab_puttag(uint8_t *line, uint32_t n, const char *tag)
{
    while (*tag != '\0') {
        line[n++] = (uint8_t)*tag++;
    }
    return n;
}

static void g4b_ab_status_thread(void *a, void *b, void *c)
{
    uint8_t buf[256];
    struct ab_header h;
    uint32_t fails = 0u;
    bool armed = false;
    /* The full status line is a fixed 71 bytes ("APXAB fails=" + 8 + " armed=" +
     * 1 + " blen=" + 8 + " bcrc=" + 8 + " thr=" + 8 + CRLF). 80 gives headroom;
     * the appends below are unbounded, so this MUST cover the worst case. */
    uint8_t line[80];
    uint32_t n = 0u;

    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    k_msleep(4500); /* after the coredump/lfs reporters, USB CDC up */

    /* Count 0x00 bytes across the tally sector = unhealthy boots since the last
     * HEALTHY erase. */
    for (uint32_t off = 0u; off < AB_SECTOR; off += sizeof(buf)) {
        if (g4b_spinor_dev_read(AB_TALLY_ADDR + off, buf, sizeof(buf)) != 0) {
            break;
        }
        for (uint32_t i = 0u; i < sizeof(buf); i++) {
            if (buf[i] == 0x00u) {
                fails++;
            }
        }
    }

    /* Descriptor: armed iff magic + hdr CRC + PROMOTE flag all hold. */
    if (g4b_spinor_dev_read(AB_HDR_ADDR, &h, sizeof(h)) == 0 &&
        h.magic == AB_MAGIC && h.version == AB_VERSION &&
        h.hdr_crc32 == crc32_ieee((const uint8_t *)&h,
                                  offsetof(struct ab_header, hdr_crc32)) &&
        (h.flags & AB_FLAG_PROMOTE) != 0u) {
        armed = true;
    } else {
        memset(&h, 0, sizeof(h));
    }

    n = ab_puttag(line, 0u, "APXAB fails=");
    n = ab_puthex(line, n, fails);
    n = ab_puttag(line, n, " armed=");
    line[n++] = armed ? '1' : '0';
    n = ab_puttag(line, n, " blen=");
    n = ab_puthex(line, n, h.b_len);
    n = ab_puttag(line, n, " bcrc=");
    n = ab_puthex(line, n, h.b_crc32);
    n = ab_puttag(line, n, " thr=");
    n = ab_puthex(line, n, armed ? h.fail_thresh : 0u);
    line[n++] = '\r';
    line[n++] = '\n';
    g4b_evidence_emit_text(line, n);

    /* Re-emit the ATTN interrupt count every few seconds. A rising count while
     * typing shows that queued key reports wake the Nordic scan thread instead
     * of the semaphore timeout fallback. */
    for (;;) {
        k_msleep(3000);
        n = ab_puttag(line, 0u, "APXISR fires=");
        n = ab_puthex(line, n, g4b_attn_isr_fires());
        line[n++] = '\r';
        line[n++] = '\n';
        g4b_evidence_emit_text(line, n);
    }
}

K_THREAD_DEFINE(g4b_ab_status_tid, 1024, g4b_ab_status_thread, NULL, NULL, NULL,
                K_PRIO_PREEMPT(14), 0, 0);
#endif /* evidence output enabled */
