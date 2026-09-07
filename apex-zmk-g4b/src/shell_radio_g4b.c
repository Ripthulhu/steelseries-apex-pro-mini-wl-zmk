/* SPDX-License-Identifier: MIT */
#include "apex_shell_link.h"
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>
#if IS_ENABLED(CONFIG_APEX_G4B_WIRELESS_UPDATE)
#include "update_g4b.h"
#endif

static shell_transport_handler_t event_handler;
static void *event_context;
static uint32_t command_generation, output_length;
static int64_t deadline;
static int output_error;
static atomic_t executing;

bool g4b_radio_shell_busy(void) { return atomic_get(&executing) != 0; }

void apex_shell_rx_ready(void)
{
    if (event_handler) event_handler(SHELL_TRANSPORT_EVT_RX_RDY, event_context);
}

static int transport_init(const struct shell_transport *transport, const void *config,
                          shell_transport_handler_t handler, void *context)
{
    ARG_UNUSED(transport); ARG_UNUSED(config);
    event_context = context;
    event_handler = handler;
    return 0;
}
static int transport_uninit(const struct shell_transport *transport)
{
    ARG_UNUSED(transport);
    event_handler = NULL;
    return 0;
}
static int transport_enable(const struct shell_transport *transport, bool blocking)
{
    ARG_UNUSED(transport); ARG_UNUSED(blocking);
    return 0;
}

static int transport_write(const struct shell_transport *transport, const void *data,
                           size_t length, size_t *written)
{
    ARG_UNUSED(transport);
    const uint8_t *bytes = data;
    size_t remaining = length;
    if (atomic_get(&executing) && !output_error) {
        if (length > APEX_SHELL_OUTPUT_MAX - output_length) output_error = -EOVERFLOW;
        else output_length += length;
        while (remaining && !output_error) {
            uint8_t frame[APEX_STREAM_DATA_MAX] = { APEX_SHELL_OUTPUT };
            size_t chunk = MIN(remaining, sizeof(frame) - 1);
            memcpy(frame + 1, bytes, chunk);
            output_error = apex_shell_send(command_generation, frame, chunk + 1, deadline);
            remaining -= chunk;
            bytes += chunk;
        }
    }
    /* An expired request discards further output rather than blocking a shell
     * handler indefinitely. Its result reports truncation when still connected. */
    *written = length;
    if (event_handler) event_handler(SHELL_TRANSPORT_EVT_TX_RDY, event_context);
    return 0;
}

static int transport_read(const struct shell_transport *transport, void *data,
                          size_t length, size_t *read);
static const struct shell_transport_api api = {
    .init = transport_init, .uninit = transport_uninit, .enable = transport_enable,
    .write = transport_write, .read = transport_read,
};
static struct shell_transport transport = { .api = &api };
SHELL_DEFINE(g4b_radio_shell, "", &transport, 4, 0, SHELL_FLAG_OLF_CRLF);

static int transport_read(const struct shell_transport *iface, void *data,
                          size_t length, size_t *read)
{
    ARG_UNUSED(iface); ARG_UNUSED(data); ARG_UNUSED(length);
    static char command[APEX_SHELL_COMMAND_MAX];
    static size_t used;
    static uint32_t assembling_generation;
    static int command_error;
    *read = 0;
    uint32_t current = apex_shell_generation();
    if (current != assembling_generation) {
        used = 0;
        command_error = 0;
        assembling_generation = current;
    }
    uint8_t frame[APEX_STREAM_DATA_MAX];
    int n = apex_shell_read(current, frame);
    if (n <= 0) return 0;
    if (frame[0] == APEX_UPDATE_DATA) {
        uint32_t received = 0;
        int rc = -ENOTSUP;
        used = 0;
        command_error = 0;
#if IS_ENABLED(CONFIG_APEX_G4B_WIRELESS_UPDATE)
        if (n > 5) {
            apex_shell_bulk_touch();
            rc = g4b_update_binary(sys_get_le32(frame + 1), frame + 5, n - 5, &received);
        }
#endif
        uint8_t reply[9] = { APEX_UPDATE_REPLY };
        sys_put_le32((uint32_t)rc, reply + 1);
        sys_put_le32(received, reply + 5);
        (void)apex_shell_send(current, reply, sizeof(reply), k_uptime_get() + 2000);
        return 0;
    }
    if (frame[0] != APEX_SHELL_COMMAND && frame[0] != APEX_SHELL_COMMAND_END) return 0;
    if ((size_t)n - 1 >= sizeof(command) - used) command_error = -E2BIG;
    for (int i = 1; i < n && !command_error; i++) {
        if (frame[i] < 32 || frame[i] > 126) command_error = -EINVAL;
        else command[used++] = frame[i];
    }
    if (frame[0] != APEX_SHELL_COMMAND_END) return 0;
    command[used] = '\0';
    if (current != apex_shell_generation()) return 0;
    if (strcmp(command, "apex") && strncmp(command, "apex ", 5)) command_error = -EPERM;
    command_generation = current;
    output_length = 0;
    output_error = 0;
    deadline = k_uptime_get() + APEX_SHELL_TIMEOUT_MS;
    atomic_set(&executing, 1);
    /* read() runs on this shell's thread before its line parser. Execute one
     * complete request there, returning no characters to the parser. This
     * avoids an extra command thread and keeps execution off the radio thread. */
    int result = command_error ? command_error : shell_execute_cmd(&g4b_radio_shell, command);
    if (output_error) result = output_error;
    uint8_t reply[5] = { APEX_SHELL_RESULT };
    sys_put_le32((uint32_t)result, reply + 1);
    (void)apex_shell_send(current, reply, sizeof(reply), deadline);
    atomic_set(&executing, 0);
    used = 0;
    command_error = 0;
    return 0;
}

static int radio_shell_init(void)
{
    BUILD_ASSERT(CONFIG_SHELL_CMD_BUFF_SIZE >= APEX_SHELL_COMMAND_MAX);
    struct shell_backend_config_flags flags = SHELL_DEFAULT_BACKEND_CONFIG_FLAGS;
    flags.echo = false;
    flags.use_colors = false;
    flags.use_vt100 = false;
    return shell_init(&g4b_radio_shell, NULL, flags, false, 0);
}
SYS_INIT(radio_shell_init, APPLICATION, 97);
