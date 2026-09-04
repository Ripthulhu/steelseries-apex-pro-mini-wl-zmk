/* SPDX-License-Identifier: MIT
 *
 * LittleFS storage on the external FM25Q08A SPI NOR. The filesystem currently
 * has no application consumer; it reserves structured storage for future data.
 * The 1 MiB chip layout around this partition is:
 *   0x60000  0x8000   ext_storage_partition - ZMK NVS (bonds, settings)
 *   0x68000  0x1000   provision marker sector (flash_spinor_g4b.c)
 *   0x69000  0x1000   A/B descriptor
 *   0x6a000  0x1000   A/B boot-health tally
 * LittleFS occupies 0x6b000-0x80000. Access goes through the shared
 * apex,g4b-spinor device and its bus lock. It mounts during application
 * initialization, before the Nordic scan thread starts. A blank partition is
 * formatted automatically; a mount failure is reported but does not stop boot.
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>

#if IS_ENABLED(CONFIG_APEX_G4B_UART_EVIDENCE)
#include <zephyr/kernel.h>
#include "evidence_g4b.h"
#endif

#define LFS_PART_ID    FIXED_PARTITION_ID(littlefs_partition)
#define LFS_MNT_POINT  "/lfs"

/* Static config: read/prog/cache/lookahead buffers are declared here rather than
 * heap-allocated, so mounting needs no k_heap and cannot fail for want of one. */
FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(g4b_lfs_cfg);

static struct fs_mount_t g4b_lfs_mnt = {
	.type = FS_LITTLEFS,
	.fs_data = &g4b_lfs_cfg,
	.storage_dev = (void *)LFS_PART_ID,
	.mnt_point = LFS_MNT_POINT,
	.flags = 0, /* NO_FORMAT left clear: auto-format a blank/stale tail once */
};

/* Result of the automount, kept so the delayed reporter (below) can emit it once
 * the USB CDC is up - the SYS_INIT itself runs long before enumeration. */
static int g4b_lfs_rc = -EAGAIN;

static int g4b_littlefs_mount(void)
{
	/* The flash device takes g4b_extbus and waits for scanner replay. This runs
	 * before the g4b thread starts. */
	g4b_lfs_rc = fs_mount(&g4b_lfs_mnt);

	/* No current feature depends on this filesystem. Report the error later. */
	return 0;
}

SYS_INIT(g4b_littlefs_mount, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#if IS_ENABLED(CONFIG_APEX_G4B_UART_EVIDENCE)
/* Best-effort one-line report, in the same ASCII shape as the flashdev
 * self-test ("APXLFS mount=<rc> blocks=<n> bfree=<n>"). Delayed a few seconds so
 * the USB CDC mirror has enumerated; the P0.10 UART copy goes out regardless.
 * Runs on its own low-priority thread so it never sits in the boot path. The
 * text buffer is on the stack (RAM) because the evidence UART uses EasyDMA and
 * cannot transmit from flash. */
static uint32_t lfs_put_str(uint8_t *buf, uint32_t n, const char *s)
{
	while (*s != '\0') {
		buf[n++] = (uint8_t)*s++;
	}
	return n;
}

static uint32_t lfs_put_u32(uint8_t *buf, uint32_t n, uint32_t v)
{
	uint8_t tmp[10];
	uint32_t t = 0u;

	if (v == 0u) {
		buf[n++] = '0';
		return n;
	}
	while (v != 0u) {
		tmp[t++] = (uint8_t)('0' + (v % 10u));
		v /= 10u;
	}
	while (t != 0u) {
		buf[n++] = tmp[--t];
	}
	return n;
}

static uint32_t lfs_put_int(uint8_t *buf, uint32_t n, int32_t v)
{
	if (v < 0) {
		buf[n++] = '-';
		return lfs_put_u32(buf, n, (uint32_t)(-(int64_t)v));
	}
	return lfs_put_u32(buf, n, (uint32_t)v);
}

static void g4b_littlefs_report(void *a, void *b, void *c)
{
	uint8_t line[64];
	uint32_t n = 0u;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	k_msleep(3000); /* let the USB CDC enumerate before sending the report */

	n = lfs_put_str(line, n, "APXLFS mount=");
	n = lfs_put_int(line, n, g4b_lfs_rc);

	if (g4b_lfs_rc == 0) {
		struct fs_statvfs st;

		if (fs_statvfs(LFS_MNT_POINT, &st) == 0) {
			n = lfs_put_str(line, n, " blocks=");
			n = lfs_put_u32(line, n, (uint32_t)st.f_blocks);
			n = lfs_put_str(line, n, " bfree=");
			n = lfs_put_u32(line, n, (uint32_t)st.f_bfree);
		}
	}

	line[n++] = '\r';
	line[n++] = '\n';
	g4b_evidence_emit_text(line, n);
}

K_THREAD_DEFINE(g4b_lfs_report_tid, 1024, g4b_littlefs_report, NULL, NULL, NULL,
		K_PRIO_PREEMPT(14), 0, 0);
#endif /* CONFIG_APEX_G4B_UART_EVIDENCE */
