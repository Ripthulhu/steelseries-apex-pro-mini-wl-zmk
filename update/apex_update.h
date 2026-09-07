/* SPDX-License-Identifier: MIT */
#ifndef APEX_UPDATE_H
#define APEX_UPDATE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "apex_update_layout.h"

#define APX_UPDATE_VERSION 3u
#define APX_UPDATE_BOARD 0x41504b31u
#define APX_UPDATE_MAGIC 0x55585041u
#define APX_UPDATE_BASE 0x1000u
#define APX_UPDATE_DOWNLOADING 0xffffffffu
#define APX_UPDATE_REQUESTED 0xfffffffeu
#define APX_UPDATE_COPYING 0xfffffffcu
#define APX_UPDATE_TRIAL 0xfffffff8u
#define APX_UPDATE_HEALTHY 0xfffffff0u
#define APX_UPDATE_REJECTED 0xffffffe0u
#define APX_UPDATE_APP_MARKER "APEX-KBD-OTA3"
#define APX_UPDATE_BOOT_MARKER "APEX-BOOT-OTA3"

/* Little-endian on both nRF52833 devices. State is excluded from the CRC so
 * each transition can clear bits without erasing the committed descriptor. */
struct apx_update_manifest {
    uint32_t magic, version, board, length;
    uint8_t sha256[32];
    uint32_t crc, state;
};

struct apx_update_io {
    int (*nor_read)(uint32_t address, void *data, uint32_t size);
    int (*nor_write)(uint32_t address, const void *data, uint32_t size);
    int (*nor_erase)(uint32_t address, uint32_t size);
    int (*app_read)(uint32_t address, void *data, uint32_t size);
    int (*app_write)(uint32_t address, const void *data, uint32_t size);
    int (*app_erase)(uint32_t address, uint32_t size);
    int (*app_finish)(void);
};

struct apx_update_download {
    struct apx_update_manifest manifest;
    uint32_t received;
    bool active;
};

uint32_t apx_update_crc(const void *data, size_t size);
uint32_t apx_update_address(uint32_t offset);
bool apx_update_manifest_valid(const struct apx_update_manifest *manifest);
int apx_update_status(const struct apx_update_io *io, struct apx_update_manifest *manifest);
int apx_update_check_filesystem(const struct apx_update_io *io);
int apx_update_begin(const struct apx_update_io *io, struct apx_update_download *download,
                     uint32_t length, const uint8_t sha256[32]);
int apx_update_write(const struct apx_update_io *io, struct apx_update_download *download,
                     uint32_t offset, const uint8_t *data, uint32_t size);
int apx_update_request(const struct apx_update_io *io, struct apx_update_download *download);
/* 1: installed and verified, 0: no installation requested, negative: failure.
 * A failure must lead to fallback or wired recovery, never an unchecked jump. */
int apx_update_install(const struct apx_update_io *io);
int apx_update_health(const struct apx_update_io *io);
#endif
