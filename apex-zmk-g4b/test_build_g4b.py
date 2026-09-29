"""Check build selection without starting a compiler or touching hardware."""
import contextlib
import io
from unittest.mock import patch

import build_g4b


for options in ([], ["--stage", "3", "--usb-studio"],
                ["--plain-image", "--high-wrapper"],
                ["--plain-image", "--beacon-state"]):
    with patch("sys.argv", ["build_g4b.py", *options]), \
            contextlib.redirect_stderr(io.StringIO()):
        try:
            build_g4b.parse_args()
        except SystemExit as error:
            assert error.code == 2
        else:
            raise AssertionError(f"retired build options accepted: {options}")

for option in ("--plain-image", "--shell", "--ble-shell", "--stop1-canary",
               "--wireless-idle", "--ab-rollback"):
    with patch("sys.argv", ["build_g4b.py", "--stage", "3", "--usb-studio", option]):
        args = build_g4b.parse_args()
        assert build_g4b.stage_config(args) == "g4b_usb.conf"

print("build_selection=PASS (retired wrapper rejected; Adafruit paths accepted)")
