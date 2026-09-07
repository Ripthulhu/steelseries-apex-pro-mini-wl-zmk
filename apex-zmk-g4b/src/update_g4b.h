/* SPDX-License-Identifier: MIT */
#ifndef UPDATE_G4B_H
#define UPDATE_G4B_H
#include <stdbool.h>
#include <zephyr/shell/shell.h>
int g4b_update_command(const struct shell *sh, size_t argc, char **argv);
int g4b_update_health(void);
bool g4b_update_busy(void);
int g4b_update_binary(uint32_t offset, const uint8_t *data, size_t length, uint32_t *received);
#endif
