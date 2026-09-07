"""Apex receiver updater, matching GG's base_apex_2022_wireless.lisp."""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import time
import zlib

VID = 0x1038
PIDS = (0x1624, 0x1625)
IMAGE_SIZE = 0x2D000
APP_BASE = 0x23000
CHUNK_SIZE = 48


def validate(image, expected_hash=None):
    digest = hashlib.sha256(image).hexdigest()
    if expected_hash and digest != expected_hash.lower():
        raise ValueError("image SHA-256 mismatch")
    if len(image) != IMAGE_SIZE:
        raise ValueError("receiver image must be exactly 0x2d000 bytes")
    crc = zlib.crc32(image[:-4]) & 0xFFFFFFFF
    if struct.unpack_from("<I", image, len(image) - 4)[0] != crc:
        raise ValueError("invalid image CRC trailer")
    sp, reset = struct.unpack_from("<II", image)
    if not 0x20000000 < sp <= 0x20020000 or sp % 8:
        raise ValueError("invalid initial stack pointer")
    if not reset & 1 or not APP_BASE <= (reset & ~1) < APP_BASE + IMAGE_SIZE - 4:
        raise ValueError("reset handler outside receiver application slot")
    return {"sha256": digest, "crc": crc, "sp": sp, "reset": reset}


def packet(command, data=b""):
    report = bytes((0, command)) + data
    if len(report) > 65:
        raise ValueError("HID output report too large")
    return report.ljust(65, b"\0")


def write_packet(offset, data):
    if offset % CHUNK_SIZE or len(data) != CHUNK_SIZE or not 0 <= offset <= IMAGE_SIZE - CHUNK_SIZE:
        raise ValueError("invalid firmware chunk")
    return packet(0x03, bytes((1, 17)) + struct.pack("<HI", len(data), offset) + data)


def check_response(reply, opcode):
    if len(reply) != 64 or reply[0] != opcode or reply[1] != 0:
        raise RuntimeError(f"unexpected {opcode:02x} response: {reply.hex()}")
    return reply


class Receiver:
    def __init__(self, log):
        import hid
        entries = [e for e in hid.enumerate(VID, 0)
                   if e["product_id"] in PIDS and e["usage_page"] == 0xFFC0 and e["usage"] == 1]
        if len(entries) != 1:
            raise RuntimeError(f"expected one Apex receiver control interface, found {len(entries)}")
        self.entry = entries[0]
        self.log = log
        self.device = hid.device()
        self.device.open_path(self.entry["path"])
        self.device.set_nonblocking(True)
        for _ in range(128):
            if not self.device.read(64):
                break
        self.device.set_nonblocking(False)
        self.record("open", pid=self.entry["product_id"], path=str(self.entry["path"]))

    def record(self, event, **fields):
        self.log.write(json.dumps({"time": time.time(), "event": event, **fields}) + "\n")
        self.log.flush()

    def exchange(self, report, timeout=20000):
        sent = self.device.write(report)
        reply = bytes(self.device.read(64, timeout))
        self.record("exchange", tx=report.hex(), written=sent, rx=reply.hex())
        if sent != 65:
            raise RuntimeError(f"short HID write: {sent}")
        return check_response(reply, report[1])

    def crc(self):
        response = self.exchange(packet(0x84, bytes((1, 17))))
        values = struct.unpack_from("<II", response, 2)
        print(f"Receiver CRCs: {values[0]:08x} / {values[1]:08x}", flush=True)
        return values

    def close(self):
        self.device.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("check", "flash", "validate"))
    parser.add_argument("image", type=Path)
    parser.add_argument("--sha256")
    parser.add_argument("--log", type=Path)
    args = parser.parse_args()
    image = args.image.read_bytes()
    meta = validate(image, args.sha256)
    print(json.dumps(meta), flush=True)
    if args.action == "validate":
        return
    if args.action == "flash" and not args.sha256:
        parser.error("flash requires --sha256 for the intended image")
    log_path = args.log or Path(__file__).parent / (time.strftime("usb-update-%Y%m%d-%H%M%S") + ".jsonl")
    with log_path.open("x", encoding="utf-8") as log:
        receiver = Receiver(log)
        try:
            before = receiver.crc()
            if args.action == "check":
                if before != (meta["crc"], meta["crc"]):
                    raise RuntimeError("device firmware-file CRC differs from supplied image")
                print("Firmware-file CRC matches supplied image; no erase or flash performed.")
                return
            receiver.record("image", **meta)
            receiver.exchange(packet(0x02, bytes((1, 17))))
            for offset in range(0, len(image), CHUNK_SIZE):
                receiver.exchange(write_packet(offset, image[offset:offset + CHUNK_SIZE]))
                if offset % (CHUNK_SIZE * 256) == 0:
                    print(f"Written {offset + CHUNK_SIZE}/{len(image)} bytes", flush=True)
            if receiver.crc() != (meta["crc"], meta["crc"]):
                raise RuntimeError("programmed CRC mismatch; reset withheld")
            receiver.record("crc_verified", **meta)
            print("Both CRCs match. Resetting receiver.", flush=True)
            try:
                result = receiver.device.write(packet(0x01, bytes((0,))))
                receiver.record("reset", written=result)
            except OSError as error:
                receiver.record("reset_disconnect", error=str(error))
        finally:
            receiver.close()


if __name__ == "__main__":
    main()

