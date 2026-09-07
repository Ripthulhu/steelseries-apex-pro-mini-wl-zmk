/* SPDX-License-Identifier: MIT */
#include "update_g4b.h"
#include "apex_update.h"
#include "ab_rollback_g4b.h"
#include "apex_control_g4b.h"
#include "spinor_g4b.h"
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell_uart.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static K_MUTEX_DEFINE(update_mutex);
static struct apx_update_download download;
static bool prepared;
static bool preparing;
static atomic_t update_recent, update_at;
#define PREPARING_VERSION (APX_UPDATE_VERSION | 0x80000000u)

bool g4b_update_busy(void)
{
    return atomic_get(&update_recent) &&
           k_uptime_get_32() - (uint32_t)atomic_get(&update_at) < 30000u;
}

static int app_read(uint32_t address, void *data, uint32_t length)
{
    if (address < APX_UPDATE_BASE || address > APX_UPDATE_BASE + APX_UPDATE_CAPACITY ||
        length > APX_UPDATE_BASE + APX_UPDATE_CAPACITY - address) return -EINVAL;
    memcpy(data, (const void *)address, length);
    return 0;
}
static const struct apx_update_io io = {
    .nor_read = g4b_spinor_dev_read, .nor_write = g4b_spinor_dev_program,
    .nor_erase = g4b_spinor_dev_erase, .app_read = app_read,
};

static int load_setting(const char *name, size_t length, settings_read_cb read, void *arg)
{
    uint32_t version;
    if (strcmp(name, "layout")) return -ENOENT;
    if (length != sizeof(version)) return -EINVAL;
    int rc = read(arg, &version, sizeof(version));
    if (rc != sizeof(version)) return rc < 0 ? rc : -EIO;
    prepared = version == APX_UPDATE_VERSION;
    preparing = version == PREPARING_VERSION;
    return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(apex_update, "apex_update", NULL, load_setting, NULL, NULL);

static bool bootloader_ready(void)
{
    const char marker[] = APX_UPDATE_BOOT_MARKER "\r\n";
    const uint8_t *boot = (const uint8_t *)0x74000;
    for (size_t i = 0; i + sizeof(marker) <= 0xa000; i++)
        if (!memcmp(boot + i, marker, sizeof(marker))) return true;
    return false;
}

static bool power_ready(void)
{
    struct apex_battery battery;
    return apex_battery_read(&battery) &&
           (battery.power_good || battery.millivolts >= 3700);
}

static int prepare(const struct shell *sh)
{
    if (sh != shell_backend_uart_get_ptr()) return -EACCES;
    if (!bootloader_ready() || !power_ready()) return -EAGAIN;
    if (prepared) return 0;
    if (!preparing) {
        int rc = apx_update_check_filesystem(&io);
        if (rc) return rc == -2 ? -ENOTEMPTY : -EIO;
        uint32_t version = PREPARING_VERSION;
        rc = settings_save_one("apex_update/layout", &version, sizeof(version));
        if (rc) return rc;
        preparing = true;
    }
    /* The NVS record makes an interrupted erase restartable, without accepting
     * an unrecognized filesystem on a device that was never prepared. */
    if (g4b_spinor_dev_erase(APX_UPDATE_LFS_BASE, 0x15000)) return -EIO;
    uint32_t version = APX_UPDATE_VERSION;
    int rc = settings_save_one("apex_update/layout", &version, sizeof(version));
    if (!rc) { prepared = true; preparing = false; }
    return rc;
}

static bool number(const char *text, uint32_t *value)
{
    char *end;
    if (!*text || *text == '-') return false;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 0);
    if (errno || *end || parsed > UINT32_MAX) return false;
    *value = parsed;
    return true;
}

static int command(const struct shell *sh, size_t argc, char **argv)
{
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "status"))) {
        struct apx_update_manifest h;
        int rc = apx_update_status(&io, &h);
        shell_print(sh, "%s layout=%u prepared=%u bootloader=%u ready=%u received=%u state=%08x bulk=1",
                    APX_UPDATE_APP_MARKER, APX_UPDATE_VERSION, prepared, bootloader_ready(),
                    g4b_ab_update_ready(), download.received, rc ? 0 : h.state);
        if (!rc) {
            char hash[65];
            bin2hex(h.sha256, sizeof(h.sha256), hash, sizeof(hash));
            shell_print(sh, "UPDATE sha256=%s", hash);
        }
        return rc == -1 ? -EIO : 0;
    }
    if (argc == 3 && !strcmp(argv[1], "prepare") && !strcmp(argv[2], "backup-confirmed"))
        return prepare(sh);
    if (!prepared || !bootloader_ready() || !g4b_ab_update_ready()) return -EAGAIN;
    if (argc == 4 && !strcmp(argv[1], "begin")) {
        uint8_t hash[32];
        uint32_t length;
        if (!number(argv[2], &length) || strlen(argv[3]) != 64 ||
            hex2bin(argv[3], 64, hash, sizeof(hash)) != sizeof(hash)) return -EINVAL;
        if (!power_ready()) return -EAGAIN;
        return apx_update_begin(&io, &download, length, hash);
    }
    if (argc == 4 && !strcmp(argv[1], "write")) {
        uint8_t data[32];
        uint32_t offset;
        size_t n = strlen(argv[3]);
        if (!number(argv[2], &offset) || !n || n > 64 || (n & 1) ||
            hex2bin(argv[3], n, data, sizeof(data)) != n / 2) return -EINVAL;
        if (!(offset % APX_UPDATE_SECTOR) && !power_ready()) return -EAGAIN;
        return apx_update_write(&io, &download, offset, data, n / 2);
    }
    if (argc == 2 && !strcmp(argv[1], "commit")) {
        if (!power_ready()) return -EAGAIN;
        return apx_update_request(&io, &download);
    }
    if (argc == 2 && !strcmp(argv[1], "reboot")) {
        struct apx_update_manifest h;
        if (apx_update_status(&io, &h) || h.state != APX_UPDATE_REQUESTED) return -EINVAL;
        if (!power_ready()) return -EAGAIN;
        sys_reboot(SYS_REBOOT_COLD);
        return 0;
    }
    return -EINVAL;
}

int g4b_update_command(const struct shell *sh, size_t argc, char **argv)
{
    if (k_mutex_lock(&update_mutex, K_NO_WAIT)) return -EBUSY;
    atomic_set(&update_at, k_uptime_get_32());
    atomic_set(&update_recent, 1);
    int rc = command(sh, argc, argv);
    shell_print(sh, "UPDATE result=%d received=%u", rc, download.received);
    k_mutex_unlock(&update_mutex);
    return rc;
}

int g4b_update_health(void)
{
    k_mutex_lock(&update_mutex, K_FOREVER);
    int rc = apx_update_health(&io);
    k_mutex_unlock(&update_mutex);
    return rc;
}

int g4b_update_binary(uint32_t offset, const uint8_t *data, size_t length, uint32_t *received)
{
    if (k_mutex_lock(&update_mutex, K_NO_WAIT)) return -EBUSY;
    atomic_set(&update_at, k_uptime_get_32());
    atomic_set(&update_recent, 1);
    int rc = -EAGAIN;
    if (prepared && g4b_ab_update_ready() && download.active) {
        if (!(offset % APX_UPDATE_SECTOR) && !power_ready()) rc = -EAGAIN;
        else rc = apx_update_write(&io, &download, offset, data, length);
    }
    *received = download.received;
    k_mutex_unlock(&update_mutex);
    return rc;
}
