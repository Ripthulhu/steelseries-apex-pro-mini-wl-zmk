/* SPDX-License-Identifier: MIT */
#ifndef APEX_SHELL_LINK_H
#define APEX_SHELL_LINK_H
#include "apex_stream.h"

#define APEX_SHELL_COMMAND_MAX 128
#define APEX_SHELL_OUTPUT_MAX 16384
#define APEX_SHELL_TIMEOUT_MS 15000
enum apex_shell_message { APEX_SHELL_COMMAND = 1, APEX_SHELL_COMMAND_END, APEX_SHELL_OUTPUT, APEX_SHELL_RESULT };

uint32_t apex_shell_generation(void);
void apex_shell_link_reset(void);
int apex_shell_link_pack(uint8_t *packet);
int apex_shell_link_receive(const uint8_t *packet, size_t length);
int apex_shell_send(uint32_t generation, const uint8_t *data, size_t length, int64_t deadline);
int apex_shell_read(uint32_t generation, uint8_t *data);
void apex_shell_rx_ready(void);
#endif
