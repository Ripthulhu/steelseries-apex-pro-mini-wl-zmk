/* SPDX-License-Identifier: MIT */
#ifndef APEX_RADIO_INPUT_H
#define APEX_RADIO_INPUT_H
#include "apex_input.h"
int apex_radio_queue_report(uint8_t type, const uint8_t *data, size_t length);
void apex_radio_input_select(bool selected);
bool apex_radio_input_connected(void);
void apex_radio_request_session(void);
/* Called from a thread before System OFF; resume starts a fresh session. */
bool apex_radio_pause(void);
void apex_radio_resume(void);
void apex_radio_delivery_notify(void);
uint32_t apex_radio_completed(uint32_t *completed_at);
/* Platform callbacks. Delivery returns 0 only once USB transfer completes. */
int apex_radio_deliver(const struct apex_input_frame *frame);
void apex_radio_release(void);
uint8_t apex_radio_host_leds(void);
void apex_radio_update_leds(uint8_t leds);
#endif
