/* SPDX-License-Identifier: MIT */
#include "apex_shell_link.h"
#include "apex_radio_input.h"
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>

int receiver_keyboard_command(const struct shell *sh, size_t argc, char **argv)
{
    if (argc == 1) {
        shell_print(sh, "Usage: keyboard <command> [arguments]");
        shell_print(sh, "  battery        Battery and charging status");
        shell_print(sh, "  radio          Connection and delivery counters");
        shell_print(sh, "  gamepad        Analog controller status (on/off to change)");
        shell_print(sh, "  info           Keyboard status summary");
        shell_print(sh, "Other commands use the same names as the keyboard's apex shell.");
        return 0;
    }
    char command[APEX_SHELL_COMMAND_MAX] = "apex";
    size_t used = strlen(command);
    for (size_t i = 1; i < argc; i++) {
        const char *arg = i == 1 && !strcmp(argv[i], "radio") ? "radio_test" : argv[i];
        size_t n = strlen(arg);
        if (used + n + 1 >= sizeof(command)) {
            shell_error(sh, "Keyboard command is too long.");
            return -E2BIG;
        }
        command[used++] = ' ';
        memcpy(command + used, arg, n);
        used += n;
    }
    command[used] = '\0';
    if (!apex_radio_input_connected()) {
        shell_error(sh, "Keyboard is not connected. Wake it or select dongle mode.");
        return -ENOTCONN;
    }
    uint32_t generation = apex_shell_generation();
    int64_t deadline = k_uptime_get() + APEX_SHELL_TIMEOUT_MS;
    int rc = 0;
    for (size_t offset = 0; offset < used;) {
        uint8_t frame[APEX_STREAM_DATA_MAX];
        size_t n = MIN(used - offset, sizeof(frame) - 1);
        frame[0] = offset + n == used ? APEX_SHELL_COMMAND_END : APEX_SHELL_COMMAND;
        memcpy(frame + 1, command + offset, n);
        rc = apex_shell_send(generation, frame, n + 1, deadline);
        if (rc) goto failed;
        offset += n;
    }
    shell_print(sh, "[keyboard]");
    while (k_uptime_get() < deadline) {
        uint8_t frame[APEX_STREAM_DATA_MAX];
        int n = apex_shell_read(generation, frame);
        if (n < 0) { rc = n; goto failed; }
        if (!n) { k_sleep(K_MSEC(1)); continue; }
        if (frame[0] == APEX_SHELL_OUTPUT) {
            shell_fprintf(sh, SHELL_NORMAL, "%.*s", n - 1, (const char *)frame + 1);
        } else if (frame[0] == APEX_SHELL_RESULT && n == 5) {
            rc = (int32_t)sys_get_le32(frame + 1);
            shell_print(sh, "\n[keyboard result: %d]", rc);
            return rc;
        } else { rc = -EPROTO; goto failed; }
    }
    rc = -ETIMEDOUT;
failed:
    apex_radio_request_session();
    shell_error(sh, "Keyboard reply incomplete (%d). The command may have run; it was not retried.", rc);
    return rc;
}
