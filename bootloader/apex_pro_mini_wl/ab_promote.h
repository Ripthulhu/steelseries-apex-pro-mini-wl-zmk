/* SPDX-License-Identifier: MIT */
#ifndef AB_PROMOTE_H_
#define AB_PROMOTE_H_

/* Restore a validated image from external NOR after the configured consecutive
 * boot-failure threshold. The descriptor, source CRC, and application-slot
 * bounds are checked before internal flash is erased. The function is a no-op
 * when rollback is not armed or validation fails.
 */
void ab_promote_check(void);

#endif /* AB_PROMOTE_H_ */
