/* SPDX-License-Identifier: MIT */
#include "apex_pair.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/shell/shell.h>
#include <string.h>

static struct nvs_fs bonds;
static K_MUTEX_DEFINE(store_mutex);
static bool mounted;

static int mount_store(void)
{
    if (mounted) return 0;
    struct flash_pages_info page;
    bonds.flash_device = FIXED_PARTITION_DEVICE(storage_partition);
    bonds.offset = FIXED_PARTITION_OFFSET(storage_partition);
    if (!device_is_ready(bonds.flash_device)) return -ENODEV;
    int rc = flash_get_page_info_by_offs(bonds.flash_device, bonds.offset, &page);
    if (rc) return rc;
    bonds.sector_size = page.size;
    bonds.sector_count = FIXED_PARTITION_SIZE(storage_partition) / page.size;
    rc = nvs_mount(&bonds);
    if (!rc) mounted = true;
    return rc;
}

int receiver_pair_storage_init(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "confirm")) {
        shell_error(sh, "Use pair_storage_init confirm to erase only the reserved pairing pages.");
        return -EINVAL;
    }
    k_mutex_lock(&store_mutex, K_FOREVER);
    int rc = mount_store();
    /* Never erase a readable store or treat a hardware failure as corruption. */
    if (rc == -EDEADLK) {
        rc = flash_erase(FIXED_PARTITION_DEVICE(storage_partition),
                         FIXED_PARTITION_OFFSET(storage_partition),
                         FIXED_PARTITION_SIZE(storage_partition));
        if (!rc) rc = mount_store();
    }
    k_mutex_unlock(&store_mutex);
    if (rc) shell_error(sh, "Pairing storage initialization failed: %d", rc);
    else shell_print(sh, "Pairing storage ready.");
    return rc;
}

int apex_bond_load(uint8_t record[APEX_BOND_SIZE])
{
    k_mutex_lock(&store_mutex, K_FOREVER);
    int rc = mount_store();
    if (!rc) {
        rc = nvs_read(&bonds, 1, record, APEX_BOND_SIZE);
        if (rc >= 0) rc = rc == APEX_BOND_SIZE && apex_bond_valid(record) ? 0 : -EBADMSG;
    }
    k_mutex_unlock(&store_mutex);
    return rc;
}

int apex_bond_store(const uint8_t record[APEX_BOND_SIZE])
{
    if (!apex_bond_valid(record)) return -EINVAL;
    k_mutex_lock(&store_mutex, K_FOREVER);
    int rc = mount_store();
    if (!rc) {
        rc = nvs_write(&bonds, 1, record, APEX_BOND_SIZE);
        if (rc >= 0) rc = 0;
    }
    k_mutex_unlock(&store_mutex);
    return rc;
}

int apex_bond_delete(void)
{
    k_mutex_lock(&store_mutex, K_FOREVER);
    int rc = mount_store();
    if (!rc) rc = nvs_delete(&bonds, 1);
    if (rc == -ENOENT) rc = 0;
    k_mutex_unlock(&store_mutex);
    return rc;
}
