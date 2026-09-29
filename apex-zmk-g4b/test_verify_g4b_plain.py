"""Test audit guards; optionally pass verify_g4b_plain.py args to audit a build too."""
import argparse
import contextlib
import io
from pathlib import Path
import sys
from unittest.mock import patch

import verify_g4b_plain as audit

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import build_release


def reject_source_change(filename, old, new, checker):
    target = ROOT / "apex-zmk-g4b" / "src" / filename
    source = target.read_text()
    assert old in source
    read_text = Path.read_text
    with patch.object(Path, "read_text", lambda path, *a, **kw:
                      source.replace(old, new) if path == target
                      else read_text(path, *a, **kw)):
        failures = []
        checker(failures)
    assert failures, f"audit accepted {filename}: {old} -> {new}"


for checker in (audit.check_gpio_ownership, audit.check_ab_health_policy):
    failures = []
    checker(failures)
    assert not failures, failures

for old, new in (
    ("!g4b_mode_is_wireless()", "g4b_mode_get() != G4B_MODE_BT"),
    ("return !usb_required || zmk_usb_is_hid_ready();", "return true;"),
    ("r == G4B_SPIM_OK && ab_output_ready()", "r == G4B_SPIM_OK"),
    ("#define G4B_AB_HEALTH_FRAMES 8u", "#define G4B_AB_HEALTH_FRAMES 1u"),
    ("frames = 0u;", "frames = 1u;"),
):
    reject_source_change("link_g4b.c", old, new, audit.check_ab_health_policy)
reject_source_change("mode_g4b.h", " || mode == G4B_MODE_DONGLE", "",
                     audit.check_ab_health_policy)
reject_source_change("sleep_g4b.c",
                     "g4b_pin_cnf_read(G4B_PORT0, (enum g4b_pin)G4B_SLEEP_MODE_PIN)",
                     "NRF_P0->PIN_CNF[3]", audit.check_gpio_ownership)

for fields in ("00001000 T", "00001000 00000020 T"):
    for symbol in ("g4b_usb_watch_thread", "g4b_ab_status_tid", "g4b_coredump_emit_tid"):
        failures = []
        audit.check_disabled_diagnostics({}, f"{fields} {symbol}\n", failures)
        assert failures, (fields, symbol)
        failures = []
        audit.check_disabled_diagnostics(
            {"APEX_G4B_USB_KICK": "y", "APEX_G4B_UART_EMIT": "y"},
            f"{fields} {symbol}\n", failures)
        assert not failures, failures

# Exercise the actual release entry point with external work mocked: the full
# audit must run even for --skip-bootloader, and its failure must stop packaging.
args = argparse.Namespace(work_root=ROOT.parent / "work", extra_conf=[],
                          dongle=False, bootloader_only=False,
                          installer_bootloader=False, skip_bootloader=True)
with patch.multiple(build_release,
                    parse_args=lambda: args, venv_bin=lambda path: path,
                    executable=lambda path, name: path / name,
                    build_app=lambda *a: ROOT / "unused-test-artifacts",
                    verify_release_config=lambda *a: None):
    for reject in (False, True):
        commands = []

        def run(command, **kwargs):
            commands.append([str(part) for part in command])
            if reject and any(Path(str(part)).name == "verify_g4b_plain.py"
                              for part in command):
                raise RuntimeError("audit failed")

        with patch.object(build_release, "run", run), contextlib.redirect_stdout(io.StringIO()):
            try:
                build_release.main()
            except RuntimeError as error:
                assert reject and str(error) == "audit failed"
            else:
                assert not reject
        checks = [Path(command[1]).name for command in commands]
        assert "verify_g4b_plain.py" in checks
        if reject:
            assert "verify_final_uf2.py" not in checks
        else:
            assert checks.index("verify_g4b_plain.py") < checks.index("verify_final_uf2.py")

if len(sys.argv) > 1:
    with contextlib.redirect_stdout(io.StringIO()) as output:
        assert audit.main() == 0, output.getvalue()
    read_config = audit.read_config
    with patch.object(audit, "read_config", lambda path:
                      dict(read_config(path), APEX_G4B_STM32_LONG_IDLE_SCAN_PERIOD_MS="255")), \
            contextlib.redirect_stdout(io.StringIO()) as output:
        assert audit.main() == 1, "audit accepted the retired 255 ms idle period"
        assert "expected the reviewed 100 ms" in output.getvalue(), output.getvalue()

print("plain_audit_guards=PASS (GPIO, wireless/USB health, tally, nm, release invocation)")
