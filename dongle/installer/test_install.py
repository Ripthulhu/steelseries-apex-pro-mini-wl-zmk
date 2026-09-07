"""Offline checks for the stock receiver installer; no USB access."""
import hashlib
import errno
import json
from pathlib import Path
import struct
import tempfile
import unittest
import sys
from unittest.mock import patch

import install


class InstallerTests(unittest.TestCase):
    def test_drive_disappears_after_completed_copy(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            drive = folder / 'drive'
            drive.mkdir()
            (drive / 'INFO_UF2.TXT').write_text(install.BOARD)
            image = folder / 'image.uf2'
            image.write_bytes(b'test image')
            with patch.object(install.os, 'fsync', side_effect=OSError(errno.EBADF, 'device removed')):
                install.install_application(image, drive)
            self.assertEqual((drive / 'NEW.UF2').read_bytes(), image.read_bytes())
            with patch.object(install.os, 'fsync', side_effect=OSError(errno.ENOSPC, 'disk full')):
                with self.assertRaises(OSError):
                    install.install_application(image, drive)

    def test_install_sequence_stops_before_bootloader_on_bad_backup(self):
        for bad_backup in (False, True):
            with self.subTest(bad_backup=bad_backup), tempfile.TemporaryDirectory() as tmp:
                folder = Path(tmp)
                for name in ('readback', 'install'):
                    (folder / (name + '.patch.json')).write_text('{}')
                (folder / 'SHA256SUMS.json').write_text('{}')
                (folder / 'build.json').write_text('{"source_id":"test-build"}')
                backup = folder / 'backup'
                events = []
                def command(args, **kwargs):
                    events.append(Path(args[1]).name)
                def check_backup(path):
                    events.append('check_backup')
                    if bad_backup:
                        raise ValueError('different reads')
                with patch.object(install, 'HERE', folder), \
                     patch.object(install, 'stock_image', return_value=b'stock'), \
                     patch.object(install, 'patched_image', return_value=b'patch'), \
                     patch.object(install, 'validate_application'), \
                     patch.object(install, 'wait_stock'), \
                     patch.object(install.time, 'sleep'), \
                     patch.object(install.subprocess, 'run', side_effect=command), \
                     patch.object(install, 'check_backup', side_effect=check_backup), \
                     patch.object(install, 'install_application', side_effect=lambda *a: events.append('application')), \
                     patch.object(install, 'wait_application', return_value='test-port'), \
                     patch.dict(sys.modules, {'hid': unittest.mock.Mock(), 'serial': unittest.mock.Mock()}), \
                     patch.object(sys, 'argv', ['install.py', '--confirm', '--backup', str(backup)]):
                    if bad_backup:
                        with self.assertRaises(ValueError):
                            install.main()
                    else:
                        install.main()
                expected = ['usb_update.py', 'readback.py', 'check_backup']
                if not bad_backup:
                    expected += ['usb_update.py', 'application']
                self.assertEqual(events, expected)

    def test_unknown_firmware_rejected(self):
        with self.assertRaises(ValueError):
            install.patched_image(bytes(0x2d000), {'format': 1, 'edits': []})

    def test_patch_bounds_and_overlap(self):
        data = bytearray(0x2d000)
        struct.pack_into('<II', data, 0, 0x2000d000, 0x26001)
        digest = hashlib.sha256(data).hexdigest()
        with patch.object(install, 'STOCK_HASH', digest):
            for edits in ([[-1, '00']], [[len(data) - 4, '00']],
                          [[100, '0000'], [101, '00']]):
                with self.assertRaises(ValueError):
                    install.patched_image(data, {'format': 1, 'edits': edits})
            result = install.patched_image(data, {'format': 1, 'edits': [[100, '01']]})
            self.assertEqual(result[100], 1)
            install.validate(result)

    def test_two_pass_backup_and_layout(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            values = {'flash': bytes(0x80000), 'uicr': bytearray(0x1000)}
            struct.pack_into('<II', values['uicr'], 0x14, 0x74000, 0x7e000)
            record = {}
            for kind, data in values.items():
                for n in (1, 2):
                    (folder / f'{kind}-{n}.bin').write_bytes(data)
                record[kind + '_sha256'] = hashlib.sha256(data).hexdigest()
            (folder / 'readback.json').write_text(json.dumps(record))
            install.check_backup(folder)
            (folder / 'flash-2.bin').write_bytes(b'bad')
            with self.assertRaises(ValueError):
                install.check_backup(folder)

    def test_only_receiver_volume_is_accepted(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            info = folder / 'INFO_UF2.TXT'
            info.write_text('Board-ID: nRF52833-ApexProMiniWL-v1')
            self.assertIsNone(install.find_drive(folder))
            info.write_text(install.BOARD)
            self.assertEqual(install.find_drive(folder), folder)


if __name__ == '__main__':
    unittest.main()
