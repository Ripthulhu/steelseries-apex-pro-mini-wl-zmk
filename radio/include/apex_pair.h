/* SPDX-License-Identifier: MIT */
#ifndef APEX_PAIR_H
#define APEX_PAIR_H
#include <stdint.h>
#include <stddef.h>
#include <zephyr/shell/shell.h>

#define APEX_BOND_SIZE 32
/* APB1, public pair ID (8), secret key (16), little-endian IEEE CRC32 (4). */
int apex_bond_valid(const uint8_t record[APEX_BOND_SIZE]);
int apex_bond_load(uint8_t record[APEX_BOND_SIZE]);
int apex_bond_store(const uint8_t record[APEX_BOND_SIZE]);
int apex_bond_delete(void);
int apex_pair_shell(const struct shell *sh, size_t argc, char **argv);
#endif
