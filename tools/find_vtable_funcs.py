"""Trouve les fonctions referencees seulement par des pointeurs en donnees (tables virtuelles).

Usage: find_vtable_funcs.py image.bin ppc_func_mapping.cpp switch_tables.toml
Un pointeur vers .text est retenu s'il n'est pas deja un debut de fonction connu, ne tombe pas
dans une fonction de .pdata et n'est pas une cible de switch. La taille va jusqu'au debut de
fonction connu suivant. Sortie: lignes `{ address = 0x..., size = 0x... },`.
"""
import bisect
import re
import struct
import sys

BASE = 0x82000000
TEXT_START, TEXT_END = 0x82230000, 0x82230000 + 0xA93CE4
DATA_SECTIONS = [(0x82000400, 0x1E2680), (0x82CD0000, 0x16E280)]  # .rdata, .data
PDATA_START, PDATA_SIZE = 0x821E2C00, 0x42C00

img = open(sys.argv[1], 'rb').read()
known = {int(x, 16) for x in re.findall(r'\{ 0x([0-9A-F]+),', open(sys.argv[2]).read())}
switch_labels = {int(x, 16) for x in re.findall(r'^\s+0x([0-9A-Fa-f]+),', open(sys.argv[3]).read(), re.M)}


def word(addr):
    return struct.unpack_from('>I', img, addr - BASE)[0]


pdata = []
for i in range(0, PDATA_SIZE, 8):
    begin, flags = struct.unpack_from('>II', img, PDATA_START - BASE + i)
    if begin:
        pdata.append((begin, begin + ((flags >> 8) & 0x3FFFFF) * 4))
pdata.sort()
pdata_starts = [b for b, _ in pdata]


def in_pdata_function(addr):
    i = bisect.bisect_right(pdata_starts, addr) - 1
    return i >= 0 and pdata[i][0] < addr < pdata[i][1]


candidates = set()
for start, size in DATA_SECTIONS:
    for addr in range(start, start + size - 3, 4):
        value = word(addr)
        if TEXT_START <= value < TEXT_END and value % 4 == 0:
            if value in known or value in switch_labels or in_pdata_function(value):
                continue
            # A function never starts with zero padding
            if word(value) == 0:
                continue
            candidates.add(value)

starts = sorted(known | set(pdata_starts) | candidates)
for value in sorted(candidates):
    i = bisect.bisect_right(starts, value)
    end = starts[i] if i < len(starts) else TEXT_END
    # Trim trailing padding
    while end > value + 4 and word(end - 4) == 0:
        end -= 4
    print(f'    {{ address = 0x{value:X}, size = 0x{end - value:X} }},')
print(f'# {len(candidates)} fonctions trouvees', file=sys.stderr)
