# SPDX-License-Identifier: Apache-2.0
"""End-to-end compaction: exact RAW, opaque chunks, index, refusal/cleanup."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

compact, decoder, vectors = map(Path, sys.argv[1:])

def crc(data):
    c = 0xffffffff
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ (0x82f63b78 if c & 1 else 0)
    return c ^ 0xffffffff

def chunk(tag, data, flags=0):
    return tag + struct.pack('<IQ', flags, len(data)) + data + struct.pack('<I', crc(data))

def parse(path):
    data = path.read_bytes()
    pos, out = 16, []
    while pos < len(data) - 16:
        tag, flags, size = struct.unpack_from('<4sIQ', data, pos)
        body = data[pos+16:pos+16+size]
        assert crc(body) == struct.unpack_from('<I', data, pos+16+size)[0]
        out.append((tag, flags, body))
        pos += 20 + size
    assert pos == len(data)-16
    return out

def run(*args, ok=True):
    p = subprocess.run(list(map(str,args)), capture_output=True, text=True)
    assert (p.returncode == 0) == ok, p.stdout+p.stderr
    return p

with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    inp, out = root/'in.craw', root/'out.craw'
    names = ['live2_scene10_gbrg', 'live2_edges16_tile5', 'rgb_live2_scene10', 'live_zero_width']
    parts = [(b'META', 0, b'{"custom":"keep formatting", "sensor": {}}', 0)]
    for i, name in enumerate(names):
        meta = b'{ "exposure": 0.125, "custom": [1,2,3] }'
        body = struct.pack('<QqI', i+42, i*33333333+987, len(meta))+meta+(vectors/(name+'.payload')).read_bytes()
        parts.append((b'FRAM', 0x1234, body, i*33333333+987))
        # Opaque ancillary bytes, including unknown chunk types and flags.
        for tag in (b'AUDI', b'LSHD', b'IMU_', b'THMB', b'XTRA'):
            parts.append((tag, i+7, bytes(range(256)), i*33333333+987))
    data = b'CRAW'+struct.pack('<HHII',0,2,0,0)
    entries = []
    for tag, flags, body, ts in parts:
        if tag != b'META': entries.append(struct.pack('<4sIQq',tag,0,len(data),ts))
        data += chunk(tag,body,flags)
    ix_offset = len(data)
    data += chunk(b'INDX',struct.pack('<I',len(entries))+b''.join(entries))
    data += struct.pack('<Q8s',ix_offset,b'CRAWEND\0')
    inp.write_bytes(data)
    run(compact,inp,out)
    assert inp.read_bytes() == data
    old,new = parse(inp),parse(out)
    assert len(old) == len(new)
    for a,b in zip(old,new):
        assert a[:2] == b[:2]
        if a[0] == b'FRAM':
            prefix = 20+struct.unpack_from('<I',a[2],16)[0]
            assert a[2][:prefix] == b[2][:prefix]
            assert len(b[2]) <= len(a[2])
        elif a[0] != b'INDX': assert a == b
    verify = json.loads(run(decoder,'verify',out).stdout)
    assert not verify['recovered'] and verify['frames']==len(names) and verify['bad_frames']==0
    rawdir=root/'raw';rawdir.mkdir()
    run(decoder,'frames',out,rawdir)
    for i,name in enumerate(names):
        assert (rawdir/f'frame_{i:06d}.raw').read_bytes() == (vectors/(name+'.raw')).read_bytes()
    saved=out.read_bytes()
    run(compact,inp,out,ok=False)
    run(compact,inp,inp,ok=False)
    assert out.read_bytes()==saved and inp.read_bytes()==data
    bad=root/'bad.craw';bad.write_bytes(data[:-16])
    run(compact,bad,root/'recovered.craw',ok=False)
    corrupt=bytearray(data);corrupt[50]^=1;bad.write_bytes(corrupt)
    run(compact,bad,root/'corrupt.craw',ok=False)
    assert not (root/'corrupt.craw').exists()
    corrupt=bytearray(data);corrupt[data.index(b'AUDI')+8]^=1;bad.write_bytes(corrupt)
    run(compact,bad,root/'badheader.craw',ok=False)
    assert not (root/'badheader.craw').exists()
    assert not list(root.glob('*.partial.*'))
print('PASS compact: exact RAW, frame records, opaque chunks, index, no-clobber and cleanup')
