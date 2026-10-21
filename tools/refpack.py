"""Compression RefPack (EA, signature 0x10FB) : decompression et compression simple.

Usage: refpack.py d entree sortie [decalage]   -> decompresse le bloc a `decalage` (defaut 0)
       refpack.py c entree sortie              -> compresse (litteraux + correspondances simples)
"""
import sys


def decompress(data, offset=0):
    pos = offset
    flags = data[pos]
    if data[pos + 1] != 0xFB:
        raise ValueError('signature RefPack absente')
    pos += 2
    size_bytes = 4 if flags & 0x80 else 3
    if flags & 0x01:  # compressed size present
        pos += size_bytes
    size = int.from_bytes(data[pos:pos + size_bytes], 'big')
    pos += size_bytes

    out = bytearray()
    while True:
        b0 = data[pos]
        if b0 < 0x80:  # 2 bytes: 0-3 literals, copy 3-10 from up to 1024 back
            b1 = data[pos + 1]
            pos += 2
            literals = b0 & 0x03
            copy_len = ((b0 >> 2) & 0x07) + 3
            copy_off = ((b0 & 0x60) << 3) + b1 + 1
        elif b0 < 0xC0:  # 3 bytes: copy 4-67 from up to 16384 back
            b1, b2 = data[pos + 1], data[pos + 2]
            pos += 3
            literals = b1 >> 6
            copy_len = (b0 & 0x3F) + 4
            copy_off = ((b1 & 0x3F) << 8) + b2 + 1
        elif b0 < 0xE0:  # 4 bytes: copy 5-1028 from up to 131072 back
            b1, b2, b3 = data[pos + 1], data[pos + 2], data[pos + 3]
            pos += 4
            literals = b0 & 0x03
            copy_len = ((b0 & 0x0C) << 6) + b3 + 5
            copy_off = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1
        elif b0 < 0xFC:  # literals only: 4-112
            pos += 1
            literals = ((b0 & 0x1F) << 2) + 4
            copy_len = 0
            copy_off = 0
        else:  # end: 0-3 trailing literals
            pos += 1
            literals = b0 & 0x03
            out += data[pos:pos + literals]
            pos += literals
            break

        out += data[pos:pos + literals]
        pos += literals
        for _ in range(copy_len):
            out.append(out[-copy_off])

    if len(out) != size:
        raise ValueError(f'taille {len(out)} != {size} annoncee')
    return bytes(out), pos - offset


def compress(data):
    """RefPack valide : correspondances trouvees par table de hachage sur 3 octets."""
    out = bytearray([0x10, 0xFB])
    out += len(data).to_bytes(3, 'big')
    table = {}
    pos = 0
    literal_start = 0
    n = len(data)

    def flush_literals(end, keep):
        # Emit literals in blocks of 4..112 (multiples of 4), leaving `keep` (0-3) for the next command
        nonlocal literal_start
        while end - literal_start - keep >= 4:
            count = min(112, (end - literal_start - keep) & ~3)
            out.append(0xE0 + ((count - 4) >> 2))
            out.extend(data[literal_start:literal_start + count])
            literal_start += count

    while pos + 3 <= n:
        key = data[pos:pos + 3]
        candidate = table.get(key)
        table[key] = pos
        if candidate is not None and pos - candidate <= 131072:
            length = 0
            while pos + length < n and length < 1028 and data[candidate + length] == data[pos + length]:
                length += 1
            offset = pos - candidate
            if length >= 3 and (length >= 4 or offset <= 1024) and (length >= 5 or offset <= 16384):
                pending = pos - literal_start
                flush_literals(pos, pending & 3)
                literals = pos - literal_start
                o = offset - 1
                if length <= 10 and offset <= 1024:
                    out.append(((o >> 3) & 0x60) | ((length - 3) << 2) | literals)
                    out.append(o & 0xFF)
                elif length <= 67 and offset <= 16384:
                    out.append(0x80 | (length - 4))
                    out.append((literals << 6) | (o >> 8))
                    out.append(o & 0xFF)
                else:
                    l = length - 5
                    out.append(0xC0 | ((o >> 12) & 0x10) | ((l >> 6) & 0x0C) | literals)
                    out.append((o >> 8) & 0xFF)
                    out.append(o & 0xFF)
                    out.append(l & 0xFF)
                out += data[literal_start:pos]
                for i in range(pos + 1, min(pos + length, n - 2)):
                    table[data[i:i + 3]] = i
                pos += length
                literal_start = pos
                continue
        pos += 1

    flush_literals(n, (n - literal_start) & 3)
    out.append(0xFC | (n - literal_start))
    out += data[literal_start:n]
    return bytes(out)


if __name__ == '__main__':
    mode, source, target = sys.argv[1:4]
    raw = open(source, 'rb').read()
    if mode == 'd':
        result, used = decompress(raw, int(sys.argv[4], 0) if len(sys.argv) > 4 else 0)
        print(f'{used} octets compresses -> {len(result)} octets', file=sys.stderr)
    else:
        result = compress(raw)
        check, _ = decompress(result)
        assert check == raw, 'verification de la compression echouee'
        print(f'{len(raw)} octets -> {len(result)} octets compresses (verifie)', file=sys.stderr)
    open(target, 'wb').write(result)
