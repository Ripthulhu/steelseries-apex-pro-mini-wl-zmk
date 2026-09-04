# Building from source

The build uses Python and the same commands on every supported operating
system. It does not connect to the keyboard or install firmware.

## Before you start

Install these first:

- Git;
- Python 3.10 or newer, including support for Python virtual environments; and
- about 10 GB of free disk space.

Check that both programs are available. If your Python command is named
`python3`, use that name everywhere below.

```sh
git --version
python --version
```

Run all commands below from the root of this repository.

## Prepare the build tools

```sh
python tools/setup_workspace.py
```

This downloads the exact ZMK source, compiler, and supporting libraries used by
the project. It also applies the two changes that ZMK needs for this keyboard.
The download is several gigabytes and may take a while. It is safe to run the
command again if it was interrupted.

By default, downloaded files are kept next to the repository in `../work`:

```text
work/
  zmk-upstream/
    .venv/
    .zephyr-sdk/
    zephyr/
    modules/
  tools/
    adafruit-boot/
```

To store them elsewhere, pass the same location to setup and build:

```sh
python tools/setup_workspace.py --work-root /path/to/work
python tools/build_release.py --work-root /path/to/work
```

Each operating system needs its own work directory. Do not reuse one between a
native system and a virtual machine or compatibility layer.

## Build the firmware

```sh
python tools/build_release.py
```

The default build includes automatic recovery. It builds the application and
bootloader, checks their memory layout, and writes these release files:

- `../work/release/apex-pro-mini-wl-ab.uf2` — ready for a normal update;
- `../work/release/apex-pro-mini-wl-ab.zip` — the complete first-install and
  repair bundle; and
- `../work/release/RELEASE-SHA256SUMS.txt` — hashes for both downloads.

The ZIP is also left unpacked at `../work/release/apex-pro-mini-wl-ab`.

That directory contains:

```text
apex-zmk.hex
apex-zmk.uf2
apex-zmk.config
bootloader_mbr.hex
uicr-open.bin
SHA256SUMS.txt
verify_bundle.py
pi4.cfg
pico-debugprobe.cfg
stlink.cfg
jlink.cfg
KEYMAP.svg
README.txt
```

It also contains the project and third-party licence texts.

The files have different jobs:

- `apex-zmk.uf2` is the file copied to the `APEXBOOT` drive for normal updates.
- `apex-zmk.config` records the final Kconfig values used by the compiler.
- `apex-zmk.hex`, `bootloader_mbr.hex`, and `uicr-open.bin` are used together
  for the first installation or a repair with a hardware programmer.
- `SHA256SUMS.txt` lets you check that none of those files were damaged.
- The `.cfg` files tell OpenOCD how to use each included programmer setup.

See [First installation](INSTALL.md) before writing any of them to a keyboard.

## Development builds

Release builds always include A/B recovery. To change normal user-facing
settings, put the changes in a separate file and pass it to the build:

```sh
python tools/build_release.py --extra-conf my-keyboard.conf
```

See [Configuration](CONFIGURATION.md) for examples and the settings that should
not be changed.

To rebuild only the application while working on the code:

```sh
python tools/build_release.py --skip-bootloader
```

Application-only files are kept under `../work/artifacts-repo-apex-zmk-g4b*`.
Intermediate compiler output is kept under `../work/build-repo-apex-zmk-g4b*`.

### Testing A/B recovery

The lower-level builder can produce an application that fails deliberately so
the bootloader's rollback path can be tested:

```sh
python apex-zmk-g4b/build_g4b.py --stage 3 --usb-studio --kscan-ingest --persistent --plain-image --wireless-idle --ab-rollback --ab-crash-test
```

Do not use that image as normal firmware. It is only for an attended recovery
test on a keyboard that already has the A/B-capable bootloader and recovery
image.

## Checks without a build

Run the same portable checks used by CI:

```sh
python tools/verify_release.py
```

To also verify the local dependency revisions and SDK:

```sh
python tools/verify_release.py --work-root ../work
```

These checks do not connect to or write to the keyboard.

## Common problems

**`python` is not found:** install Python 3 and make sure its executable is on
your command path.

**Virtual-environment creation fails:** install the virtual-environment package
provided by your Python or operating-system distributor.

**A prepared dependency is reported as dirty:** the setup tool will not discard
local changes in downloaded source. Move or remove that work directory if you
do not need those changes, then run setup again.

**The build cannot find the compiler or UF2 converter:** rerun
`python tools/setup_workspace.py` with the same `--work-root` used for the
build.
