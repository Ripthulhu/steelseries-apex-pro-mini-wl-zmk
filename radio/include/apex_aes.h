/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
/* The caller must have exclusive ownership of ECB for the rest of this boot. */
void apex_aes_enable(void);
uint32_t apex_aes_blocks(void);
uint32_t apex_aes_fallbacks(void);
