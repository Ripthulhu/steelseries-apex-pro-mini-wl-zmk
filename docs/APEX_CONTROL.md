# Apex control, shell, and Studio RPC

This documents the `apex_control` system added to the g4b firmware: a shared
control/telemetry API with three frontends — an interactive **UART shell**, the
**ZMK Studio `apex` RPC subsystem**, and the existing keymap behaviors. It also
covers the debug build, flashing, and the reset/power safety model.

> Written 2026-09-05. If you touch the Studio RPC, read **§8** first — several of
> its pieces live in the ZMK *workspace* (`work/zmk-upstream`), not this repo, and
> are easy to lose.

## 1. Architecture

```
   [ UART shell ]      [ Studio apex RPC ]      [ keymap behaviors ]
            \                  |                        /
             +--------> apex_control API <-------------+
                        (src/apex_control_g4b.{c,h})
                                |
      +-------------------------+---------------------------+
      v                         v                           v
 twi_g4b (BQ25895)   link_g4b (STM32 scanner, RGB)   rgb_fx_g4b / mode_g4b
```

`apex_control` (`src/apex_control_g4b.{c,h}`) is a thin, thread-safe wrapper over
the existing subsystem getters/setters. Every frontend calls it, so a value set
from the shell, Studio, or a Fn key goes through identical code and persists the
same way. Guarded by `CONFIG_APEX_G4B_SHELL` today (see §8 for the release path).

## 2. The debug build

```sh
python repo/apex-zmk-g4b/build_g4b.py --stage 3 --usb-studio --shell
```

`--shell` (added to `build_g4b.py`):
- appends `g4b_shell.conf` (enables `CONFIG_APEX_G4B_SHELL`, `CONFIG_SHELL`,
  `CONFIG_LOG`, the SWD-replacement tool shells, telemetry, and a bigger Studio
  RPC TX buffer);
- adds `-DAPEX_G4B_SHELL_DTS` so `g4b_usb.overlay` instantiates a **third USB
  CDC-ACM** (`shell_cdc`) as `zephyr,shell-uart`;
- implies `--plain-image` and **skips the release audit** (`verify_g4b_plain.py`)
  because SHELL/LOG are intentionally present.

Output UF2: `work/artifacts-repo-apex-zmk-g4b/apex-zmk-g4b.plain.uf2`.

The board then enumerates `1d50:615e` with three CDC ports:
| Port (example) | Interface | Use |
|---|---|---|
| COM3 | MI_00 | Studio RPC (`studio_rpc_usb_uart`) |
| COM12 | MI_02 | DFU trigger (`dfu_cdc`, 1200-baud / `APEXDFU!`) |
| COM15 | MI_04 | **the `apex` shell** (`shell_cdc`) — open at 115200 |

## 3. Flashing (no SWD needed)

DFU over USB, two ways in:
- **From the shell:** `apex dfu` (writes `GPREGRET=0x57`, resets into APEXBOOT).
- **From the host:** `tools/dfu_flash.py <uf2> COM12` — 1200-baud touch on the
  dfu_cdc port, waits for the `APEXBOOT` drive, copies the UF2.

Gotcha: the UF2 bootloader flashes and unmounts the drive the instant the file
lands, so use `shutil.copyfile` (not `copy`) — the follow-up chmod throws
`WinError 433`, which is harmless (the bytes already triggered the flash).

## 4. `apex` shell command reference

Open the shell port (COM15) at 115200. Every subcommand has `-h`; typing `apex`
lists them.

**Logging is quiet by default** so the prompt is usable. Logs are still compiled
in — turn them on with `log enable <err|wrn|inf|dbg> [module]` (e.g.
`log enable dbg zmk`) or `log go`; `log halt` / `log disable [module]` to quiet
again. A boot-time runtime filter (`apex_shell_g4b.c`) drops all sources to error;
the once-per-second ZMK endpoint spam was removed at the source by making
`mode_g4b.c`'s `mode_gate_work` edge-triggered (it no longer re-selects the
transport / re-stops advertising every tick — a genuine efficiency fix that
applies to all builds). ZMK's own `CONFIG_ZMK_LOG_LEVEL` is a promptless int
hardcoded to 4 (DBG) and can't be lowered from a fragment, so don't try — quiet it
at runtime instead.

> **BLE advertising is owned entirely by ZMK — never stop it from our code.**
> `mode_gate_work` used to call `bt_le_adv_stop()` **and** force
> `bt_conn_disconnect()` when entering a non-BT mode. That races the softdevice-less
> controller: ZMK's `update_advertising()` (`app/src/ble.c`) ignores the preferred
> transport and re-runs on every disconnect, so a stop/disconnect issued behind its
> back interleaves with ZMK re-opening advertising and asserts in the LLL radio-event
> prepare (`lll_adv.c` `prepare_cb`) — a kernel oops (`reason 3`, e.g. PC `0x2f76`)
> that **self-reboots the board on a mode switch**. The board looked alive but keys
> were dead until it faulted and re-ran the clean boot bring-up. Fix: `mode_gate_work`
> now only calls `zmk_endpoint_set_preferred_transport()` and lets ZMK own the radio;
> a live BLE link simply stops being the report sink, which is what we want anyway.
> Verified by 8 forced `apex mode` transitions with no new `apex crash` record.

### Status / telemetry
| Command | Shows |
|---|---|
| `apex info` | one-shot dashboard: battery, charge, actuation, RT, RGB, die temp, uptime, keypresses, active HID transport |
| `apex mon [count] [sec]` | live status line each interval (blocks the shell) |
| `apex battery` | mV / % / mA / charging / power-good / raw status+fault |
| `apex temp` | nRF die temp (°C) + **battery pack temp** (confirmed real NTC on the BQ TS pin; 128-entry TSPCT→°C table in `twi_g4b.c`, fed by a fresh one-shot conversion) |
| `apex telemetry` | uptime, USB conn state, temps |
| `apex stats` | total key presses, uptime, presses/min |
| `apex link` | scanner health: **boot handshake** (one-shot 59/59 frames — proves the link came up, frozen after handover), plus **live** counters from the permanent keyboard loop — `key events` (accepted key state changes, climbs as you type) and `idle keepalives` (0xA0 polls between events). *The old "ingest calls/ok" fields were boot-probe snapshots that froze at 0 after handover and falsely looked like keys weren't ingesting — watch the live counters instead.* |
| `apex depth` | live per-key Hall travel % (forces a 0xA2 sample; hold keys) |
| `apex halldump` | raw Hall value of all 70 scan slots, unfiltered (in-loop 0xA2, no desync) — for scanner RE: read at rest vs. with a key held and diff |
| `apex ble` | active BLE profile + per-profile connected/open |
| `apex bqreg <r>` / `apex bqdump` | one / all BQ25895 registers (read-only) |

### Control
| Command | Effect |
|---|---|
| `apex charge [limit 80\|100 \| vreg <mV> \| current <mA> \| on\|off]` | live BQ25895 config. **VREG hard-clamped ≤4400 mV** (pack max). 80=4096 mV, 100=4352 mV. Persisted to NVS. |
| `apex act [<mm>]` | actuation point, snaps to the 6-point ladder (1.0–3.0 mm) |
| `apex rt [on <mm> \| off]` | rapid trigger |
| `apex rgb [<0..11> \| bright up\|down]` | effect select / underglow brightness |
| `apex mode [bt\|usb\|dongle\|auto]` | **force the transport regardless of the physical switch**; persisted (NVS + NOINIT). USB↔BT instant; dongle owns the 2.4 GHz radio (radio builds reboot to swap stacks; override survives the reset). |

### Debug (SWD replacement)
`apex reginfo` (RESETREAS/GPREGRET/VTOR/UICR/FICR), `apex uicr` (hexdump),
`apex crash` (decoded last coredump), plus the Zephyr shells enabled here:
`devmem` (peek/poke memory+registers), `flash` (read/erase/write int+ext flash),
`hwinfo`, `kernel` (threads/stacks/uptime/reboot), `thread_analyzer`, `log`,
`device list`, `settings`.

**`apex scanraw <hex> [hex...]`** — send a raw 64-byte frame to the STM32 scanner
and print the reply. The single debug lever for the scanner-link protocol: probe
any opcode against the real chip instead of guessing. The exchange runs on the g4b
thread at its single-writer-safe point (like the reset requests). Examples:
`apex scanraw 90` (version → ASCII `33 2e 32 34 2e 31` = "3.24.1"),
`apex scanraw 20` (scanner state), `apex scanraw a1` (key bitmap),
`apex scanraw a0` (travel/heartbeat), `apex scanraw a2 08 00` (8 per-key Hall
samples from index 0). The destructive opcodes **`0x01` (reset) / `0x02` (power) /
`0x32` (flash write) are refused**. A raw frame injected outside the scan loop's
own rhythm (an `0xA2` drops ATTN, an `0xA1` consumes an event) would otherwise
strand the scanner's event state and stop keys, so **every `scanraw` automatically
re-runs the full boot bring-up afterward** (~0.5 s) to re-arm it — verified on
hardware that keys keep working even after firing `0xA2`. See
[PROTOCOL.md](PROTOCOL.md) for the opcode map.

### Reset / power — see §5
`apex reset <nrf|dfu|stm32|usb|rgb|charger|ble>`, `apex power <rgb|charge> <on|off>`.

## 5. Reset / power safety model

Component resets that touch **g4b-owned pins/SPIM** (STM32 enable, RGB rail, USB
rail) must run on the g4b thread, not the shell thread (single-writer invariant).
So the shell only raises a request flag (`g4b_request_*` in `link_g4b.c`) and the
g4b keyboard loop services it at its ATTN-low safe point
(`s3_service_shell_requests()`).

Rules baked in:
- **The USB rail (P0.25) is only ever *pulsed* (150 ms low → high) and always
  restored in the same service call.** Never leave it down — that would kill USB
  (and the shell) until a power cycle. `apex reset usb` drops the host connection
  briefly and it re-enumerates.
- **STM32 reset is a 250 ms power drop** (`g4b_request_stm32_reset`), not a short
  glitch — a short pulse leaves the scanner in a bad state and the keyboard stops
  typing. After the drop, `s3_cfg_dirty` re-sends actuation/RT/fx (the STM32 lost
  its RAM config). If it doesn't re-sync, `apex reboot`.
- RGB rail can stay off (`apex power rgb off`) — LEDs only.
- Charger "reset" = re-apply the safe config (`g4b_bq_configure_charge`), never a
  raw REG14 reset (would drop the 4.096 V cap).

All targets verified to recover: usb reset re-enumerated with the nRF up; nrf/dfu
rebooted and recovered; stm32/rgb/charger/ble healthy.

## 6. NVS persistence

`apex_control` registers a settings handler on subtree **`apxc`** storing
`{vreg_mv, mode_override}`, applied in the settings `commit` at boot. So a forced
charge limit and a forced mode survive a power cycle. Debounced 5 s. The mode
override is *also* kept in NOINIT RAM by `mode_g4b` for the warm-reset radio-owner
case (NVS adds cold-boot persistence).

## 7. `apex_control` API (`src/apex_control_g4b.h`)

Battery: `apex_battery_read`. Charge: `apex_charge_get/set_preset/set_vreg_mv/
set_current_ma/set_enabled` (VREG clamped in firmware). Actuation/RT:
`apex_actuation_get/set_tenths`, `apex_rapid_trigger_*`. RGB: `apex_rgb_effect_*`.
Telemetry: `apex_telemetry_read` (incl. die temp — reads NRF_TEMP directly, masking
the RC-LFCLK calibration's DATARDY interrupt so its ISR can't steal the sample).
Stats: `apex_stats_keypresses`, `apex_stats_heatmap` (per-position counts),
`apex_stats_heatmap_reset`. Mode: `apex_mode_set` (persisting wrapper).

## 8. ZMK Studio `apex` RPC subsystem

The heatmap (and future GUI panels) reach the Studio host over a custom `apex`
protobuf subsystem alongside core/keymap/behaviors. **Several pieces live in the
workspace, not this repo:**

| Where | Change |
|---|---|
| `work/zmk-upstream/modules/msgs/zmk-studio-messages/proto/zmk/apex.proto` | NEW — apex Request/Response + `Heatmap` (usage heatmap) + `Status` (battery mV/%/charging, battery °C, die °C). Requests: `apex_get_heatmap`, `apex_reset_heatmap`, `apex_get_status`. |
| …/`proto/zmk/apex.options.in` | NEW — `zmk.apex.Heatmap.counts max_count:128` (nanopb) |
| …/`proto/zmk/studio.proto` | added `import "apex.proto"` + `zmk.apex.Request apex = 6;` (Request) + `zmk.apex.Response apex = 6;` (RequestResponse) |
| `work/zmk-upstream/app/CMakeLists.txt` | added `apex.proto` to the `nanopb_generate_cpp(...)` list (~line 128) |
| `repo/apex-zmk-g4b/src/studio_apex_g4b.c` | NEW — `ZMK_RPC_SUBSYSTEM(apex)` + `apex_get_heatmap` / `apex_reset_heatmap` handlers |
| `repo/apex-zmk-g4b/CMakeLists.txt` | compiles `studio_apex_g4b.c` + adds `${CMAKE_BINARY_DIR}` so the module sees the generated `proto/zmk/*.pb.h` |
| `repo/apex-zmk-g4b/g4b_shell.conf` | `CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE=1024` (stock 64 truncates the 128-count response) |

Because the proto module + `app/CMakeLists.txt` are west-managed, re-running
`setup_workspace.py` may revert them — reapply from this table (a build recipe
patch is the proper long-term fix, TODO).

**Adding a request:** name the proto field `apex_<verb>` (the ZMK handler macro
uses the field name as the C function symbol, so keep it globally unique), add a
`static zmk_studio_Response apex_<verb>(const zmk_studio_Request*)` handler, and a
`ZMK_RPC_SUBSYSTEM_HANDLER(apex, apex_<verb>, ZMK_STUDIO_RPC_HANDLER_UNSECURED)`.

**Testing raw (no client):** the RPC framing is SOF `0xAB` / ESC `0xAC` / EOF
`0xAD` byte-stuffing around a protobuf `Request`. `tools/apex_rpc_test.py` sends
`apex_get_heatmap` to COM3 and decodes the `Heatmap`. Verified: 144-byte response,
128 per-key counts.

**Client + UI (TODO):** the `@zmkfirmware/zmk-studio-ts-client` npm package is
prebuilt and doesn't know `apex`; regenerate it from the extended proto (ts-proto)
as a local package in `apex-zmk-studio/`, then build a heatmap panel that polls
`apex_get_heatmap` and colours the physical layout Studio already renders.

## 9. Release build

Everything above is gated on `CONFIG_APEX_G4B_SHELL` (debug). To ship the Studio
panels on the normal firmware, move `apex_control_g4b.c` + `studio_apex_g4b.c` +
the RPC buffer size to a config that's on in the release build (`CONFIG_ZMK_STUDIO`)
and make it pass `verify_g4b.py` / `verify_g4b_plain.py`. The shell itself stays
debug-only.
