/* SPDX-License-Identifier: MIT */
#include "apex_update.h"
#include <string.h>
#include <tinycrypt/sha256.h>

/* Optional install-progress hook. The bootloader defines this (before including
 * this file) to drive the same red->green key-matrix bar the DFU write path uses
 * (board_rgb_progress). Portable builds and the tests leave it a no-op. */
#ifndef APX_UPDATE_PROGRESS
#define APX_UPDATE_PROGRESS(done, total) ((void)0)
#endif

_Static_assert(sizeof(struct apx_update_manifest) == 56, "update descriptor size");
_Static_assert(APX_UPDATE_FIRST_SIZE + APX_UPDATE_SECOND_SIZE + APX_UPDATE_THIRD_SIZE ==
               APX_UPDATE_CAPACITY, "candidate capacity");

uint32_t apx_update_crc(const void *data, size_t size)
{
    const uint8_t *p = data;
    uint32_t crc = ~0u;
    while (size--) {
        crc ^= *p++;
        for (unsigned int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

uint32_t apx_update_address(uint32_t offset)
{
    if (offset < APX_UPDATE_FIRST_SIZE) return APX_UPDATE_FIRST_ADDR + offset;
    if (offset < APX_UPDATE_FIRST_SIZE + APX_UPDATE_SECOND_SIZE)
        return APX_UPDATE_SECOND_ADDR + offset - APX_UPDATE_FIRST_SIZE;
    if (offset < APX_UPDATE_CAPACITY)
        return APX_UPDATE_THIRD_ADDR + offset - APX_UPDATE_FIRST_SIZE - APX_UPDATE_SECOND_SIZE;
    return UINT32_MAX;
}

bool apx_update_manifest_valid(const struct apx_update_manifest *h)
{
    return h->magic == APX_UPDATE_MAGIC && h->version == APX_UPDATE_VERSION &&
           h->board == APX_UPDATE_BOARD && h->length >= 8 &&
           h->length <= APX_UPDATE_CAPACITY && !(h->length & 3u) &&
           h->crc == apx_update_crc(h, offsetof(struct apx_update_manifest, crc)) &&
           (h->state == APX_UPDATE_DOWNLOADING || h->state == APX_UPDATE_REQUESTED ||
            h->state == APX_UPDATE_COPYING || h->state == APX_UPDATE_TRIAL ||
            h->state == APX_UPDATE_HEALTHY || h->state == APX_UPDATE_REJECTED);
}

int apx_update_status(const struct apx_update_io *io, struct apx_update_manifest *h)
{
    if (io->nor_read(APX_UPDATE_JOURNAL, h, sizeof(*h))) return -1;
    return apx_update_manifest_valid(h) ? 0 : -2;
}

static int set_state(const struct apx_update_io *io, uint32_t state)
{
    uint32_t actual;
    uint32_t address = APX_UPDATE_JOURNAL + offsetof(struct apx_update_manifest, state);
    if (io->nor_write(address, &state, sizeof(state)) ||
        io->nor_read(address, &actual, sizeof(actual)) || actual != state) return -1;
    return 0;
}

int apx_update_check_filesystem(const struct apx_update_io *io)
{
    /* Empty 21-block volume created by the pinned LittleFS formatter. Its
     * complete image was mounted read-only and checked for user entries.
     * Different metadata or any data beyond it requires manual migration. */
    static const uint8_t empty_header[64] = {
        0x01,0x00,0x00,0x00,0xf0,0x0f,0xff,0xf7,0x6c,0x69,0x74,0x74,0x6c,0x65,0x66,0x73,
        0x2f,0xe0,0x00,0x10,0x01,0x00,0x02,0x00,0x00,0x10,0x00,0x00,0x15,0x00,0x00,0x00,
        0xff,0x00,0x00,0x00,0xff,0xff,0xff,0x7f,0xfe,0x03,0x00,0x00,0x7f,0xef,0xfc,0x10,
        0x10,0x00,0x00,0x00,0xe5,0x39,0x4c,0xc0,0x0f,0xf0,0x00,0x0c,0xaa,0x34,0xff,0x20,
    };
    uint8_t buffer[256];
    bool blank = true, known_empty = true;
    for (uint32_t offset = 0; offset < 0x15000; offset += sizeof(buffer)) {
        if (io->nor_read(APX_UPDATE_LFS_BASE + offset, buffer, sizeof(buffer))) return -1;
        for (uint32_t i = 0; i < sizeof(buffer); i++) {
            uint8_t expected = offset + i < sizeof(empty_header) ? empty_header[offset + i] : 0xff;
            if (buffer[i] != 0xff) blank = false;
            if (buffer[i] != expected) known_empty = false;
        }
    }
    return blank || known_empty ? 0 : -2;
}

int apx_update_begin(const struct apx_update_io *io, struct apx_update_download *d,
                     uint32_t length, const uint8_t sha256[32])
{
    struct apx_update_manifest old;
    d->active = false;
    d->received = 0;
    d->manifest = (struct apx_update_manifest) {
        .magic = APX_UPDATE_MAGIC, .version = APX_UPDATE_VERSION,
        .board = APX_UPDATE_BOARD, .length = length, .state = APX_UPDATE_DOWNLOADING,
    };
    memcpy(d->manifest.sha256, sha256, 32);
    d->manifest.crc = apx_update_crc(&d->manifest, offsetof(struct apx_update_manifest, crc));
    if (!apx_update_manifest_valid(&d->manifest)) return -2;
    int rc = apx_update_status(io, &old);
    if (rc == -1) return rc;
    if (!rc && (old.state == APX_UPDATE_REQUESTED || old.state == APX_UPDATE_COPYING ||
                old.state == APX_UPDATE_TRIAL)) return -3;
    if (io->nor_erase(APX_UPDATE_JOURNAL, APX_UPDATE_SECTOR) ||
        io->nor_write(APX_UPDATE_JOURNAL, &d->manifest, sizeof(d->manifest)) ||
        apx_update_status(io, &old) || memcmp(&old, &d->manifest, sizeof(old))) return -1;
    d->active = true;
    return 0;
}

int apx_update_write(const struct apx_update_io *io, struct apx_update_download *d,
                     uint32_t offset, const uint8_t *data, uint32_t size)
{
    uint8_t verify[256];
    if (!d->active || !size || size > sizeof(verify) || offset > d->manifest.length ||
        size > d->manifest.length - offset ||
        size > APX_UPDATE_SECTOR - (offset % APX_UPDATE_SECTOR)) return -2;
    uint32_t address = apx_update_address(offset);
    if (offset < d->received) {
        if (size > d->received - offset || io->nor_read(address, verify, size) ||
            memcmp(verify, data, size)) return -2;
        return 0;
    }
    if (offset != d->received) return -2;
    if (!(offset % APX_UPDATE_SECTOR) && io->nor_erase(address, APX_UPDATE_SECTOR)) return -1;
    if (io->nor_write(address, data, size) || io->nor_read(address, verify, size) ||
        memcmp(verify, data, size)) return -1;
    d->received += size;
    return 0;
}

static int verify_image(const struct apx_update_io *io,
                        const struct apx_update_manifest *h, bool internal)
{
    struct tc_sha256_state_struct hash;
    uint8_t buffer[256], digest[32];
    uint32_t marker_match = 0;
    bool marker_found = false;
    const char marker[] = APX_UPDATE_APP_MARKER;
    if (!tc_sha256_init(&hash)) return -1;
    for (uint32_t offset = 0; offset < h->length;) {
        uint32_t n = h->length - offset;
        if (n > sizeof(buffer)) n = sizeof(buffer);
        int rc = internal ? io->app_read(APX_UPDATE_BASE + offset, buffer, n) :
                            io->nor_read(apx_update_address(offset), buffer, n);
        if (rc) return -1;
        if (!offset) {
            uint32_t vectors[2];
            memcpy(vectors, buffer, sizeof(vectors));
            if (vectors[0] <= 0x20000000u || vectors[0] > 0x20020000u || (vectors[0] & 7u) ||
                !(vectors[1] & 1u) || vectors[1] < APX_UPDATE_BASE ||
                (vectors[1] & ~1u) >= APX_UPDATE_BASE + h->length) return -2;
        }
        for (uint32_t i = 0; i < n; i++) {
            if (buffer[i] == (uint8_t)marker[marker_match]) marker_match++;
            else marker_match = buffer[i] == (uint8_t)marker[0] ? 1 : 0;
            if (marker_match == sizeof(marker) - 1) { marker_found = true; marker_match = 0; }
        }
        if (!tc_sha256_update(&hash, buffer, n)) return -1;
        offset += n;
    }
    if (!tc_sha256_final(digest, &hash)) return -1;
    return marker_found && !memcmp(digest, h->sha256, sizeof(digest)) ? 0 : -2;
}

int apx_update_request(const struct apx_update_io *io, struct apx_update_download *d)
{
    if (!d->active || d->received != d->manifest.length) return -2;
    int rc = verify_image(io, &d->manifest, false);
    if (rc) return rc;
    rc = set_state(io, APX_UPDATE_REQUESTED);
    if (!rc) d->active = false;
    return rc;
}

int apx_update_install(const struct apx_update_io *io)
{
    struct apx_update_manifest h;
    uint8_t source[256], current[256];
    int rc = apx_update_status(io, &h);
    if (rc == -2)
        return h.magic == APX_UPDATE_MAGIC && h.state != APX_UPDATE_DOWNLOADING ? -2 : 0;
    if (rc) return rc;
    if (h.state != APX_UPDATE_REQUESTED && h.state != APX_UPDATE_COPYING) return 0;
    if (verify_image(io, &h, false)) {
        (void)set_state(io, APX_UPDATE_REJECTED);
        return -2;
    }
    if (set_state(io, APX_UPDATE_COPYING)) return -1;
    for (uint32_t page = 0; page < h.length; page += APX_UPDATE_SECTOR) {
        APX_UPDATE_PROGRESS(page, h.length);
        uint32_t end = page + APX_UPDATE_SECTOR;
        if (end > h.length) end = h.length;
        bool same = true;
        for (uint32_t offset = page; offset < end; offset += sizeof(source)) {
            uint32_t n = end - offset;
            if (n > sizeof(source)) n = sizeof(source);
            if (io->nor_read(apx_update_address(offset), source, n) ||
                io->app_read(APX_UPDATE_BASE + offset, current, n)) return -1;
            if (memcmp(source, current, n)) same = false;
        }
        if (same) continue;
        if (io->app_erase(APX_UPDATE_BASE + page, APX_UPDATE_SECTOR)) return -1;
        for (uint32_t offset = page; offset < end; offset += sizeof(source)) {
            uint32_t n = end - offset;
            if (n > sizeof(source)) n = sizeof(source);
            if (io->nor_read(apx_update_address(offset), source, n) ||
                io->app_write(APX_UPDATE_BASE + offset, source, n)) return -1;
        }
    }
    if (verify_image(io, &h, true) || io->app_finish()) return -1;
    if (set_state(io, APX_UPDATE_TRIAL)) return -1;
    APX_UPDATE_PROGRESS(h.length, h.length); /* full bar -> green: verified copy */
    return 1;
}

int apx_update_health(const struct apx_update_io *io)
{
    struct apx_update_manifest h;
    int rc = apx_update_status(io, &h);
    if (rc == -2) return 0;
    if (rc) return rc;
    if (h.state != APX_UPDATE_TRIAL) return 0;
    rc = verify_image(io, &h, true);
    if (rc == -1) return rc;
    return set_state(io, rc ? APX_UPDATE_REJECTED : APX_UPDATE_HEALTHY);
}
