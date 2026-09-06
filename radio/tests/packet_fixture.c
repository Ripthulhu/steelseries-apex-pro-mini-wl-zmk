/* SPDX-License-Identifier: MIT */
#include "apex_packet.h"
#include "apex_connection.h"

size_t fixture_session_size(void) { return sizeof(struct apex_session); }
void fixture_exhaust_counter(struct apex_session *s) { s->tx_counter = UINT32_MAX; }
size_t fixture_connection_size(void) { return sizeof(struct apex_connection); }
