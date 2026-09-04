/* SPDX-License-Identifier: MIT
 *
 * Captures fatal-error context in a fixed external-NOR ring and emits the most
 * recent valid record after reboot. The fault path uses only registers, static
 * RAM, and g4b_spinor_fault_write(); it does not allocate, log, call the kernel,
 * or take the shared external-bus lock. Stack data is copied only when the saved
 * pointer lies within nRF52833 SRAM.
 */

#include <stddef.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/fatal.h>
#include <zephyr/arch/cpu.h>     /* struct arch_esf, _callee_saved_t */

#include <nrfx.h>

#include "coredump_g4b.h"
#include "spinor_g4b.h"
#include "evidence_g4b.h"        /* g4b_evidence_emit_text, G4B_POWER_RESETREAS */

/* The ring occupies 0xFC000-0x100000, above every other NOR allocation. */
BUILD_ASSERT(G4B_COREDUMP_RING_BASE >=
                 G4B_NOR_AB_IMAGE_ADDR + G4B_NOR_AB_IMAGE_SIZE,
             "coredump ring overlaps image B");
BUILD_ASSERT(G4B_COREDUMP_RING_BASE +
             G4B_COREDUMP_RING_SLOTS * G4B_COREDUMP_SLOT_SIZE <=
                 G4B_NOR_TOTAL_SIZE,
             "coredump ring runs past the end of the 1 MiB device");
BUILD_ASSERT(sizeof(struct g4b_coredump_record) <= 256u,
             "coredump record must fit one 256-byte page-program");
BUILD_ASSERT((G4B_COREDUMP_RING_BASE & 0xFFFu) == 0u,
             "coredump ring base must be 4 KiB-sector aligned");

/* SCB fault-status registers. */
#define G4B_SCB_CFSR   (*(volatile uint32_t *)0xE000ED28u)
#define G4B_SCB_HFSR   (*(volatile uint32_t *)0xE000ED2Cu)
#define G4B_SCB_MMFAR  (*(volatile uint32_t *)0xE000ED34u)
#define G4B_SCB_BFAR   (*(volatile uint32_t *)0xE000ED38u)
/* Direct SYSRESETREQ avoids a CONFIG_REBOOT dependency in the fault path. */
#define G4B_SCB_AIRCR  (*(volatile uint32_t *)0xE000ED0Cu)
#define G4B_AIRCR_SYSRESETREQ ((0x5FAu << 16) | (1u << 2))

/* nRF52833: 128 KiB SRAM at 0x20000000. Used only to bound the stack copy. */
#define G4B_RAM_BASE 0x20000000u
#define G4B_RAM_END  0x20020000u

/* Static storage avoids extending a potentially damaged stack. */
static struct g4b_coredump_record g4b_cd_rec;

/* Primed at boot; the defaults remain valid if a fault occurs before SYS_INIT. */
static volatile uint32_t g4b_cd_next_slot;   /* slot to write next */
static volatile uint32_t g4b_cd_next_seq = 1u;

/* Newest valid record found at boot, kept for g4b_coredump_emit_last(). */
static struct g4b_coredump_record g4b_cd_last;
static bool g4b_cd_have_last;

/* CRC-32 used by the existing evidence decoder. */
static uint32_t g4b_cd_crc32(const uint8_t *data, uint32_t len, uint32_t crc)
{
    for (uint32_t i = 0u; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return crc;
}

/* Overrides the weak kernel handler and runs entirely in exception context. */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
    struct g4b_coredump_record *r = &g4b_cd_rec;
    uint32_t slot;
    uint32_t sp = 0u;

    memset(r, 0, sizeof(*r));
    r->magic = G4B_COREDUMP_MAGIC;
    r->version = (uint16_t)G4B_COREDUMP_VERSION;
    r->seq = (uint16_t)g4b_cd_next_seq;
    r->reason = reason;
    r->resetreas = *(volatile uint32_t *)G4B_POWER_RESETREAS;

    if (esf != NULL) {
        r->r0   = esf->basic.r0;
        r->r1   = esf->basic.r1;
        r->r2   = esf->basic.r2;
        r->r3   = esf->basic.r3;
        r->r12  = esf->basic.r12;
        r->lr   = esf->basic.lr;
        r->pc   = esf->basic.pc;
        r->xpsr = esf->basic.xpsr;
        sp = (uint32_t)esf;   /* the basic frame sits at the pre-fault SP */

#if defined(CONFIG_EXTRA_EXCEPTION_INFO)
        r->msp        = esf->extra_info.msp;
        r->exc_return = esf->extra_info.exc_return;
        if (esf->extra_info.callee != NULL) {
            const _callee_saved_t *c = esf->extra_info.callee;

            r->r4  = c->v1;  /* r4  */
            r->r5  = c->v2;  /* r5  */
            r->r6  = c->v3;  /* r6  */
            r->r7  = c->v4;  /* r7  */
            r->r8  = c->v5;  /* r8  */
            r->r9  = c->v6;  /* r9  */
            r->r10 = c->v7;  /* r10 */
            r->r11 = c->v8;  /* r11 */
            r->psp = c->psp;
        }
#endif
    }
    r->sp = sp;

    /* BFAR and MMFAR are interpreted only when CFSR marks them valid. */
    r->cfsr  = G4B_SCB_CFSR;
    r->hfsr  = G4B_SCB_HFSR;
    r->bfar  = G4B_SCB_BFAR;
    r->mmfar = G4B_SCB_MMFAR;

    /* Reject an invalid stack pointer instead of risking another fault. */
    if (sp >= G4B_RAM_BASE &&
        sp <= G4B_RAM_END - sizeof(r->stack)) {
        memcpy(r->stack, (const void *)sp, sizeof(r->stack));
        r->stack_valid = 1u;
    } else {
        r->stack_valid = 0u;
    }

    r->crc32 = ~g4b_cd_crc32((const uint8_t *)r,
                             offsetof(struct g4b_coredump_record, crc32),
                             0xFFFFFFFFu);

    /* A failed best-effort write must not prevent the reset. */
    slot = g4b_cd_next_slot % G4B_COREDUMP_RING_SLOTS;
    (void)g4b_spinor_fault_write(G4B_COREDUMP_RING_BASE + slot * G4B_COREDUMP_SLOT_SIZE,
                                 (const uint8_t *)r, (uint32_t)sizeof(*r));

    /* Request a system reset, then wait for it to complete. */
    __DSB();
    G4B_SCB_AIRCR = G4B_AIRCR_SYSRESETREQ;
    __DSB();
    for (;;) {
        /* wait for the reset */
    }
}

/* Scan the ring before main() to prime the cursor and retain the newest record. */
static int g4b_coredump_scan(void)
{
    uint32_t best_seq = 0u;
    uint32_t best_slot = 0u;
    bool any = false;

    for (uint32_t s = 0u; s < G4B_COREDUMP_RING_SLOTS; s++) {
        struct g4b_coredump_record tmp;

        if (!g4b_spinor_coredump_read(
                G4B_COREDUMP_RING_BASE + s * G4B_COREDUMP_SLOT_SIZE,
                (uint8_t *)&tmp, (uint32_t)sizeof(tmp))) {
            continue;
        }
        if (tmp.magic != G4B_COREDUMP_MAGIC) {
            continue;   /* blank (0xFF) or foreign bytes */
        }
        if (tmp.crc32 != ~g4b_cd_crc32((const uint8_t *)&tmp,
                                       offsetof(struct g4b_coredump_record, crc32),
                                       0xFFFFFFFFu)) {
            continue;   /* torn/partial write */
        }
        /* Compare at uint16_t width so sequence wrap preserves ordering. */
        if (!any || (uint16_t)(tmp.seq - best_seq) < 0x8000u) {
            best_seq = tmp.seq;
            best_slot = s;
            g4b_cd_last = tmp;
            g4b_cd_have_last = true;
        }
        any = true;
    }

    if (any) {
        g4b_cd_next_seq = (uint32_t)(uint16_t)(best_seq + 1u);
        g4b_cd_next_slot = (best_slot + 1u) % G4B_COREDUMP_RING_SLOTS;
    } else {
        g4b_cd_next_seq = 1u;
        g4b_cd_next_slot = 0u;
    }
    return 0;
}

SYS_INIT(g4b_coredump_scan, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* Emit the newest record as ASCII hex over the configured evidence channels. */
#if IS_ENABLED(CONFIG_APEX_G4B_UART_EVIDENCE) && \
    (IS_ENABLED(CONFIG_APEX_G4B_UART_EMIT) || \
     IS_ENABLED(CONFIG_APEX_G4B_EVIDENCE_USB))
static uint32_t cd_puthex(uint8_t *line, uint32_t n, uint32_t v)
{
    static const char hexd[] = "0123456789abcdef";

    for (int shift = 28; shift >= 0; shift -= 4) {
        line[n++] = (uint8_t)hexd[(v >> shift) & 0xFu];
    }
    return n;
}

static uint32_t cd_puttag(uint8_t *line, uint32_t n, const char *tag)
{
    while (*tag != '\0') {
        line[n++] = (uint8_t)*tag++;
    }
    return n;
}

void g4b_coredump_emit_last(void)
{
    const struct g4b_coredump_record *r = &g4b_cd_last;
    uint8_t line[96];
    uint32_t n;

    if (!g4b_cd_have_last) {
        return;
    }

    /* Header: seq, fault reason, resetreas, PC, LR, xPSR. */
    n = cd_puttag(line, 0u, "APXCD seq=");
    n = cd_puthex(line, n, r->seq);
    n = cd_puttag(line, n, " rsn=");
    n = cd_puthex(line, n, r->reason);
    n = cd_puttag(line, n, " rr=");
    n = cd_puthex(line, n, r->resetreas);
    n = cd_puttag(line, n, " pc=");
    n = cd_puthex(line, n, r->pc);
    n = cd_puttag(line, n, " lr=");
    n = cd_puthex(line, n, r->lr);
    n = cd_puttag(line, n, " psr=");
    n = cd_puthex(line, n, r->xpsr);
    line[n++] = 13; line[n++] = 10;
    g4b_evidence_emit_text(line, n);

    /* Fault status registers. */
    n = cd_puttag(line, 0u, "APXCD cfsr=");
    n = cd_puthex(line, n, r->cfsr);
    n = cd_puttag(line, n, " hfsr=");
    n = cd_puthex(line, n, r->hfsr);
    n = cd_puttag(line, n, " bfar=");
    n = cd_puthex(line, n, r->bfar);
    n = cd_puttag(line, n, " mmfar=");
    n = cd_puthex(line, n, r->mmfar);
    line[n++] = 13; line[n++] = 10;
    g4b_evidence_emit_text(line, n);

    /* R0-R3, R12. */
    n = cd_puttag(line, 0u, "APXCD r0=");
    n = cd_puthex(line, n, r->r0);
    n = cd_puttag(line, n, " r1=");
    n = cd_puthex(line, n, r->r1);
    n = cd_puttag(line, n, " r2=");
    n = cd_puthex(line, n, r->r2);
    n = cd_puttag(line, n, " r3=");
    n = cd_puthex(line, n, r->r3);
    n = cd_puttag(line, n, " r12=");
    n = cd_puthex(line, n, r->r12);
    line[n++] = 13; line[n++] = 10;
    g4b_evidence_emit_text(line, n);

    /* R4-R11 (zero unless CONFIG_EXTRA_EXCEPTION_INFO captured them). */
    n = cd_puttag(line, 0u, "APXCD r4=");
    n = cd_puthex(line, n, r->r4);
    n = cd_puttag(line, n, " r5=");
    n = cd_puthex(line, n, r->r5);
    n = cd_puttag(line, n, " r6=");
    n = cd_puthex(line, n, r->r6);
    n = cd_puttag(line, n, " r7=");
    n = cd_puthex(line, n, r->r7);
    line[n++] = 13; line[n++] = 10;
    g4b_evidence_emit_text(line, n);

    n = cd_puttag(line, 0u, "APXCD r8=");
    n = cd_puthex(line, n, r->r8);
    n = cd_puttag(line, n, " r9=");
    n = cd_puthex(line, n, r->r9);
    n = cd_puttag(line, n, " r10=");
    n = cd_puthex(line, n, r->r10);
    n = cd_puttag(line, n, " r11=");
    n = cd_puthex(line, n, r->r11);
    line[n++] = 13; line[n++] = 10;
    g4b_evidence_emit_text(line, n);

    /* Stack pointers + the first eight words of the captured slice. */
    n = cd_puttag(line, 0u, "APXCD sp=");
    n = cd_puthex(line, n, r->sp);
    n = cd_puttag(line, n, " msp=");
    n = cd_puthex(line, n, r->msp);
    n = cd_puttag(line, n, " psp=");
    n = cd_puthex(line, n, r->psp);
    n = cd_puttag(line, n, " excret=");
    n = cd_puthex(line, n, r->exc_return);
    n = cd_puttag(line, n, " sv=");
    n = cd_puthex(line, n, r->stack_valid);
    line[n++] = 13; line[n++] = 10;
    g4b_evidence_emit_text(line, n);

    for (uint32_t w = 0u; w < G4B_COREDUMP_STACK_WORDS; w += 4u) {
        n = cd_puttag(line, 0u, "APXCD stk");
        n = cd_puthex(line, n, w);
        line[n++] = '=';
        for (uint32_t k = 0u; k < 4u; k++) {
            n = cd_puthex(line, n, r->stack[w + k]);
            line[n++] = ' ';
        }
        line[n++] = 13; line[n++] = 10;
        g4b_evidence_emit_text(line, n);
    }
}

/* Delay emission long enough for the USB serial endpoint to enumerate. */
static void g4b_coredump_emit_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    k_msleep(4000);
    g4b_coredump_emit_last();
}

K_THREAD_DEFINE(g4b_coredump_emit_tid, 1024, g4b_coredump_emit_thread,
                NULL, NULL, NULL, K_PRIO_PREEMPT(14), 0, 0);
#else
void g4b_coredump_emit_last(void)
{
}
#endif /* evidence output enabled */
