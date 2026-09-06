/* SPDX-License-Identifier: MIT */
#include "apex_pair.h"
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell_uart.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>
#include <tinycrypt/sha256.h>

BUILD_ASSERT(DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_shell_uart), zephyr_cdc_acm_uart),
             "Pairing requires a USB CDC shell");
static K_MUTEX_DEFINE(pair_mutex);
static uint8_t receive_record[APEX_BOND_SIZE];
static size_t received;
static bool expired;
static int64_t receive_deadline;

static void wipe(void *data, size_t size)
{
    volatile uint8_t *p = data;
    while (size--) *p++ = 0;
}

int apex_bond_valid(const uint8_t r[APEX_BOND_SIZE])
{
    if (!r || memcmp(r, "APB1", 4) || sys_get_le32(r + 28) != crc32_ieee(r, 28)) return 0;
    uint8_t key = 0, id = 0;
    for (size_t i = 0; i < 8; ++i) id |= r[4 + i];
    for (size_t i = 0; i < 16; ++i) key |= r[12 + i];
    return key && id;
}

static void timeout_work(struct k_work *work)
{
    ARG_UNUSED(work);
    k_mutex_lock(&pair_mutex, K_FOREVER);
    if (k_uptime_get() >= receive_deadline) {
        expired = true;
        wipe(receive_record, sizeof(receive_record));
    }
    k_mutex_unlock(&pair_mutex);
    /* Keep bypass installed until the next input is discarded. A late key
     * must not be echoed or interpreted as an ordinary shell command. */
}
static K_WORK_DELAYABLE_DEFINE(pair_timeout, timeout_work);

static void receive_bytes(const struct shell *sh, uint8_t *data, size_t length)
{
    int rc = 0;
    k_mutex_lock(&pair_mutex, K_FOREVER);
    if (expired || k_uptime_get() >= receive_deadline) rc = -ETIMEDOUT;
    else if (length > sizeof(receive_record) - received) rc = -EMSGSIZE;
    else {
        memcpy(receive_record + received, data, length);
        received += length;
        if (received < sizeof(receive_record)) {
            wipe(data, length);
            k_mutex_unlock(&pair_mutex);
            return;
        }
        rc = apex_bond_valid(receive_record) ? apex_bond_store(receive_record) : -EINVAL;
    }
    wipe(data, length);
    wipe(receive_record, sizeof(receive_record));
    k_work_cancel_delayable(&pair_timeout);
    shell_set_bypass(sh, NULL);
    if (rc) shell_error(sh, "APX_PAIR_ERROR %d", rc);
    else shell_print(sh, "APX_PAIR_SAVED");
    k_mutex_unlock(&pair_mutex);
}

int apex_pair_shell(const struct shell *sh, size_t argc, char **argv)
{
    if (sh != shell_backend_uart_get_ptr()) return -EACCES;
    uint8_t record[APEX_BOND_SIZE];
    int rc = apex_bond_load(record);
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "status"))) {
        if (rc == -ENOENT) shell_print(sh, "APX_PAIR_V1 unpaired");
        else if (rc) shell_error(sh, "APX_PAIR_ERROR %d", rc);
        else {
            struct tc_sha256_state_struct hash;
            uint8_t digest[32];
            char id[17], fingerprint[17];
            tc_sha256_init(&hash);
            tc_sha256_update(&hash, record + 4, 24);
            tc_sha256_final(digest, &hash);
            bin2hex(record + 4, 8, id, sizeof(id));
            bin2hex(digest, 8, fingerprint, sizeof(fingerprint));
            shell_print(sh, "APX_PAIR_V1 paired %s %s", id, fingerprint);
            wipe(digest, sizeof(digest));
            wipe(&hash, sizeof(hash));
        }
        wipe(record, sizeof(record));
        return rc == -ENOENT ? 0 : rc;
    }
    wipe(record, sizeof(record));
    if (argc == 3 && !strcmp(argv[1], "clear") && !strcmp(argv[2], "confirm")) {
        rc = apex_bond_delete();
        if (rc) shell_error(sh, "APX_PAIR_ERROR %d", rc);
        else shell_print(sh, "APX_PAIR_CLEARED");
        return rc;
    }
    if ((argc == 2 || (argc == 3 && !strcmp(argv[2], "replace"))) && !strcmp(argv[1], "write")) {
        if (rc && rc != -ENOENT) return rc;
        if (!rc && argc != 3) {
            shell_error(sh, "Already paired; use write replace to change the pairing.");
            return -EALREADY;
        }
        k_mutex_lock(&pair_mutex, K_FOREVER);
        received = 0;
        expired = false;
        receive_deadline = k_uptime_get() + 5000;
        wipe(receive_record, sizeof(receive_record));
        shell_set_bypass(sh, receive_bytes);
        k_work_reschedule(&pair_timeout, K_SECONDS(5));
        shell_print(sh, "APX_PAIR_READY 32");
        k_mutex_unlock(&pair_mutex);
        return 0;
    }
    shell_error(sh, "Usage: pair [status | write [replace] | clear confirm]");
    return -EINVAL;
}
