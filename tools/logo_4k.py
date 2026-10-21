"""Builds the replacement of the "HD" image of the start screen ("EA | HD" -> "EA | 5K ULTRA HD").

The original is a 128x64 DXT3 (BC2) texture, white with alpha, whose decoded data has the XXH3 hash
0E70C115A64CE986. The new text is drawn by tools/logo_4k.swift (8-bit alpha), then encoded as BC2: explicit
4-bit alpha, white where visible and black where transparent (like the original), written to
simpsons/modifications/textures/0E70C115A64CE986.bin (read by runtime/textures.cpp).
Usage: python3 tools/logo_4k.py
"""
import os
import struct
import subprocess
import sys
import tempfile

WIDTH, HEIGHT = 128, 64
HASH = "0E70C115A64CE986"

tools = os.path.dirname(os.path.abspath(__file__))
output = os.path.join(tools, "..", "simpsons", "modifications", "textures", HASH + ".bin")

with tempfile.TemporaryDirectory() as folder:
    alpha_path = os.path.join(folder, "logo.alpha")
    subprocess.run(["swift", os.path.join(tools, "logo_4k.swift"), alpha_path], check=True)
    alpha = open(alpha_path, "rb").read()
if len(alpha) != WIDTH * HEIGHT:
    sys.exit("taille inattendue du dessin")

blocks = bytearray()
for block_y in range(0, HEIGHT, 4):
    for block_x in range(0, WIDTH, 4):
        pixels = [alpha[(block_y + y) * WIDTH + block_x + x] for y in range(4) for x in range(4)]
        alpha_bits = 0
        color_indices = 0
        for i, value in enumerate(pixels):
            alpha_bits |= ((value * 15 + 127) // 255) << (4 * i)
            color_indices |= (0 if value > 0 else 1) << (2 * i)  # 0 = white (color 0), 1 = black (color 1)
        blocks += struct.pack("<QHHI", alpha_bits, 0xFFFF, 0x0000, color_indices)

os.makedirs(os.path.dirname(output), exist_ok=True)
with open(output, "wb") as file:
    file.write(blocks)
print("ecrit :", os.path.normpath(output), len(blocks), "octets")
