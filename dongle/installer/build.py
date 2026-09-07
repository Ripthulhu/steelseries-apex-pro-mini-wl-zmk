#!/usr/bin/env python3
"""Compile installer patches without including the SteelSeries firmware."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess


def array(name, blob):
    words = struct.unpack('<' + 'I' * (len(blob) // 4), blob)
    return 'const unsigned ' + name + '[] = {\n' + ','.join(hex(v) for v in words) + '\n};\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--boot', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    from intelhex import IntelHex
    from elftools.elf.elffile import ELFFile
    here = Path(__file__).resolve().parent
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    compiler = args.workspace.resolve() / '.zephyr-sdk/arm-zephyr-eabi/bin'
    suffix = '.exe' if os.name == 'nt' else ''
    boot = IntelHex(str(args.boot))
    boot_bytes = bytes(boot.tobinarray(start=0x74000, size=0xa000))
    mbr = bytearray(boot.tobinarray(start=0, size=4096))
    struct.pack_into('<II', mbr, 0xff8, 0x74000, 0x7e000)
    if not 0x74000 <= (struct.unpack_from('<I', boot_bytes, 4)[0] & ~1) < 0x7d800:
        raise ValueError('Unexpected bootloader reset vector')
    blobs = output / 'migration_blobs.c'
    blobs.write_text(array('boot_image', boot_bytes) + array('mbr_image', mbr))
    for name in ('readback', 'install'):
        elf, raw = output / (name + '.elf'), output / (name + '.bin')
        command = [str(compiler / ('arm-zephyr-eabi-gcc' + suffix)), '-mcpu=cortex-m4',
                   '-mthumb', '-Os', '-ffreestanding', '-fno-builtin',
                   '-fno-delete-null-pointer-checks', '-fno-unwind-tables',
                   '-fno-asynchronous-unwind-tables', '-nostdlib', '-Wall', '-Wextra', '-Werror',
                   '-Wl,-T,' + str(here / 'migrate.ld'), str(here / 'migrate.c'),
                   str(blobs), '-o', str(elf)]
        if name == 'readback':
            command += ['-DDONGLE_INSPECT_ONLY', '-DDONGLE_READBACK']
        subprocess.run(command, check=True)
        subprocess.run([str(compiler / ('arm-zephyr-eabi-objcopy' + suffix)),
                        '-O', 'binary', str(elf), str(raw)], check=True)
        payload = raw.read_bytes()
        if len(payload) > 0x17000:
            raise ValueError('Installer exceeds the unused stock application space')
        edits = [(0x3000, payload), (4, struct.pack('<I', 0x26001)),
                 (0x1fb0, struct.pack('<I', 0x2001f800))]
        if name == 'readback':
            with elf.open('rb') as stream:
                symbol = ELFFile(stream).get_section_by_name('.symtab').get_symbol_by_name('readback_report')[0]
                handler = int(symbol['st_value']) | 1
            edits.append((0x1fa8, struct.pack('<HHI', 0x4b00, 0x4718, handler)))
        (output / (name + '.patch.json')).write_text(json.dumps(
            {'format': 1, 'edits': [[offset, data.hex()] for offset, data in edits]}, indent=2) + '\n')


if __name__ == '__main__':
    main()
