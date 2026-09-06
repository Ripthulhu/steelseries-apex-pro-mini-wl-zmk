/* SPDX-License-Identifier: MIT */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include "apex_packet.h"

/* This local test never accesses pairing storage or the radio. */
int receiver_crypto_test(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    static const uint8_t reference[] = {
        0x01, 0x01, 0x00, 0x10, 0x01, 0x00, 0x00, 0x00,
        0xaf, 0x5f, 0xe1, 0xd4, 0x67, 0x03, 0x47, 0xd6,
        0x9c, 0x91, 0x80, 0xba, 0x07, 0xc6, 0x39, 0x55,
        0xe7, 0xb8, 0xd0, 0xf0, 0x68, 0x32, 0x01, 0xb2,
        0xcb, 0x6f, 0x08, 0x20, 0x8f, 0x7b, 0xb3, 0x3c,
    };
    struct apex_session tx, rx;
    uint8_t key[16], kn[16], dn[16], packet[APEX_PACKET_MAX], output[64], type;
    for (unsigned int i = 0; i < 16; ++i) {
        key[i] = i;
        kn[i] = i + 16;
        dn[i] = i + 32;
    }
    int rc = apex_session_init(&tx, key, kn, dn, APEX_KEYBOARD);
    if (!rc) rc = apex_session_init(&rx, key, kn, dn, APEX_RECEIVER);
    uint32_t start = k_cycle_get_32();
    for (unsigned int i = 0; !rc && i < 100; ++i) {
        int n = apex_packet_encode(&tx, APEX_PACKET_INPUT, kn, sizeof(kn), packet, sizeof(packet));
        if (i == 0 && (n != sizeof(reference) || memcmp(packet, reference, sizeof(reference)))) {
            rc = -EIO;
            break;
        }
        if (n < 0 || apex_packet_decode(&rx, packet, n, &type, output, sizeof(output)) != 16 ||
            type != APEX_PACKET_INPUT || memcmp(output, kn, 16)) {
            rc = -EIO;
            break;
        }
        if (apex_packet_decode(&rx, packet, n, &type, output, sizeof(output)) != APEX_PACKET_REPLAY) {
            rc = -EIO;
        }
    }
    uint32_t elapsed = k_cycle_get_32() - start;
    apex_session_clear(&tx);
    apex_session_clear(&rx);
    if (rc) {
        shell_error(sh, "Packet crypto test failed: %d", rc);
    } else {
        shell_print(sh, "100 encrypt/decrypt/replay checks passed; %llu us total",
                    k_cyc_to_us_floor64(elapsed));
    }
    return rc;
}
