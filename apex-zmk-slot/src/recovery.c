/* SPDX-License-Identifier: MIT */

#include <nrf.h>
#include <string.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#endif
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#define APEX_APPLICATION_BASE 0x0001C000UL
#define APEX_WDT_BASE 0x40010000UL
#define APEX_WDT_RUNSTATUS (*(volatile uint32_t *)(APEX_WDT_BASE + 0x400UL))
#define APEX_WDT_RREN (*(volatile uint32_t *)(APEX_WDT_BASE + 0x508UL))

static bool apex_vendor_wdt_active;
static uint32_t apex_vendor_wdt_rren;

static int apex_vendor_handoff_init(void) {
    apex_vendor_wdt_active = (APEX_WDT_RUNSTATUS & 1U) != 0U;
    apex_vendor_wdt_rren = APEX_WDT_RREN;
    return 0;
}

SYS_INIT(apex_vendor_handoff_init, PRE_KERNEL_1, 0);

/*
 * This routine must survive erasing the page containing the vector table.
 * Keep it in RAM, disable interrupts before the erase, and reset directly
 * through AIRCR without returning to flash.
 */
static __ramfunc FUNC_NORETURN void apex_invalidate_and_reset(void) {
    __disable_irq();

    while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {
    }
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Een << NVMC_CONFIG_WEN_Pos;
    while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {
    }
    NRF_NVMC->ERASEPAGE = APEX_APPLICATION_BASE;
    while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {
    }
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren << NVMC_CONFIG_WEN_Pos;
    while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {
    }

    __DSB();
    SCB->AIRCR = (0x5FAUL << SCB_AIRCR_VECTKEY_Pos) | SCB_AIRCR_SYSRESETREQ_Msk;
    __DSB();
    for (;;) {
        __NOP();
    }
}

static void apex_auto_return_handler(struct k_work *work) {
    ARG_UNUSED(work);
    apex_invalidate_and_reset();
}

K_WORK_DELAYABLE_DEFINE(apex_auto_return_work, apex_auto_return_handler);

static int apex_recovery_init(void) {
    /*
     * The recovery wrapper owns its enabled RR channel.  Experimental ZMK
     * images must not feed it: expiration is the hardware-backed escape path
     * when Bluetooth, logging, or the scheduler fails.
     */
    if (CONFIG_APEX_RECOVERY_AUTO_RETURN_SECONDS > 0) {
        k_work_schedule(&apex_auto_return_work,
                        K_SECONDS(CONFIG_APEX_RECOVERY_AUTO_RETURN_SECONDS));
    }
    return 0;
}

SYS_INIT(apex_recovery_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#if defined(CONFIG_SHELL)
static int cmd_apex_status(const struct shell *sh, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    shell_print(sh, "Apex ZMK is running from 0x%08lx; auto-return=%d seconds",
                APEX_APPLICATION_BASE, CONFIG_APEX_RECOVERY_AUTO_RETURN_SECONDS);
    shell_print(sh, "vendor watchdog inherited=%s",
                apex_vendor_wdt_active ? "yes" : "no");
    shell_print(sh, "vendor watchdog RREN=0x%08lx (not fed by ZMK)",
                (unsigned long)apex_vendor_wdt_rren);
    return 0;
}

static int cmd_apex_keep(const struct shell *sh, size_t argc, char **argv) {
    if (argc != 2 || strcmp(argv[1], "CONFIRM") != 0) {
        shell_error(sh, "usage: apex keep CONFIRM");
        return -EINVAL;
    }
    int rc = k_work_cancel_delayable(&apex_auto_return_work);
    shell_print(sh, "automatic return cancelled (result %d)", rc);
    return 0;
}

static int cmd_apex_bootloader(const struct shell *sh, size_t argc, char **argv) {
    if (argc != 2 || strcmp(argv[1], "ERASE-FIRST-PAGE") != 0) {
        shell_error(sh, "usage: apex bootloader ERASE-FIRST-PAGE");
        return -EINVAL;
    }
    shell_warn(sh, "invalidating application and returning to vendor bootloader");
    k_sleep(K_MSEC(250));
    apex_invalidate_and_reset();
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    apex_commands,
    SHELL_CMD(status, NULL, "Show recovery status", cmd_apex_status),
    SHELL_CMD(keep, NULL, "Cancel the canary auto-return", cmd_apex_keep),
    SHELL_CMD(bootloader, NULL, "Invalidate app and enter vendor bootloader",
              cmd_apex_bootloader),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(apex, &apex_commands, "Apex Pro Mini Wireless recovery", NULL);
#endif
