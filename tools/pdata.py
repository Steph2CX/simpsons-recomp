import struct, sys
# Usage: pdata.py image.bin addr...  -> fonction (.pdata) contenant chaque adresse
img = open(sys.argv[1], 'rb').read()
base, pstart, psize = 0x82000000, 0x821E2C00, 0x42C00
funcs = []
for i in range(0, psize, 8):
    begin, flags = struct.unpack_from('>II', img, pstart - base + i)
    if begin == 0:
        continue
    length = ((flags >> 8) & 0x3FFFFF) * 4
    funcs.append((begin, length))
funcs.sort()
print(f"{len(funcs)} fonctions dans .pdata", file=sys.stderr)
for a in sys.argv[2:]:
    a = int(a, 16)
    for begin, length in funcs:
        if begin <= a < begin + length:
            print(f"{a:#x} -> fonction {begin:#x} taille {length:#x}")
            break
    else:
        print(f"{a:#x} -> ?")
