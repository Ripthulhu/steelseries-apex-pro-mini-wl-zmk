/* SPDX-License-Identifier: MIT */
#include "apex_pair.h"
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#define BOND_SETTING "apex_radio/bond"
static K_MUTEX_DEFINE(bond_mutex);

struct bond_read {
    uint8_t *record;
    int result;
};

static int read_bond(const char *name, size_t len, settings_read_cb read_cb,
                     void *cb_arg, void *param)
{
    struct bond_read *read = param;
    if (name && *name) return 0;
    if (len == 0) {
        read->result = -ENOENT;
        return 0;
    }
    if (len != APEX_BOND_SIZE) {
        read->result = -EBADMSG;
        return 0;
    }
    int rc = read_cb(cb_arg, read->record, APEX_BOND_SIZE);
    read->result = rc < 0 ? rc :
        (rc == APEX_BOND_SIZE && apex_bond_valid(read->record) ? 0 : -EBADMSG);
    return 0;
}

int apex_bond_load(uint8_t record[APEX_BOND_SIZE])
{
    if (!record) return -EINVAL;
    struct bond_read read = {.record = record, .result = -ENOENT};
    k_mutex_lock(&bond_mutex, K_FOREVER);
    int rc = settings_load_subtree_direct(BOND_SETTING, read_bond, &read);
    k_mutex_unlock(&bond_mutex);
    if (!rc) rc = read.result;
    if (rc) memset(record, 0, APEX_BOND_SIZE);
    return rc;
}

int apex_bond_store(const uint8_t record[APEX_BOND_SIZE])
{
    if (!apex_bond_valid(record)) return -EINVAL;
    k_mutex_lock(&bond_mutex, K_FOREVER);
    int rc = settings_save_one(BOND_SETTING, record, APEX_BOND_SIZE);
    k_mutex_unlock(&bond_mutex);
    return rc;
}

int apex_bond_delete(void)
{
    k_mutex_lock(&bond_mutex, K_FOREVER);
    int rc = settings_delete(BOND_SETTING);
    k_mutex_unlock(&bond_mutex);
    return rc;
}
