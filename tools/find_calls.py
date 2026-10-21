import struct, sys
img = open(sys.argv[1], 'rb').read()
base, text_start, text_end = 0x82000000, 0x82230000, 0x82230000 + 0xA93CE4
target = int(sys.argv[2], 16)
for addr in range(text_start, text_end, 4):
    (ins,) = struct.unpack_from('>I', img, addr - base)
    if ins >> 26 == 18 and ins & 3 == 1:  # bl
        off = ins & 0x03FFFFFC
        if off & 0x02000000:
            off -= 0x04000000
        if addr + off == target:
            print(hex(addr))
