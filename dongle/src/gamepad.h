/* SPDX-License-Identifier: MIT */
#ifndef RECEIVER_GAMEPAD_H
#define RECEIVER_GAMEPAD_H
#include <stdbool.h>
struct shell;
int receiver_gamepad_init(void);
void receiver_gamepad_poll(void);
void receiver_gamepad_release(void);
void receiver_gamepad_status(const struct shell *sh);
void receiver_usb_gamepad(bool enabled);
#endif
