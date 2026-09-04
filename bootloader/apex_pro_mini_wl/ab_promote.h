/* SPDX-License-Identifier: MIT */
#ifndef AB_PROMOTE_H_
#define AB_PROMOTE_H_

#include <stdint.h>

/* Restore a validated image from external NOR after the configured consecutive
 * boot-failure threshold. The descriptor, source CRC, and application-slot
 * bounds are checked before internal flash is erased. The function is a no-op
 * when rollback is not armed or validation fails.
 */
void ab_promote_check(void);

/* Append a short recovery report to INFO_UF2.TXT. ab_promote_check() captures
 * the values before the bootloader starts USB, so this does not reopen the
 * external flash while the mass-storage device is running. */
void ab_promote_info_append(char *text, uint32_t capacity);

/* Return the newest CRC-valid crash record captured during boot. The returned
 * pointer remains valid while the bootloader is running. */
uint32_t ab_promote_crash_file(const uint8_t **data);

#endif /* AB_PROMOTE_H_ */
