/* SPDX-License-Identifier: MIT
 *
 * Enter the Adafruit_nRF52_Bootloader (UF2/serial DFU) over USB from a host,
 * with no key press. Two triggers on a dedicated CDC-ACM port (dfu_cdc):
 *
 *  1. MAGIC STRING (reliable): the host writes the exact eight bytes
 *     "APEXDFU!" to the port through the normal CDC data path.
 *
 *  2. 1200-baud "touch" (standard, best-effort): opening the port at 1200 baud
 *     via the CDC DTE-rate-change callback. Works with tools that actually send
 *     SET_LINE_CODING (adafruit-nrfutil, Arduino); some hosts/drivers don't.
 *
 * Reboot path: sys_reboot(RST_UF2). ZMK's NRF_STORE_REBOOT_TYPE_GPREGRET (on by
 * default for nRF52) stores 0x57 into NRF_POWER->GPREGRET, which the Adafruit
 * bootloader reads as DFU_MAGIC_UF2_RESET. Same mechanism as &bootloader.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>

#include <nrfx.h>

/* dt-bindings/zmk/reset.h : RST_UF2 = 0x57 (the Adafruit UF2 DFU magic). */
#define G4B_RST_UF2        0x57
#define G4B_DFU_TOUCH_BAUD 1200u

/* Host writes this exact byte string to the dfu CDC to request DFU. */
static const char g4b_dfu_magic[] = "APEXDFU!";
#define G4B_DFU_MAGIC_LEN (sizeof(g4b_dfu_magic) - 1)

#define G4B_DFU_CDC_NODE DT_NODELABEL(dfu_cdc)

#if DT_NODE_EXISTS(G4B_DFU_CDC_NODE)

static const struct device *const g4b_dfu_cdc = DEVICE_DT_GET(G4B_DFU_CDC_NODE);

static void g4b_dfu_reboot(struct k_work *work)
{
	ARG_UNUSED(work);
	/* Write the Adafruit UF2 DFU magic to GPREGRET, then reset. Done directly
	 * (not sys_reboot(RST_UF2)) because this ZMK/Zephyr tree does NOT translate
	 * the reboot type to GPREGRET: NRF_STORE_REBOOT_TYPE_GPREGRET is an unused
	 * Kconfig symbol and sys_arch_reboot() is the __weak stub that ignores the
	 * type. GPREGRET (0x4000051C) is what the bootloader's check_dfu_mode reads. */
	NRF_POWER->GPREGRET = G4B_RST_UF2;
	__DSB();
	NVIC_SystemReset();
}

static K_WORK_DELAYABLE_DEFINE(g4b_dfu_reboot_work, g4b_dfu_reboot);

static void g4b_dfu_arm(void)
{
	/* Defer so the USB transfer that requested it can complete first. */
	k_work_reschedule(&g4b_dfu_reboot_work, K_MSEC(50));
}

/* --- trigger 1: magic string on the CDC RX ------------------------------- */

static void g4b_dfu_uart_isr(const struct device *dev, void *user_data)
{
	static uint8_t match; /* how many magic bytes matched so far */
	uint8_t c;

	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) > 0 && uart_irq_rx_ready(dev) > 0) {
		while (uart_fifo_read(dev, &c, 1) == 1) {
			if (c == (uint8_t)g4b_dfu_magic[match]) {
				if (++match == G4B_DFU_MAGIC_LEN) {
					match = 0;
					g4b_dfu_arm();
				}
			} else {
				/* restart, allowing this byte to be a new start */
				match = (c == (uint8_t)g4b_dfu_magic[0]) ? 1u : 0u;
			}
		}
	}
}

/* --- trigger 2: 1200-baud touch (host SET_LINE_CODING) ------------------- */

/* Strong override of the __weak hook in ZMK's usb.c. The next stack has no
 * per-device DTE-rate callback (legacy cdc_acm_dte_rate_callback_set is gone);
 * the single usbd_msg cb routes USBD_MSG_CDC_ACM_LINE_CODING here. Filter to the
 * port so the Studio RPC CDC's line-coding can't false-trigger DFU. */
void zmk_usb_cdc_line_coding_changed(const struct device *dev);

void zmk_usb_cdc_line_coding_changed(const struct device *dev)
{
	uint32_t baud = 0;

	if (dev != g4b_dfu_cdc) {
		return;
	}
	if (uart_line_ctrl_get(dev, UART_LINE_CTRL_BAUD_RATE, &baud) == 0 &&
	    baud == G4B_DFU_TOUCH_BAUD) {
		g4b_dfu_arm();
	}
}

static int g4b_dfu_trigger_init(void)
{
	if (!device_is_ready(g4b_dfu_cdc)) {
		return 0;
	}

	uart_irq_rx_disable(g4b_dfu_cdc);
	uart_irq_tx_disable(g4b_dfu_cdc);
	uart_irq_callback_user_data_set(g4b_dfu_cdc, g4b_dfu_uart_isr, NULL);
	uart_irq_rx_enable(g4b_dfu_cdc);

	/* The 1200-baud path is wired via zmk_usb_cdc_line_coding_changed() above,
	 * driven from usb.c's usbd_msg callback - nothing to register here. */
	return 0;
}

SYS_INIT(g4b_dfu_trigger_init, APPLICATION, 99);

#endif /* DT_NODE_EXISTS(dfu_cdc) */
