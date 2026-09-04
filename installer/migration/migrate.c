/* SPDX-License-Identifier: MIT */

#include <stdint.h>

#define EXPECTED_BOOTLOADER_ADDR  0x0006e000u
#define EXPECTED_PARAM_PAGE_ADDR  0x0006b000u

#define METADATA_ADDR             0x0005b000u
#define BOOTLOADER_SOURCE_ADDR    0x0005c000u
#define BOOTLOADER_LENGTH         0x0000a000u
#define BOOTLOADER_END            0x00078000u

#define POWER_GPREGRET            (*(volatile uint32_t *)0x4000051cu)
#define NVMC_READY                (*(volatile uint32_t *)0x4001e400u)
#define NVMC_CONFIG               (*(volatile uint32_t *)0x4001e504u)
#define NVMC_ERASEPAGE            (*(volatile uint32_t *)0x4001e508u)
#define SCB_AIRCR                  (*(volatile uint32_t *)0xe000ed0cu)
#define RETAINED_STATUS           (*(volatile uint32_t *)0x20002ff0u)

#define MIGRATION_MAGIC0          0x4d585041u /* "APXM" */
#define MIGRATION_MAGIC1          0x00314749u /* "IG1\0" */
#define GPREGRET_UF2_DFU          0x57u

struct migration_metadata {
    uint32_t magic0;
    uint32_t magic1;
    uint32_t version;
    uint32_t source;
    uint32_t destination;
    uint32_t length;
    uint32_t crc32;
    uint32_t reserved;
};

struct mbr_command {
    uint32_t command;
    uint32_t source;
    uint32_t length_words;
    uint32_t unused;
};

static uint32_t crc32(const uint8_t *data, uint32_t length)
{
    uint32_t crc = 0xffffffffu;
    for (uint32_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint32_t bit = 0; bit < 8; ++bit) {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return crc ^ 0xffffffffu;
}

extern uint32_t call_mbr(struct mbr_command *command);

static uint32_t read_flash_word(uint32_t address)
{
    uint32_t value;
    __asm volatile("ldr %0, [%1]" : "=r"(value) : "r"(address));
    return value;
}

__attribute__((noreturn)) void migration_recover(uint32_t status)
{
    RETAINED_STATUS = 0x4d470000u | status;
    __asm volatile("cpsid i" ::: "memory");

    NVMC_CONFIG = 2u;
    while (NVMC_READY == 0u) {}
    NVMC_ERASEPAGE = 0x0001c000u;
    while (NVMC_READY == 0u) {}
    NVMC_CONFIG = 0u;
    while (NVMC_READY == 0u) {}

    __asm volatile("dsb\n isb" ::: "memory");
    SCB_AIRCR = 0x05fa0004u;
    __asm volatile("dsb\n isb" ::: "memory");
    for (;;) {}
}

void migration_main(void)
{
    const struct migration_metadata *meta =
        (const struct migration_metadata *)METADATA_ADDR;
    const uint32_t *vectors = (const uint32_t *)BOOTLOADER_SOURCE_ADDR;

    if (read_flash_word(0x00000ff8u) != EXPECTED_BOOTLOADER_ADDR) {
        migration_recover(1u);
    }
    if (read_flash_word(0x00000ffcu) != EXPECTED_PARAM_PAGE_ADDR) {
        migration_recover(2u);
    }
    if (meta->magic0 != MIGRATION_MAGIC0 || meta->magic1 != MIGRATION_MAGIC1 ||
        meta->version != 1u || meta->source != BOOTLOADER_SOURCE_ADDR ||
        meta->destination != EXPECTED_BOOTLOADER_ADDR ||
        meta->length != BOOTLOADER_LENGTH) {
        migration_recover(3u);
    }
    if (vectors[0] < 0x20000000u || vectors[0] > 0x20020000u ||
        (vectors[1] & 1u) == 0u || (vectors[1] & ~1u) < EXPECTED_BOOTLOADER_ADDR ||
        (vectors[1] & ~1u) >= BOOTLOADER_END) {
        migration_recover(4u);
    }
    if (crc32((const uint8_t *)BOOTLOADER_SOURCE_ADDR, BOOTLOADER_LENGTH) !=
        meta->crc32) {
        migration_recover(5u);
    }

    RETAINED_STATUS = 0x4d470100u;
    POWER_GPREGRET = GPREGRET_UF2_DFU;
    __asm volatile("dsb" ::: "memory");

    struct mbr_command command = {
        .command = 0u,
        .source = BOOTLOADER_SOURCE_ADDR,
        .length_words = BOOTLOADER_LENGTH / sizeof(uint32_t),
        .unused = 0u,
    };
    uint32_t result = call_mbr(&command);

    /* A successful copy never returns: the MBR starts the new bootloader. */
    migration_recover(0x40u | (result & 0x3fu));
}
