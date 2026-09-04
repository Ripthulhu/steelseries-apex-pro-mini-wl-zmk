#!/usr/bin/env python3
"""Tests for the manifest and UICR checks used before destructive SWD writes."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import make_uicr
import program_keyboard


class BackupValidationTests(unittest.TestCase):
    def make_pair(self, directory: Path) -> tuple[Path, Path, Path]:
        flash = directory / program_keyboard.BACKUP_FILES["flash"][0]
        uicr = directory / program_keyboard.BACKUP_FILES["uicr"][0]
        manifest = directory / program_keyboard.BACKUP_MANIFEST
        flash.write_bytes(b"\xa5" * program_keyboard.BACKUP_FILES["flash"][2])
        uicr.write_bytes(make_uicr.build_image())
        program_keyboard.write_backup_manifest(
            manifest,
            program_keyboard.backup_digest(
                flash, program_keyboard.BACKUP_FILES["flash"][2]
            ),
            program_keyboard.backup_digest(
                uicr, program_keyboard.BACKUP_FILES["uicr"][2]
            ),
        )
        return flash, uicr, manifest

    def test_accepts_matching_manifest_pair_and_open_uicr(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            flash, uicr, manifest = self.make_pair(Path(temporary))
            checked_flash, checked_uicr = program_keyboard.checked_backup_pair(
                flash, uicr, manifest
            )
            self.assertEqual(checked_flash, flash.resolve())
            self.assertEqual(checked_uicr, uicr.resolve())
            program_keyboard.validate_open_bootloader_uicr(checked_uicr)

    def test_rejects_tampered_flash(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            flash, uicr, manifest = self.make_pair(Path(temporary))
            flash.write_bytes(b"\x5a" + flash.read_bytes()[1:])
            with self.assertRaisesRegex(RuntimeError, "does not match the manifest"):
                program_keyboard.checked_backup_pair(flash, uicr, manifest)

    def test_rejects_all_ff_uicr_for_bootloader_repair(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "uicr-open-backup.bin"
            path.write_bytes(b"\xff" * program_keyboard.BACKUP_FILES["uicr"][2])
            with self.assertRaisesRegex(RuntimeError, "not from the open bootloader layout"):
                program_keyboard.validate_open_bootloader_uicr(path)

    def test_rejects_manifest_for_another_target(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            flash, uicr, manifest = self.make_pair(Path(temporary))
            text = manifest.read_text(encoding="ascii").replace(
                program_keyboard.BACKUP_TARGET, "another-target"
            )
            manifest.write_text(text, encoding="ascii", newline="\n")
            with self.assertRaisesRegex(RuntimeError, "different target"):
                program_keyboard.checked_backup_pair(flash, uicr, manifest)


if __name__ == "__main__":
    unittest.main()
