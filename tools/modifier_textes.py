#!/usr/bin/env python3
"""Remplace des expressions dans un fichier de textes du jeu (.str : SToc + RefPack + bloc LH2 "2HCL").

Usage : modifier_textes.py <source.str> <sortie.str> <remplacements.tsv>
remplacements.tsv : une ligne par remplacement, "ancien<TAB>nouveau" (UTF-8, lignes # ignorees).
Le fichier produit garde l'enveloppe d'origine ; sa taille decompressee ne doit pas depasser l'originale
(le jeu reserve une place fixe en memoire).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from refpack import compress, decompress  # noqa: E402

DATA_OFFSET = 0x800


def parse_lh2(chunk):
    base = chunk.find(b'2HCL')
    count = struct.unpack_from('>I', chunk, base + 16)[0]
    ids = struct.unpack_from(f'>{count}I', chunk, base + 0x20)
    offsets = struct.unpack_from(f'>{count}I', chunk, base + 0x20 + 4 * count)
    strings = {}
    for string_id, offset in zip(ids, offsets):
        start = base + offset
        strings[string_id] = chunk[start:chunk.index(b'\0', start)]
    return base, list(ids), strings


def build_lh2(header, ids, strings):
    count = len(ids)
    table_size = 0x20 + 8 * count
    offsets = []
    data = bytearray()
    for string_id in ids:
        offsets.append(table_size + len(data))
        data += strings[string_id] + b'\0'
    out = bytearray(header[:0x20])
    struct.pack_into('>I', out, 4, table_size + len(data))
    struct.pack_into('>I', out, 16, count)
    out += struct.pack(f'>{count}I', *ids)
    out += struct.pack(f'>{count}I', *offsets)
    out += data
    return bytes(out)


def max_output_lead(packed):
    """Plus grande avance (octets ecrits - octets lus) pendant la decompression RefPack."""
    pos = 5
    written = 0
    lead = 0
    while True:
        b0 = packed[pos]
        if b0 < 0x80:
            literals, copy = b0 & 3, ((b0 >> 2) & 7) + 3
            pos += 2
        elif b0 < 0xC0:
            literals, copy = packed[pos + 1] >> 6, (b0 & 0x3F) + 4
            pos += 3
        elif b0 < 0xE0:
            literals, copy = b0 & 3, ((b0 & 0x0C) << 6) + packed[pos + 3] + 5
            pos += 4
        elif b0 < 0xFC:
            literals, copy = ((b0 & 0x1F) << 2) + 4, 0
            pos += 1
        else:
            literals, copy = b0 & 3, 0
            pos += 1
        pos += literals
        written += literals + copy
        lead = max(lead, written - pos)
        if b0 >= 0xFC:
            return lead


def round_up(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


def main():
    source, target, table = sys.argv[1:4]
    replacements = []
    for line in open(table, encoding='utf-8'):
        if line.strip() and not line.startswith('#'):
            old, new = line.rstrip('\n').split('\t', 1)
            replacements.append((old.encode('cp1252'), new.encode('cp1252')))

    raw = open(source, 'rb').read()
    chunk, _ = decompress(raw, DATA_OFFSET)
    base, ids, strings = parse_lh2(chunk)
    changed = 0
    for string_id in ids:
        text = strings[string_id]
        for old, new in replacements:
            text = text.replace(old, new)
        if text != strings[string_id]:
            strings[string_id] = text
            changed += 1

    # Chunk = original prefix (RenderWare header, resource name and id) + new LH2 block
    lh2 = build_lh2(chunk[base:base + 0x20], ids, strings)
    new_chunk = bytearray(chunk[:base]) + lh2
    struct.pack_into('<I', new_chunk, 4, len(new_chunk) - 0xC)  # RenderWare chunk size (little-endian)
    struct.pack_into('>I', new_chunk, base - 0xC, len(lh2))  # LH2 size in the resource header
    if len(new_chunk) > len(chunk):
        raise SystemExit(f'trop grand de {len(new_chunk) - len(chunk)} octets')

    packed = compress(bytes(new_chunk))
    data_size = round_up(len(packed), 0x800)
    # SToc header: compressed data loaded at load_offset in a buffer and decompressed in place
    load_offset = round_up(max_output_lead(packed), 0x20)
    header = bytearray(raw[:DATA_OFFSET])
    struct.pack_into('>4I', header, 0x1C, len(new_chunk), round_up(load_offset + data_size, 0x100), data_size, load_offset)
    output = bytes(header) + packed + b'\0' * (data_size - len(packed))

    _, check_ids, check = parse_lh2(decompress(output, DATA_OFFSET)[0])
    assert check_ids == ids and all(check[i] == strings[i] for i in ids), 'relecture incorrecte'
    os.makedirs(os.path.dirname(os.path.abspath(target)), exist_ok=True)
    open(target, 'wb').write(output)
    print(f'{changed} textes modifies -> {target}', file=sys.stderr)


if __name__ == '__main__':
    main()
