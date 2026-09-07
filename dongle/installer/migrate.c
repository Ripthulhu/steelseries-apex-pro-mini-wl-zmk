/* Runs from unused space in the stock application at 0x26000. */
#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(a))
#define NVMC 0x4001e000u
#define PAGE 4096u

extern const uint32_t boot_image[];
extern const uint32_t mbr_image[];
#define DIAG ((volatile uint32_t *)0x2001f800)

#ifdef DONGLE_READBACK
#define DUMP_CURSOR (*(volatile uint32_t *)0x2001f840)
/* Called by the stock USB version command after normal startup. */
void readback_report(uint8_t *out)
{
    uint32_t offset = DUMP_CURSOR;
#ifdef DONGLE_ACL_READBACK
    if (offset >= 128u) offset = 0;
    uint32_t address = 0x4001e800u + offset;
    uint32_t limit = 128u;
    uint32_t source = 0x2001f880u + offset;
#else
    if (offset >= 0x81000u) offset = 0;
    uint32_t address = offset < 0x80000u ? offset : 0x10001000u + offset - 0x80000u;
    uint32_t limit = offset < 0x80000u ? 0x80000u : 0x81000u;
    uint32_t source = address;
#endif
    uint32_t size = limit - offset;
    if (size > 56) size = 56;
    out[0] = 'D'; out[1] = 'M'; out[2] = 'P'; out[3] = '1';
    for (unsigned i = 0; i < 4; ++i) out[4 + i] = address >> (8 * i);
    for (unsigned i = 0; i < 56; ++i)
        out[8 + i] = i < size ? *(volatile uint8_t *)(source + i) : 0xff;
    DUMP_CURSOR = offset + size;
}
#endif

static void feed(void)
{
    if (REG(0x40010400)) {
        for (unsigned i = 0; i < 8; ++i)
            REG(0x40010600 + i * 4) = 0x6e524635;
    }
}

static void ready(void)
{
    while (!REG(NVMC + 0x400))
        feed();
}

static void mode(uint32_t value)
{
    ready();
    REG(NVMC + 0x504) = value;
    ready();
}

static int write_page(uint32_t address, const uint32_t *source)
{
    feed();
    mode(2);
    REG(NVMC + 0x508) = address;
    ready();
    mode(1);
    for (unsigned i = 0; i < PAGE / 4; ++i) {
        if (source[i] != 0xffffffffu) {
            REG(address + 4 * i) = source[i];
            ready();
        }
        feed();
    }
    mode(0);
    for (unsigned i = 0; i < PAGE / 4; ++i)
        if (REG(address + 4 * i) != source[i]) {
            DIAG[2] = address + 4 * i;
            DIAG[3] = source[i];
            DIAG[4] = REG(address + 4 * i);
            return 0;
        }
    return 1;
}

__attribute__((noreturn)) static void reset(void)
{
    REG(0x4000051c) = 0x57;
    __asm volatile("dsb" ::: "memory");
    REG(0xe000ed0c) = 0x05fa0004;
    for (;;) {}
}

/* A pre-commit failure returns to stock startup, keeping its USB updater. */
__attribute__((noreturn)) static void stock_start(void)
{
    mode(0);
    __asm volatile("ldr r0, =0x2000d000\n"
                   "msr msp, r0\n"
                   "ldr r0, =0x23ad1\n"
                   "cpsie i\n"
                   "bx r0" ::: "r0", "memory");
    __builtin_unreachable();
}

__attribute__((noreturn)) void migrate(void)
{
    REG(0xe000ed08) = 0x23000;
    for (unsigned i = 0; i < 16; ++i) DIAG[i] = 0;
    DIAG[0] = 0x3147494d;
    DIAG[1] = 1;
    DIAG[5] = REG(0x10000010);
    DIAG[6] = REG(0x10000014);
    DIAG[7] = REG(0x10000100);
    DIAG[8] = REG(0x10000104);
    DIAG[9] = REG(0x10000108);
    DIAG[10] = REG(0x1000010c);
    DIAG[11] = REG(0x10000110);
    DIAG[12] = REG(0x10001014);
    DIAG[13] = REG(0x10001018);
    DIAG[14] = REG(0x10001208);
    DIAG[15] = REG(0x10001304);
#ifdef DONGLE_READBACK
    DUMP_CURSOR = 0;
#endif
#ifdef DONGLE_ACL_READBACK
    for (unsigned i = 0; i < 32; ++i) REG(0x2001f880 + 4*i) = REG(0x4001e800 + 4*i);
#endif
#ifdef DONGLE_INSPECT_ONLY
    stock_start();
#endif
    if (REG(0x10000010) != PAGE || REG(0x10000014) != 128 ||
        REG(0x10000100) != 0x52833)
        stock_start();

    /* These addresses are already configured on the captured receiver.
     * Do not erase UICR or change its voltage, reset pins or protection. */
    if (REG(0x10001014) != 0x74000 || REG(0x10001018) != 0x7e000) {
        DIAG[1] = 9;
        stock_start();
    }

    /* The old loader's code at 0x1000 is not an application vector table.
     * Leave it intact; the new loader will stay in USB DFU with no valid app. */
    uint32_t old_sp = REG(0x1000);
    if (old_sp >= 0x20000000 && old_sp <= 0x20020000) {
        DIAG[1] = 10;
        stock_start();
    }

    DIAG[1] = 4;
    for (uint32_t offset = 0; offset < 0xa000; offset += PAGE)
        if (!write_page(0x74000 + offset, boot_image + offset / 4))
            stock_start();

    DIAG[1] = 5;
    mode(2);
    REG(NVMC + 0x508) = 0x7e000;
    ready();
    REG(NVMC + 0x508) = 0x7f000;
    ready();
    mode(0);
    for (uint32_t address = 0x7e000; address < 0x80000; address += 4)
        if (REG(address) != 0xffffffffu) {
            DIAG[2] = address;
            DIAG[3] = 0xffffffffu;
            DIAG[4] = REG(address);
            stock_start();
        }

    /* Commit the reset path last. The verified factory dump is on the PC;
     * 0x50000..0x7d000 belongs to the stock updater, not a backup partition. */
    DIAG[1] = 8;
    while (!write_page(0, mbr_image))
        feed();
    reset();
}

__attribute__((naked, section(".entry"), noreturn)) void entry(void)
{
    __asm volatile("cpsid i\n"
                   "ldr r0, =0x20020000\n"
                   "msr msp, r0\n"
                   "b migrate");
}

