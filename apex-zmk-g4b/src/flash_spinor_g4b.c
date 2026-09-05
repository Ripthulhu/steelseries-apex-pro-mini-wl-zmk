/* SPDX-License-Identifier: MIT
 *
 * Zephyr flash driver for the external FM25Q08A SPI-NOR (1 MiB) on the Apex Pro
 * Mini WL. It wraps the direct-register SPIM0 primitives in spinor_g4b.c so ZMK
 * can put its settings/NVS on the freed external chip.
 *
 * Why a custom driver rather than Zephyr's jedec,spi-nor: that driver pulls in
 * CONFIG_SPI + CONFIG_GPIO + CONFIG_PINCTRL, all of which this firmware's build
 * verifier forbids (pin writes are confined to pins_g4b.c). This driver keeps
 * every pin/bus access inside the sanctioned direct-register path and exposes
 * only the standard flash_driver_api on top.
 *
 * The bootloader is untouched: UF2/DFU still flashes the application into
 * INTERNAL flash. This chip is only ever driven by the running app, so settings
 * on it also survive a firmware reflash.
 */

#define DT_DRV_COMPAT apex_g4b_spinor

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/init.h>
#include <string.h>

#include "nor_layout_g4b.h"
#include "spinor_g4b.h"

#define NOR_SECTOR_SIZE G4B_NOR_SECTOR_SIZE
#define NOR_TOTAL_SIZE  G4B_NOR_TOTAL_SIZE

static int fs_read(const struct device *dev, off_t off, void *data, size_t len)
{
	ARG_UNUSED(dev);
	if (off < 0) {
		return -EINVAL;
	}
	return g4b_spinor_dev_read((uint32_t)off, data, (uint32_t)len);
}

static int fs_write(const struct device *dev, off_t off, const void *data,
		    size_t len)
{
	ARG_UNUSED(dev);
	if (off < 0) {
		return -EINVAL;
	}
	return g4b_spinor_dev_program((uint32_t)off, data, (uint32_t)len);
}

static int fs_erase(const struct device *dev, off_t off, size_t size)
{
	ARG_UNUSED(dev);
	if (off < 0) {
		return -EINVAL;
	}
	return g4b_spinor_dev_erase((uint32_t)off, (uint32_t)size);
}

static const struct flash_parameters *fs_get_parameters(const struct device *dev)
{
	ARG_UNUSED(dev);
	static const struct flash_parameters params = {
		.write_block_size = 1u,   /* SPI-NOR programs arbitrary byte counts */
		.erase_value = 0xFF,
	};
	return &params;
}

#if defined(CONFIG_FLASH_PAGE_LAYOUT)
static const struct flash_pages_layout fs_layout = {
	.pages_count = NOR_TOTAL_SIZE / NOR_SECTOR_SIZE, /* 256 sectors */
	.pages_size = NOR_SECTOR_SIZE,
};

static void fs_page_layout(const struct device *dev,
			   const struct flash_pages_layout **layout,
			   size_t *layout_size)
{
	ARG_UNUSED(dev);
	*layout = &fs_layout;
	*layout_size = 1u;
}
#endif /* CONFIG_FLASH_PAGE_LAYOUT */

static const struct flash_driver_api fs_api = {
	.read = fs_read,
	.write = fs_write,
	.erase = fs_erase,
	.get_parameters = fs_get_parameters,
#if defined(CONFIG_FLASH_PAGE_LAYOUT)
	.page_layout = fs_page_layout,
#endif
};

static int fs_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return g4b_spinor_dev_init();
}

DEVICE_DT_INST_DEFINE(0, fs_init, NULL, NULL, NULL, POST_KERNEL,
		      CONFIG_FLASH_INIT_PRIORITY, &fs_api);

/* One-time format of the external NVS partition.
 *
 * The chip currently holds a stale vendor record at 0x60000 (neither valid NVS
 * nor blank), which nvs_mount cannot cope with - it mounts a BLANK partition
 * cleanly but chokes on non-NVS bytes. Erase the partition to 0xFF exactly once,
 * guarded by a magic marker in a dedicated sector at 0x68000 (outside the
 * partition, addressed raw here rather than via DT):
 *   - marker present  -> already formatted; do nothing, so settings persist
 *                        across reboots and across app reflashes.
 *   - marker absent    -> erase the partition, then write the marker.
 *
 * This is a SYS_INIT, so it runs before main() and therefore before ZMK's
 * settings_subsys_init()/nvs_mount(), and before the g4b thread starts its boot
 * replay - the erase can never collide with the STM32 scanner. The SPIM0
 * primitives touch only registers/pins and need no driver to be up, and at
 * SYS_INIT time the g4b thread is not running, so the shared bus lock and
 * replay gate inside g4b_spinor_dev_* are uncontended no-ops. */
#if IS_ENABLED(CONFIG_APEX_G4B_SPINOR_NVS_PROVISION)
#define NVS_PART_BASE   G4B_NOR_NVS_ADDR
#define NVS_PART_SIZE   G4B_NOR_NVS_SIZE
#define NVS_MARK_ADDR   G4B_NOR_NVS_MARK_ADDR
#define NVS_MARK_MAGIC  "APXNVS01" /* 8 bytes */

static int fs_provision(void)
{
	uint8_t mark[8];

	if (g4b_spinor_dev_read(NVS_MARK_ADDR, mark, sizeof(mark)) == 0 &&
	    memcmp(mark, NVS_MARK_MAGIC, sizeof(mark)) == 0) {
		return 0; /* already provisioned - leave existing NVS intact */
	}

	/* Format the partition, then stamp the marker so this never repeats. On any
	 * failure the marker is not written, so the next boot retries rather than
	 * leaving a half-formatted partition marked done. */
	if (g4b_spinor_dev_erase(NVS_PART_BASE, NVS_PART_SIZE) != 0) {
		return 0;
	}
	if (g4b_spinor_dev_erase(NVS_MARK_ADDR, 0x1000u) != 0) {
		return 0;
	}
	(void)g4b_spinor_dev_program(NVS_MARK_ADDR, NVS_MARK_MAGIC, sizeof(NVS_MARK_MAGIC) - 1u);
	return 0;
}

SYS_INIT(fs_provision, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
#endif /* CONFIG_APEX_G4B_SPINOR_NVS_PROVISION */

/* One-shot end-to-end self-test through the Zephyr flash API (not just the raw
 * SPIM primitives): erase/write/read the dedicated scratch sector at 0x81000
 * and report over the evidence channel. It sits above the raw self-test sector
 * and below the reserved free-data area, clear of image B. */
#if IS_ENABLED(CONFIG_APEX_G4B_SPINOR_FLASHDEV_SELFTEST)
#include <zephyr/kernel.h>
#include "evidence_g4b.h"

#define FS_SCRATCH G4B_NOR_FLASHDEV_TEST_ADDR

static void fs_selftest(void *a, void *b, void *c)
{
	const struct device *dev = DEVICE_DT_INST_GET(0);
	static uint8_t wr[32], rd[32];
	uint8_t line[40];
	uint32_t n = 0u, match = 0u;
	int e, w;
	const char *t;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	k_msleep(3000); /* let the USB CDC enumerate before sending the report */

	for (uint32_t i = 0; i < 32u; i++) {
		wr[i] = (uint8_t)(0x5Au ^ i);
		rd[i] = 0u;
	}
	e = flash_erase(dev, FS_SCRATCH, 0x1000u);
	w = flash_write(dev, FS_SCRATCH, wr, 32u);
	(void)flash_read(dev, FS_SCRATCH, rd, 32u);
	for (uint32_t i = 0; i < 32u; i++) {
		if (rd[i] == wr[i]) {
			match++;
		}
	}
	(void)flash_erase(dev, FS_SCRATCH, 0x1000u); /* leave blank */

	t = "APXFDEV erase="; while (*t) line[n++] = (uint8_t)*t++;
	line[n++] = (e == 0) ? '1' : '0';
	t = " write="; while (*t) line[n++] = (uint8_t)*t++;
	line[n++] = (w == 0) ? '1' : '0';
	t = " match="; while (*t) line[n++] = (uint8_t)*t++;
	if (match >= 10u) { line[n++] = (uint8_t)('0' + match / 10u); }
	line[n++] = (uint8_t)('0' + match % 10u);
	line[n++] = '\r';
	line[n++] = '\n';
	g4b_evidence_emit_text(line, n);
}

K_THREAD_DEFINE(g4b_fs_selftest_tid, 1024, fs_selftest, NULL, NULL, NULL,
		K_PRIO_PREEMPT(14), 0, 0);
#endif
