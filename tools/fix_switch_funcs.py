"""Calcule des limites de fonctions explicites pour les switch hors .pdata.

Usage: fix_switch_funcs.py image.bin switch_tables.toml bad_switches.txt
Sortie: lignes `{ address = 0x..., size = 0x... },` pour la cle `functions`.
"""
import re
import struct
import sys

BASE = 0x82000000
TEXT_START, TEXT_END = 0x82230000, 0x82230000 + 0xA93CE4
PDATA_START, PDATA_SIZE = 0x821E2C00, 0x42C00

img = open(sys.argv[1], 'rb').read()
toml = open(sys.argv[2]).read()
bad = [int(x, 16) for x in open(sys.argv[3]).read().split()]


def word(addr):
    return struct.unpack_from('>I', img, addr - BASE)[0]


# Fonctions connues par .pdata
pdata = []
for i in range(0, PDATA_SIZE, 8):
    begin, flags = struct.unpack_from('>II', img, PDATA_START - BASE + i)
    if begin:
        pdata.append((begin, ((flags >> 8) & 0x3FFFFF) * 4))
pdata.sort()

# Debuts de fonctions probables : cibles de bl + debuts .pdata
starts = {b for b, _ in pdata}
for addr in range(TEXT_START, TEXT_END, 4):
    ins = word(addr)
    if ins >> 26 == 18 and ins & 3 == 1:
        off = ins & 0x03FFFFFC
        if off & 0x02000000:
            off -= 0x04000000
        starts.add(addr + off)

# Debuts apres du remplissage a zero
for addr in range(TEXT_START + 4, TEXT_END, 4):
    if word(addr - 4) == 0 and word(addr) != 0:
        starts.add(addr)

starts = sorted(s for s in starts if TEXT_START <= s < TEXT_END)

# Tables de sauts : base (adresse du bctr) -> labels
tables = {}
for block in toml.split('[[switch]]')[1:]:
    base = int(re.search(r'base = (0x[0-9A-Fa-f]+)', block).group(1), 16)
    labels = [int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]+', block.split('labels', 1)[1])]
    tables[base] = labels


def next_start(addr):
    for s in starts:
        if s > addr:
            return s
    return TEXT_END


def prev_start(addr):
    best = TEXT_START
    for s in starts:
        if s > addr:
            break
        best = s
    return best


def table_for_bctr(bctr):
    # Le log donne l'adresse du bctr ; la table commence quelques instructions avant
    candidates = [b for b in tables if bctr - 0x80 <= b <= bctr]
    return tables[max(candidates)] if candidates else None


results = {}
for sw in bad:
    labels = table_for_bctr(sw)
    if labels is None:
        print(f'# {sw:#x}: table introuvable', file=sys.stderr)
        continue
    start = prev_start(sw)
    end = next_start(sw)
    # Etendre jusqu'a couvrir toutes les cibles du switch
    while max(labels) >= end and end < TEXT_END:
        end = next_start(end)
    while min(labels) < start and start > TEXT_START:
        start = prev_start(start - 4)
    # Retirer le remplissage final
    while end > start and word(end - 4) == 0:
        end -= 4
    results[start] = max(results.get(start, 0), end - start)

for start in sorted(results):
    print(f'    {{ address = {start:#X}, size = {results[start]:#X} }},'.replace('0X', '0x'))
print(f'# {len(results)} fonctions pour {len(bad)} switch', file=sys.stderr)
