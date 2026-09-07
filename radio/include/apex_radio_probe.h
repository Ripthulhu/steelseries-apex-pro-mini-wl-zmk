/* SPDX-License-Identifier: MIT */
#ifndef APEX_RADIO_PROBE_H
#define APEX_RADIO_PROBE_H
#include "apex_packet.h"
#include <zephyr/shell/shell.h>
void apex_radio_probe_start(enum apex_role role);
int apex_radio_probe_status(const struct shell *sh, size_t argc, char **argv);
int apex_radio_benchmark(const struct shell *sh, size_t argc, char **argv);
#endif
