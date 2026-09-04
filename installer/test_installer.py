# SPDX-License-Identifier: MIT

from __future__ import annotations

import hashlib
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import flash_stock
import package_migration


def ihex_record(address: int, kind: int, data: bytes) -> str:
    record = bytes([len(data)]) + address.to_bytes(2, "big") + bytes([kind]) + data
    checksum = (-sum(record)) & 0xFF
    return ":" + (record + bytes([checksum])).hex().upper()


def rewrite_vendor_crc(image: bytearray) -> None:
    struct.pack_into("<I", image, len(image) - 4, zlib.crc32(image[:-4]) & 0xFFFFFFFF)


def synthetic_vendor_image() -> bytes:
    image = bytearray(flash_stock.IMAGE_SIZE)
    struct.pack_into("<II", image, 0, 0x20020000, 0x0001C101)
    payload = bytearray(flash_stock.BOOTLOADER_LENGTH)
    struct.pack_into("<II", payload, 0, 0x20020000, 0x0006E101)
    payload_crc = zlib.crc32(payload) & 0xFFFFFFFF
    metadata = flash_stock.METADATA.pack(
        b"APXMIG1\0", 1, 0x5C000, 0x6E000,
        flash_stock.BOOTLOADER_LENGTH, payload_crc, 0,
    )
    image[flash_stock.METADATA_OFFSET:
          flash_stock.METADATA_OFFSET + len(metadata)] = metadata
    image[flash_stock.BOOTLOADER_OFFSET:
          flash_stock.BOOTLOADER_OFFSET + len(payload)] = payload
    rewrite_vendor_crc(image)
    return bytes(image)


class InstallerValidationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.image = synthetic_vendor_image()

    def validate_bytes(self, image: bytes) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "image.bin"
            path.write_bytes(image)
            flash_stock.validate(path, hashlib.sha256(image).hexdigest())

    def test_valid_image_passes(self) -> None:
        self.validate_bytes(self.image)

    def test_rejects_wrong_sha256(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "image.bin"
            path.write_bytes(self.image)
            with self.assertRaisesRegex(RuntimeError, "SHA-256 mismatch"):
                flash_stock.validate(path, "0" * 64)

    def test_rejects_wrong_image_size(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "short.bin"
            image = self.image[:-1]
            path.write_bytes(image)
            with self.assertRaisesRegex(RuntimeError, "wrong image size"):
                flash_stock.validate(path, hashlib.sha256(image).hexdigest())

    def test_rejects_invalid_vendor_crc(self) -> None:
        image = bytearray(self.image)
        image[0x200] ^= 0x01
        with self.assertRaisesRegex(RuntimeError, "invalid SteelSeries CRC"):
            self.validate_bytes(image)

    def test_rejects_wrong_migration_vector(self) -> None:
        image = bytearray(self.image)
        struct.pack_into("<I", image, 4, 0x0005B001)
        rewrite_vendor_crc(image)
        with self.assertRaisesRegex(RuntimeError, "migration reset vector"):
            self.validate_bytes(image)

    def test_rejects_wrong_layout_metadata(self) -> None:
        image = bytearray(self.image)
        struct.pack_into("<I", image, flash_stock.METADATA_OFFSET + 12, 0x5D000)
        rewrite_vendor_crc(image)
        with self.assertRaisesRegex(RuntimeError, "metadata"):
            self.validate_bytes(image)

    def test_rejects_corrupt_embedded_bootloader(self) -> None:
        image = bytearray(self.image)
        image[flash_stock.BOOTLOADER_OFFSET + 0x200] ^= 0x01
        rewrite_vendor_crc(image)
        with self.assertRaisesRegex(RuntimeError, "embedded bootloader CRC"):
            self.validate_bytes(image)


class MigrationPackagingTests(unittest.TestCase):
    def test_packages_valid_synthetic_image(self) -> None:
        migration = bytearray(0x1008)
        struct.pack_into("<II", migration, 0, 0x20020000, 0x0001C101)
        loader_lines = [
            ihex_record(0, 4, b"\x00\x06"),
            ihex_record(0xE000, 0, struct.pack("<II", 0x20020000, 0x0006E101)),
            ihex_record(0, 4, b"\x10\x00"),
            ihex_record(0x1014, 0, struct.pack("<II", 0x0006E000, 0x0006B000)),
            ihex_record(0, 1, b""),
        ]

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            migration_path = root / "migration.bin"
            loader_path = root / "bootloader.hex"
            output_path = root / "migration.vendor.bin"
            migration_path.write_bytes(migration)
            loader_path.write_text("\n".join(loader_lines) + "\n", encoding="ascii")

            argv = [
                "package_migration.py",
                str(migration_path),
                str(loader_path),
                str(output_path),
            ]
            with mock.patch.object(sys, "argv", argv):
                self.assertEqual(package_migration.main(), 0)

            image = output_path.read_bytes()
            flash_stock.validate(output_path, hashlib.sha256(image).hexdigest())

    def test_rejects_loader_data_past_legacy_window(self) -> None:
        lines = [
            ihex_record(0, 4, b"\x00\x06"),
            ihex_record(0xE000, 0, struct.pack("<II", 0x20020000, 0x6E101)),
            ihex_record(0, 4, b"\x00\x07"),
            ihex_record(0x8000, 0, b"\x00\x01\x02\x03"),
            ihex_record(0, 1, b""),
        ]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "oversize.hex"
            path.write_text("\n".join(lines) + "\n", encoding="ascii")
            with self.assertRaisesRegex(ValueError, "outside the legacy layout"):
                package_migration.extract_bootloader(path)

    def test_rejects_wrong_loader_uicr_markers(self) -> None:
        lines = [
            ihex_record(0, 4, b"\x00\x06"),
            ihex_record(0xE000, 0, struct.pack("<II", 0x20020000, 0x6E101)),
            ihex_record(0, 4, b"\x10\x00"),
            ihex_record(0x1014, 0, struct.pack("<II", 0x74000, 0x6B000)),
            ihex_record(0, 1, b""),
        ]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "wrong-uicr.hex"
            path.write_text("\n".join(lines) + "\n", encoding="ascii")
            with self.assertRaisesRegex(ValueError, "wrong UICR marker"):
                package_migration.extract_bootloader(path)


if __name__ == "__main__":
    unittest.main()
