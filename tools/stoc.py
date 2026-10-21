#!/usr/bin/env python3
"""Archive EA 'SToc' (.str) : liste et extrait les blocs (RefPack si compresses).
Usage : stoc.py <fichier.str> [dossier de sortie]"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from refpack import decompress


def read_toc(raw):
    assert raw[:4] == b'SToc'
    table = struct.unpack_from('>I', raw, 0x10)[0]
    entries = []
    pos = table
    offset = 0x800
    while pos + 0x18 <= 0x800:
        h, kind, size, aligned, stored, extra = struct.unpack_from('>6I', raw, pos)
        if h == 0 and kind == 0:
            break
        entries.append(dict(hash=h, kind=kind, size=size, aligned=aligned, stored=stored, extra=extra, offset=offset))
        offset += stored
        pos += 0x18
    return entries


def chunk_data(raw, e):
    data = raw[e['offset']:e['offset'] + e['stored']]
    if len(data) > 2 and data[1] == 0xFB:
        return decompress(data)[0]
    return data


if __name__ == '__main__':
    raw = open(sys.argv[1], 'rb').read()
    for i, e in enumerate(read_toc(raw)):
        d = chunk_data(raw, e)
        print(i, ' '.join(f'{k}={v:08X}' for k, v in e.items()), 'decomp', hex(len(d)))
        if len(sys.argv) > 2:
            os.makedirs(sys.argv[2], exist_ok=True)
            open(os.path.join(sys.argv[2], f'{i:02d}_{e["hash"]:08X}.bin'), 'wb').write(d)
