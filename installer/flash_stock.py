# SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
import time
import zlib
from pathlib import Path


VID = 0x1038
NORMAL_PID = 0x1626
RECOVERY_PID = 0x1627
USAGE_PAGE = 0xFFC0
USAGE = 0x0001
IMAGE_SIZE = 0x4B000
CHUNK_SIZE = 50
CRC32_RESIDUE = 0x2144DF1C
METADATA_OFFSET = 0x5B000 - 0x1C000
BOOTLOADER_OFFSET = 0x5C000 - 0x1C000
BOOTLOADER_LENGTH = 0xA000
METADATA = struct.Struct("<8s6I")


def validate(path: Path, expected_sha256: str) -> tuple[bytes, int]:
    image = path.read_bytes()
    digest = hashlib.sha256(image).hexdigest()
    if digest.lower() != expected_sha256.lower():
        raise RuntimeError(f"SHA-256 mismatch: got {digest}")
    if len(image) != IMAGE_SIZE:
        raise RuntimeError(f"wrong image size: {len(image)} != {IMAGE_SIZE}")
    if zlib.crc32(image) & 0xFFFFFFFF != CRC32_RESIDUE:
        raise RuntimeError("invalid SteelSeries CRC residue")
    trailer = struct.unpack_from("<I", image, IMAGE_SIZE - 4)[0]
    computed = zlib.crc32(image[:-4]) & 0xFFFFFFFF
    if trailer != computed:
        raise RuntimeError("CRC trailer does not match the image")

    migration_stack, migration_reset = struct.unpack_from("<II", image)
    if migration_stack != 0x20020000:
        raise RuntimeError(f"invalid migration stack: 0x{migration_stack:08x}")
    if migration_reset & 1 == 0 or not 0x1C000 <= (migration_reset & ~1) < 0x5B000:
        raise RuntimeError(f"invalid migration reset vector: 0x{migration_reset:08x}")

    magic, version, source, destination, length, payload_crc, reserved = \
        METADATA.unpack_from(image, METADATA_OFFSET)
    if (magic, version, source, destination, length, reserved) != (
        b"APXMIG1\0", 1, 0x5C000, 0x6E000, BOOTLOADER_LENGTH, 0
    ):
        raise RuntimeError("migration metadata is not the expected version/layout")
    payload = image[BOOTLOADER_OFFSET : BOOTLOADER_OFFSET + BOOTLOADER_LENGTH]
    if zlib.crc32(payload) & 0xFFFFFFFF != payload_crc:
        raise RuntimeError("embedded bootloader CRC does not match metadata")
    stack, reset = struct.unpack_from("<II", payload)
    if not 0x20000000 <= stack <= 0x20020000:
        raise RuntimeError(f"invalid bootloader stack: 0x{stack:08x}")
    if reset & 1 == 0 or not 0x6E000 <= (reset & ~1) < 0x78000:
        raise RuntimeError(f"invalid bootloader reset vector: 0x{reset:08x}")

    print(f"image={path}")
    print(f"sha256={digest}")
    print(f"size={len(image)} crc32=0x{computed:08x} migration_reset=0x{migration_reset:08x}")
    print(f"bootloader_crc32=0x{payload_crc:08x} reset=0x{reset:08x}")
    return image, computed


def import_hid():
    try:
        import hid
    except ImportError as exc:
        raise RuntimeError("USB flashing needs the 'hidapi' Python package") from exc
    return hid


def channels(hid, pid: int) -> list[dict]:
    return [
        item for item in hid.enumerate(VID, pid)
        if item.get("usage_page") == USAGE_PAGE and item.get("usage") == USAGE
    ]


def wait_channel(hid, pid: int, timeout: float = 60.0) -> dict:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        found = channels(hid, pid)
        if len(found) == 1:
            return found[0]
        if len(found) > 1:
            raise RuntimeError(f"multiple 1038:{pid:04x} update channels found")
        time.sleep(0.1)
    raise TimeoutError(f"timed out waiting for 1038:{pid:04x}")


def open_device(hid, pid: int):
    item = wait_channel(hid, pid)
    dev = hid.device()
    dev.open_path(item["path"])
    return dev


def exchange(dev, payload: bytes, timeout_ms: int = 6000) -> bytes:
    report = payload + bytes(65 - len(payload))
    written = dev.write(report)
    reply = bytes(dev.read(64, timeout_ms))
    if not reply:
        raise TimeoutError(f"no reply for command 0x{payload[1]:02x}")
    if len(reply) != 64:
        raise RuntimeError(f"short reply: {len(reply)}/64")
    if written not in (65, -1):
        raise RuntimeError(f"short write: {written}/65")
    if reply[1] == 0x40:
        raise RuntimeError(f"device rejected command: {reply[:16].hex(' ')}")
    return reply


def reopen(hid, dev, pid: int):
    try:
        dev.close()
    except OSError:
        pass
    return open_device(hid, pid)


def resilient_exchange(hid, dev, pid: int, payload: bytes, attempts: int = 6):
    last_error: Exception | None = None
    for attempt in range(attempts):
        try:
            return dev, exchange(dev, payload)
        except (OSError, TimeoutError, RuntimeError) as exc:
            last_error = exc
            if attempt + 1 == attempts:
                break
            time.sleep(min(0.2 * (2**attempt), 1.0))
            dev = reopen(hid, dev, pid)
    raise RuntimeError(f"USB command failed after {attempts} attempts: {last_error}")


def flash(image: bytes, image_crc: int, pid: int) -> None:
    hid = import_hid()
    dev = open_device(hid, pid)
    print(f"update_channel=1038:{pid:04x}")
    try:
        dev, reply = resilient_exchange(hid, dev, pid, bytes([0, 0x42, 3, 11]))
        print(f"erase_reply={reply[:16].hex(' ')}")
        total = len(image) // CHUNK_SIZE
        for index in range(total):
            offset = index * CHUNK_SIZE
            payload = bytes([0, 0x43, 3, 11]) + struct.pack("<HI", CHUNK_SIZE, offset)
            payload += image[offset : offset + CHUNK_SIZE]
            dev, _ = resilient_exchange(hid, dev, pid, payload)
            if (index + 1) % 256 == 0 or index + 1 == total:
                print(f"progress={index + 1}/{total}", flush=True)

        dev = reopen(hid, dev, pid)
        dev, reply = resilient_exchange(hid, dev, pid, bytes([0, 0x84, 3, 11]))
        if len(reply) < 10:
            raise RuntimeError("device CRC reply is too short")
        crc_a, crc_b = struct.unpack_from("<II", reply, 2)
        print(f"device_crc=0x{crc_a:08x} expected=0x{image_crc:08x}")
        if crc_a != crc_b or crc_a != image_crc:
            raise RuntimeError("device CRC mismatch; reset was not sent")

        report = bytes([0, 0x41, 0]) + bytes(62)
        try:
            dev.write(report)
        except OSError:
            pass  # Expected when reset tears the HID endpoint down immediately.
        print("migration_staged=true")
        print("next=wait for the APEXBOOT drive, then copy the final firmware UF2")
    finally:
        try:
            dev.close()
        except OSError:
            pass


def main() -> int:
    parser = argparse.ArgumentParser(description="Flash a validated migration image through SteelSeries recovery")
    parser.add_argument("image", type=Path)
    parser.add_argument("--confirm-sha256", required=True)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--normal", action="store_true", help="require stock normal PID 1038:1626")
    mode.add_argument("--recovery", action="store_true", help="require stock recovery PID 1038:1627")
    parser.add_argument("--flash", action="store_true", help="perform the update; without this flag only validation runs")
    args = parser.parse_args()

    image, crc = validate(args.image, args.confirm_sha256)
    if not args.flash:
        print("validate_only=true")
        return 0
    if not (args.normal or args.recovery):
        parser.error("--flash requires exactly one of --normal or --recovery")
    flash(image, crc, NORMAL_PID if args.normal else RECOVERY_PID)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, TimeoutError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
