#!/usr/bin/env python3
"""Stage a last-known-good image B for A/B auto-rollback.

Streams a raw application image (the SAME bytes that live at internal 0x1000 -
i.e. zmk.bin / the app-slot payload, max 0x71000 bytes) into the external
SPI-NOR tail over the running firmware's updater vendor HID (usage page 0xFFC0),
then commits the descriptor so the bootloader-side promote can restore it.

Protocol (Feature report, 64 bytes, cmd byte first):
  0x50                                             AB erase B slot
  0x51 fs file len(u16 LE) off(u32 LE) data...     AB write chunk
  0x52 fs file len(u16 LE) blen(u32 LE)            AB commit (blen in the off slot)
Status (GET_FEATURE rep[0]): 0x00 READY, 0x01 BUSY, 0x02 ERROR.

This writes the external NOR B slot and its descriptor. Promotion to internal
flash is handled by the bootloader after repeated unhealthy boots.
"""
import sys, time, hid

VID, PID = 0x1d50, 0x615e
USAGE_PAGE = 0xFFC0
REPORT_LEN = 64
CHUNK = REPORT_LEN - 9
AB_ERASE, AB_WRITE, AB_COMMIT = 0x50, 0x51, 0x52
ST_READY, ST_BUSY, ST_ERROR = 0x00, 0x01, 0x02


def open_updater():
    for d in hid.enumerate(VID, PID):
        if d.get("usage_page") == USAGE_PAGE:
            h = hid.device(); h.open_path(d["path"]); return h
    raise SystemExit("updater HID (usage page 0xFFC0) not found")


def poll_ready(h, timeout=20.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        rep = h.get_feature_report(0, REPORT_LEN + 1)
        st = rep[1] if len(rep) > REPORT_LEN else rep[0]
        if st == ST_READY:
            return
        if st == ST_ERROR:
            raise SystemExit("device reported ERROR")
        time.sleep(0.02)
    raise SystemExit("timed out waiting for READY")


def feat(h, payload):
    h.send_feature_report(bytes([0x00]) + bytes(payload) +
                          bytes(REPORT_LEN - len(payload)))


def main(path):
    img = open(path, "rb").read()
    if not img or len(img) > 0x71000:
        raise SystemExit(f"image is {len(img)} bytes; must be 1..0x71000")
    h = open_updater()
    print(f"erasing B slot ({len(img)} bytes) ...")
    feat(h, [AB_ERASE]); poll_ready(h, 60.0)
    off = 0
    while off < len(img):
        chunk = img[off:off + CHUNK]
        hdr = [AB_WRITE, 3, 11, len(chunk) & 0xFF, (len(chunk) >> 8) & 0xFF,
               off & 0xFF, (off >> 8) & 0xFF, (off >> 16) & 0xFF, (off >> 24) & 0xFF]
        feat(h, hdr + list(chunk)); poll_ready(h)
        off += len(chunk)
        if off % 0x8000 < CHUNK:
            print(f"  {off}/{len(img)}")
    blen = len(img)
    hdr = [AB_COMMIT, 3, 11, 0, 0,
           blen & 0xFF, (blen >> 8) & 0xFF, (blen >> 16) & 0xFF, (blen >> 24) & 0xFF]
    print("committing descriptor ...")
    feat(h, hdr); poll_ready(h)
    print(f"staged B: {blen} bytes, armed for rollback")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: stage_b.py <app-image.bin>")
    main(sys.argv[1])
