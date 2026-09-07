/* SPDX-License-Identifier: MIT */
#include <zephyr/kernel.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>
#include <hal/nrf_power.h>
#include "apex_pair.h"
#include "apex_aes.h"
#if IS_ENABLED(CONFIG_APEX_RECEIVER_RADIO_PROBE)
#include "apex_radio_probe.h"
#endif

int receiver_usb_init(void);
#if IS_ENABLED(CONFIG_APEX_RECEIVER_RADIO_INPUT)
int receiver_hid_init(void);
int receiver_hid_benchmark(const struct shell *sh, size_t argc, char **argv);
void receiver_hid_poll(void);
void receiver_hid_wait(void);
int receiver_hid_status(const struct shell *sh, size_t argc, char **argv);
int receiver_hid_test_hold(const struct shell *sh, size_t argc, char **argv);
#endif
int receiver_pair_storage_init(const struct shell *sh, size_t argc, char **argv);
int receiver_crypto_test(const struct shell *sh, size_t argc, char **argv);
#ifndef APEX_RECEIVER_BUILD_ID
#define APEX_RECEIVER_BUILD_ID "unrecorded-local-build"
#endif
static uint32_t reset_reason;
static const struct device *const watchdog = DEVICE_DT_GET(DT_NODELABEL(wdt0));
static int watchdog_channel;
static bool inherited_watchdog;

static void enter_bootloader(void)
{
    NRF_POWER->GPREGRET = 0x57;
    sys_reboot(SYS_REBOOT_COLD);
}

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
    ARG_UNUSED(reason);
    ARG_UNUSED(esf);
    enter_bootloader();
    CODE_UNREACHABLE;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    shell_print(sh, "Apex receiver: radio=%s, HID forwarding=%s",
                IS_ENABLED(CONFIG_APEX_RECEIVER_RADIO_PROBE) ? "enabled" : "disabled",
                IS_ENABLED(CONFIG_APEX_RECEIVER_RADIO_INPUT) ? "enabled" : "disabled");
    shell_print(sh, "build: %s", APEX_RECEIVER_BUILD_ID);
    shell_print(sh, "uptime: %lld ms, reset reason: 0x%08x", k_uptime_get(), reset_reason);
    shell_print(sh, "watchdog: %u ticks, %s", NRF_WDT->CRV,
                inherited_watchdog ? "inherited across reset" : "started by receiver");
    shell_print(sh, "application: 0x1000..0x6d000, pairing storage: 0x6d000..0x74000");
    return 0;
}

static void reboot_work(struct k_work *work)
{
    ARG_UNUSED(work);
    enter_bootloader();
}
static K_WORK_DELAYABLE_DEFINE(dfu_work, reboot_work);

static int cmd_dfu(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    shell_print(sh, "Rebooting the dongle into APEXDONGLE.");
    k_work_reschedule(&dfu_work, K_MSEC(200));
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(dongle_commands,
#if IS_ENABLED(CONFIG_APEX_RECEIVER_RADIO_PROBE)
    SHELL_CMD_ARG(radio_test, NULL, "Radio connection and delivery counters.", apex_radio_probe_status, 1, 0),
#endif
#if IS_ENABLED(CONFIG_APEX_RECEIVER_RADIO_INPUT)
    SHELL_CMD_ARG(hid_status, NULL, "USB HID state and media report counters.", receiver_hid_status, 1, 0),
    SHELL_CMD_ARG(hid_bench, NULL, "Pause radio and send 1000 empty USB reports.", receiver_hid_benchmark, 1, 0),
    SHELL_CMD_ARG(hid_test_hold, NULL, "Delay HID for 500 ms and restart the radio session.", receiver_hid_test_hold, 1, 0),
#endif
    SHELL_CMD_ARG(pair_storage_init, NULL, "Initialize unreadable pairing pages; requires confirm.", receiver_pair_storage_init, 1, 1),
    SHELL_CMD_ARG(pair, NULL, "USB pairing: status, write [replace], clear confirm.", apex_pair_shell, 1, 2),
    SHELL_CMD_ARG(status, NULL, "Receiver firmware and reset status.", cmd_status, 1, 0),
    SHELL_CMD_ARG(dfu, NULL, "Enter the dongle USB bootloader.", cmd_dfu, 1, 0),
    SHELL_CMD_ARG(crypto_test, NULL, "Run packet crypto checks locally; no radio traffic.", receiver_crypto_test, 1, 0),
    SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(dongle, &dongle_commands, "Apex receiver commands.", NULL);

int main(void)
{
    /* Receiver builds have no Bluetooth controller or other ECB owner. */
    apex_aes_enable();
    reset_reason = NRF_POWER->RESETREAS;
    NRF_POWER->RESETREAS = reset_reason;
    struct wdt_timeout_cfg cfg = {
        .window = {.min = 0, .max = 5000},
        .flags = WDT_FLAG_RESET_SOC,
    };
    /* nRF52 watchdog configuration survives software resets and cannot be
     * changed while running. Adopt it after an update instead of rebooting. */
    inherited_watchdog = NRF_WDT->RUNSTATUS != 0;
    if (!inherited_watchdog) {
        if (!device_is_ready(watchdog)) enter_bootloader();
        watchdog_channel = wdt_install_timeout(watchdog, &cfg);
        if (watchdog_channel < 0 || wdt_setup(watchdog, WDT_OPT_PAUSE_HALTED_BY_DBG)) {
            enter_bootloader();
        }
    }
#if IS_ENABLED(CONFIG_APEX_RECEIVER_RADIO_INPUT)
    if (receiver_hid_init()) enter_bootloader();
#endif
    if (receiver_usb_init()) enter_bootloader();
#if IS_ENABLED(CONFIG_APEX_RECEIVER_RADIO_PROBE)
    apex_radio_probe_start(APEX_RECEIVER);
#endif
    for (;;) {
        if (inherited_watchdog) {
            for (unsigned int i = 0; i < 8; ++i) {
                if (NRF_WDT->RREN & BIT(i)) NRF_WDT->RR[i] = 0x6e524635;
            }
        } else {
            wdt_feed(watchdog, watchdog_channel);
        }
#if IS_ENABLED(CONFIG_APEX_RECEIVER_RADIO_INPUT)
        receiver_hid_poll();
        receiver_hid_wait();
#else
        k_sleep(K_MSEC(100));
#endif
    }
    return 0;
}
