#!/usr/bin/env python3
# analyse_son.py <file.f32> [skip seconds]: raw 48 kHz stereo float32 recording of the speakers (parec monitor).
# Counts dropouts: runs of >= 2 ms of digital silence with sound on both sides (what crackles / cuts sound like).
import sys
from array import array

RATE = 48000
data = array('f')
with open(sys.argv[1], 'rb') as f:
    data.frombytes(f.read())
skip = int(float(sys.argv[2]) * RATE) if len(sys.argv) > 2 else 0
mono = [abs(data[i]) + abs(data[i + 1]) for i in range(skip * 2, len(data) - 1, 2)]

def loud(start, end):
    window = mono[max(0, start):max(0, min(len(mono), end))]
    return bool(window) and sum(v * v for v in window) / len(window) > 1e-5

drops, total_ms, worst, i = [], 0.0, 0.0, 0
while i < len(mono):
    if mono[i] == 0.0:
        j = i
        while j < len(mono) and mono[j] == 0.0:
            j += 1
        if j - i >= RATE // 500 and loud(i - 240, i) and loud(j, j + 240):
            ms = (j - i) * 1000 / RATE
            drops.append(((skip + i) / RATE, ms))
            total_ms += ms
            worst = max(worst, ms)
        i = j
    else:
        i += 1

seconds = len(mono) / RATE
sound = sum(1 for k in range(0, len(mono), 480) if loud(k, k + 480)) / 100
print(f"{seconds:.0f} s enregistrees ({sound:.0f} s avec du son) : {len(drops)} coupures, "
      f"{total_ms:.0f} ms au total, la plus longue {worst:.0f} ms")
for t, ms in drops[:15]:
    print(f"  a {t:7.2f} s : {ms:.0f} ms")
