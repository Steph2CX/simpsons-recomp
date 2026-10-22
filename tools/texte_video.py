#!/usr/bin/env python3
"""Message shown during movies ("Appuyez sur Échap ou sur Select / pour passer la vidéo"), in one of the game's clouds.

tools/texte_video.sh makes the game write the message instead of its "Nouvel objectif !" hint (temporary copy of the
texts, never the installed game), which it shows in a translucent cloud at the start of Land of Chocolate, and captures
that moment twice: with the cloud, and with SIMPSONS_SANS_BULLES=1 (same picture without it). This script takes the
cloud and its navy text out of each pair and writes simpsons/runtime/texte_video.h: one premultiplied RGBA image per
language (renderer.cpp draws it during movies).

Every pixel of the cloud is the game's colour F = cloud + t (text - cloud), 0 <= t <= 1, laid with coverage alpha over
the picture without it: with - without = alpha (cloud - without) + alpha t (text - cloud), solved per pixel.

Usage: texte_video.py <out.h> <language>:<with.ppm>,<without.ppm> [<language>:...]   (first language = default)
"""
import base64
import sys

import numpy as np

# Pulse of the title screen's "Appuyez sur START." (measured on it, tools/texte_video.sh before the clouds): the
# message grows and shrinks around its centre between SMALLEST and 1 of its size, once every PERIOD seconds
SMALLEST, PERIOD = 0.9202, 0.640
# Width of the cloud: NARROW times the text's, at least MIN_NARROW of the game's
NARROW, MIN_NARROW = 1.4, 0.55


def read_ppm(path):
    data = open(path, 'rb').read()
    tokens, pos = [], 0
    while len(tokens) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        tokens.append(data[start:pos])
    width, height = int(tokens[1]), int(tokens[2])
    return np.frombuffer(data, dtype=np.uint8, count=width * height * 3, offset=pos + 1).reshape(height, width, 3)


def dilate(mask, radius):
    out = mask.copy()
    for _ in range(radius):
        grown = out.copy()
        grown[1:] |= out[:-1]
        grown[:-1] |= out[1:]
        grown[:, 1:] |= out[:, :-1]
        grown[:, :-1] |= out[:, 1:]
        out = grown
    return out


def erode(mask, radius):
    return ~dilate(~mask, radius)


def cut_out(with_cloud, without):
    """Box of the cloud (upper half of the picture) and its premultiplied RGBA."""
    a, b = with_cloud.astype(np.float64), without.astype(np.float64)
    changed = np.abs(a - b).max(axis=2) > 2
    changed[a.shape[0] // 2:] = False
    ys, xs = np.nonzero(changed)
    x0, x1, y0, y1 = xs.min() - 4, xs.max() + 5, ys.min() - 4, ys.max() + 5
    a, b = a[y0:y1, x0:x1], b[y0:y1, x0:x1]
    difference = a - b
    # Cloud colour and opacity: with = alpha cloud + (1 - alpha) without, on the cloud away from the (blue) text
    blue = a[..., 2] - a[..., 0]
    plain = (np.abs(difference).max(axis=2) > 10) & (blue <= 25)
    cloud, opacities = np.zeros(3), []
    for channel in range(3):
        slope, intercept = np.polyfit(b[..., channel][plain], a[..., channel][plain], 1)
        cloud[channel] = intercept / (1.0 - slope)
        opacities.append(1.0 - slope)
    opacity = float(np.mean(opacities))
    # Text colour: the bluest pixels (the middle of the letters)
    text = np.median(a[blue >= np.percentile(blue[blue > 25], 80)], axis=0)
    # Per pixel, least squares of difference = u (cloud - without) + v (text - cloud), u = alpha, v = alpha t
    m1 = cloud - b
    m2 = np.broadcast_to(text - cloud, b.shape)
    a11, a12, a22 = (m1 * m1).sum(2), (m1 * m2).sum(2), (m2 * m2).sum(2)
    r1, r2 = (m1 * difference).sum(2), (m2 * difference).sum(2)
    det = a11 * a22 - a12 * a12
    solvable = det > 1e-3
    u = np.where(solvable, (r1 * a22 - r2 * a12) / np.where(solvable, det, 1.0), r1 / np.maximum(a11, 1e-3))
    v = np.where(solvable, (a11 * r2 - a12 * r1) / np.where(solvable, det, 1.0), 0.0)
    u = np.clip(u, 0.0, 1.0)
    v = np.clip(v, 0.0, u)
    u[~(np.abs(difference).max(axis=2) > 2)] = 0.0
    v[u == 0.0] = 0.0
    error = np.abs(np.clip(u[..., None] * cloud + v[..., None] * (text - cloud), 0.0, 255.0)
                   + (1.0 - u[..., None]) * b - a).mean()
    # Two layers: the text, coverage v (premultiplied u cloud + v (text - cloud) = v text + (1 - v) u' cloud), in the
    # box of its letters (elsewhere v is noise on the cloud's edge), over the cloud, coverage u', of one opacity away
    # from its edge (where the picture without it is close to its colour, the solve is unsteady: pale streaks of the
    # scenery)
    letters = (v > 0.5) & (u > 0.5)
    rows = np.nonzero(letters.sum(axis=1) >= 3)[0]
    columns = np.nonzero(letters.sum(axis=0) >= 3)[0]
    box = np.zeros_like(letters)
    box[max(rows[0] - 6, 0):rows[-1] + 7, max(columns[0] - 6, 0):columns[-1] + 7] = True
    coverage = np.where(box, v, 0.0)
    inside = erode(u > opacity / 2, 4) | box
    u = np.where(inside, opacity, np.minimum(u, opacity))
    # The game's cloud is made for three lines: narrowed around the text (letters unchanged)
    width = u.shape[1]
    narrowed = int(np.clip((columns[-1] + 1 - columns[0]) * NARROW, MIN_NARROW * width, width))
    source = (np.arange(narrowed) + 0.5) * width / narrowed - 0.5
    u = np.stack([np.interp(source, np.arange(width), row) for row in u])
    # Text centred in the narrowed cloud
    left = (columns[0] + columns[-1] + 1) // 2 - narrowed // 2
    coverage = np.pad(coverage, ((0, 0), (max(0, -left), max(0, left + narrowed - width))))
    coverage = coverage[:, max(0, left):][:, :narrowed]
    color = np.clip(coverage[..., None] * text + (1.0 - coverage[..., None]) * u[..., None] * cloud, 0.0, 255.0)
    u = coverage + (1.0 - coverage) * u
    x0 = (x0 + x1 - narrowed) // 2
    x1 = x0 + narrowed
    rgba = np.concatenate([color, u[..., None] * 255.0], axis=2)
    return (x0, y0, x1, y1), np.rint(rgba).astype(np.uint8), cloud, text, error


def encode(rgba):
    """Run-length pairs [count][R G B A] (count 1-255), base64."""
    flat = rgba.reshape(-1, 4)
    keys = flat.astype(np.uint32) @ np.array([1 << 24, 1 << 16, 1 << 8, 1], dtype=np.uint32)
    out = bytearray()
    n = len(keys)
    changes = np.flatnonzero(np.diff(keys)) + 1
    starts = np.concatenate([[0], changes])
    ends = np.concatenate([changes, [n]])
    for start, end in zip(starts, ends):
        value = bytes(flat[start])
        length = end - start
        while length > 0:
            run = min(length, 255)
            out += bytes([run]) + value
            length -= run
    return base64.b64encode(bytes(out)).decode()


def main():
    target = sys.argv[1]
    entries = []
    capture_height = None
    for argument in sys.argv[2:]:
        language, files = argument.split(':', 1)
        with_cloud, without = (read_ppm(f) for f in files.split(','))
        (x0, y0, x1, y1), rgba, cloud, text, error = cut_out(with_cloud, without)
        height, width = with_cloud.shape[:2]
        capture_height = capture_height or height
        encoded = encode(rgba)
        center_x = (x0 + x1) / 2 / width
        print(f'{language}: {x1 - x0}x{y1 - y0}, centre {center_x:.4f}, cloud {np.rint(cloud)}, text {text}, '
              f'mean error {error:.2f}, {len(encoded)} base64 characters')
        lines = [encoded[i:i + 112] for i in range(0, len(encoded), 112)]
        entries.append(f'    {{ "{language}", {x1 - x0}, {y1 - y0}, {center_x:.4f}f,\n'
                       + '\n'.join(f'        "{l}"' for l in lines) + ' },')

    header = f'''// Generated by tools/texte_video.py from captures of one of the game's clouds (tools/texte_video.sh)
#pragma once

// Run-length pairs [count][R G B A], base64; premultiplied alpha
struct SkipText
{{
    const char* language;    // folder of the movies (movies\\<language>\\...)
    unsigned width, height;  // in pixels of a SKIP_TEXT_CAPTURE_HEIGHT-pixel picture
    float centerX;           // where the game shows the cloud (fraction of the picture's width)
    const char* runs;
}};

// The first one is used for other languages
constexpr SkipText SKIP_TEXTS[] = {{
{chr(10).join(entries)}
}};

// Height of the captured picture; pulse of the title screen's "Appuyez sur START.": the message grows and shrinks
// around its centre between SKIP_TEXT_SMALLEST and 1 of its size, at a constant speed, once every SKIP_TEXT_PERIOD
// seconds
constexpr float SKIP_TEXT_CAPTURE_HEIGHT = {capture_height}.0f;
constexpr float SKIP_TEXT_SMALLEST = {SMALLEST:.4f}f, SKIP_TEXT_PERIOD = {PERIOD:.3f}f;
'''
    open(target, 'w').write(header)


main()
