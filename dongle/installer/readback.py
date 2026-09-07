"""Save two matching full-flash/UICR reads from the diagnostic receiver build."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import time
import hid

output = Path(sys.argv[1]).resolve()
output.mkdir(parents=True, exist_ok=False)
entries = [e for e in hid.enumerate(0x1038, 0x1624)
           if e['usage_page'] == 0xffc0 and e['usage'] == 1]
assert len(entries) == 1, entries
device = hid.device()
device.open_path(entries[0]['path'])
device.set_nonblocking(True)
while device.read(64):
    pass
device.set_nonblocking(False)
captures = []
try:
    for run in range(2):
        data = bytearray(0x81000)
        covered = bytearray(len(data))
        total = 0
        deadline = time.monotonic() + 120
        packets = 0
        while total < len(data):
            assert time.monotonic() < deadline, ('readback timeout', total)
            assert device.write(bytes((0, 0x90, 0)) + bytes(62)) == 65
            report = bytes(device.read(64, 2000))
            assert len(report) == 64 and report[:4] == b'DMP1', report.hex()
            address = struct.unpack_from('<I', report, 4)[0]
            if address < 0x80000:
                offset, limit = address, 0x80000
            else:
                assert 0x10001000 <= address < 0x10002000, hex(address)
                offset, limit = address - 0x10001000 + 0x80000, 0x81000
            count = min(56, limit - offset)
            payload = report[8:8+count]
            for i in range(count):
                if covered[offset+i]:
                    assert data[offset+i] == payload[i], ('inconsistent duplicate', hex(address+i))
            total += count - sum(covered[offset:offset+count])
            data[offset:offset+count] = payload
            covered[offset:offset+count] = bytes([1]) * count
            packets += 1
            if packets % 1024 == 0:
                print(f'Read {run+1}: {total}/{len(data)} bytes', flush=True)
        captures.append(bytes(data))
        (output / f'flash-{run+1}.bin').write_bytes(data[:0x80000])
        (output / f'uicr-{run+1}.bin').write_bytes(data[0x80000:])
        print('Read complete:', run+1, hashlib.sha256(data).hexdigest(), flush=True)
finally:
    device.close()
assert captures[0] == captures[1], 'full reads differ; saved both for comparison'
metadata = {'method': 'diagnostic USB readback, two byte-identical passes',
            'flash_sha256': hashlib.sha256(captures[0][:0x80000]).hexdigest(),
            'uicr_sha256': hashlib.sha256(captures[0][0x80000:]).hexdigest(),
            'warning': 'Captured after loading temporary readback firmware; includes that application, not the previous application.'}
(output / 'readback.json').write_text(json.dumps(metadata, indent=2) + '\n')
print(json.dumps(metadata, indent=2))
