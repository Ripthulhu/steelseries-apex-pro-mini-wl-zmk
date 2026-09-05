#!/usr/bin/env python3
import serial, time, sys
SOF, ESC, EOF = 0xAB, 0xAC, 0xAD

def frame(d):
    o = bytearray([SOF])
    for b in d:
        if b in (SOF, ESC, EOF):
            o.append(ESC)
        o.append(b)
    o.append(EOF)
    return bytes(o)

def unframe(buf):
    i = buf.find(bytes([SOF]))
    if i < 0:
        return None
    out = bytearray(); esc = False
    for b in buf[i+1:]:
        if esc:
            out.append(b); esc = False
        elif b == ESC:
            esc = True
        elif b == EOF:
            return bytes(out)
        elif b == SOF:
            out = bytearray()
        else:
            out.append(b)
    return None

def rv(d, i):
    sh = 0; v = 0
    while True:
        b = d[i]; i += 1
        v |= (b & 0x7f) << sh
        if not b & 0x80:
            break
        sh += 7
    return v, i

def walk(d):
    i = 0; o = {}
    while i < len(d):
        t, i = rv(d, i); fn = t >> 3; wt = t & 7
        if wt == 0:
            v, i = rv(d, i); o.setdefault(fn, []).append(v)
        elif wt == 2:
            ln, i = rv(d, i); o.setdefault(fn, []).append(d[i:i+ln]); i += ln
        else:
            break
    return o

port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
s = serial.Serial(port, 115200, timeout=0.5)
s.dtr = True
time.sleep(0.5)
drained = s.read(16384)
print("drained", len(drained), "bytes of stale data")
req = bytes([0x08, 0x01, 0x32, 0x02, 0x08, 0x01])
s.write(frame(req)); s.flush()
raw = bytearray(); t = time.time() + 3
while time.time() < t:
    c = s.read(512)
    if c:
        raw += c
        si = raw.find(bytes([SOF]))
        if si >= 0 and raw.find(bytes([EOF]), si) >= 0:
            break
s.close()
print("response", len(raw), "bytes")
p = unframe(bytes(raw))
if not p:
    print("no complete frame"); sys.exit(1)
top = walk(p)
rr = walk(top[1][0])
if 6 not in rr:
    print("no apex field in response:", list(rr.keys())); sys.exit(1)
apex = walk(rr[6][0])
hm = walk(apex[1][0])
total = hm.get(2, [0])[0]; peak = hm.get(3, [0])[0]
blob = hm.get(1, [b''])[0]
cs = []; i = 0
while i < len(blob):
    v, i = rv(blob, i); cs.append(v)
nz = [(idx, c) for idx, c in enumerate(cs) if c]
print("HEATMAP OK: %d keys, total=%d, peak=%d, nonzero=%s" % (len(cs), total, peak, nz[:12]))
