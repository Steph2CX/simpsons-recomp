#!/usr/bin/env python3
"""Message shown during movies ("Appuyez sur Échap ou sur Select / pour passer la vidéo"), drawn by the game itself.

tools/texte_video.sh makes the game write the message instead of "Appuyez sur START." on its title screen (temporary
copy of the texts, never the installed game) and captures that screen in 5K over a pulse of the text. This script cuts
the text out of each capture (white letters, black outline, over the beige clouds), keeps the capture where the pulse
is at its largest, and writes simpsons/runtime/texte_video.h: one premultiplied RGBA image per language, its place on
the title screen and the pulse measured on the captures (renderer.cpp draws it the same way during movies).

Usage: texte_video.py <out.h> <language>:<capture.ppm>,<capture.ppm>... [<language>:...]   (first language = default)
"""
import base64
import sys

import numpy as np


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


def text_box(image):
    """Box of the outline's dark pixels in the lower quarter of the title screen (the logo is above)."""
    top = image.shape[0] * 3 // 4
    dark = image[top:].mean(axis=2) < 40
    ys, xs = np.nonzero(dark)
    return xs.min(), ys.min() + top, xs.max() + 1, ys.max() + 1 + top


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


def box_blur(values, radius):
    """Mean over (2 radius + 1)^2 pixels, edges clamped (summed area table)."""
    padded = np.pad(values, ((radius + 1, radius), (radius + 1, radius)), mode='edge').astype(np.float64)
    table = padded.cumsum(axis=0).cumsum(axis=1)
    size = 2 * radius + 1
    total = table[size:, size:] - table[:-size, size:] - table[size:, :-size] + table[:-size, :-size]
    return total / (size * size)


def cut_out(region):
    """Premultiplied RGBA of the text: outline and letters opaque, the outline's outer edge blended with the clouds."""
    pixels = region.astype(np.float64)
    luma = pixels @ [0.299, 0.587, 0.114]
    chroma = pixels.max(axis=2) - pixels.min(axis=2)
    letters = (chroma < 6) & (luma > 200)          # neutral light grey
    core = letters | (luma < 60)                  # letters and the outline's middle
    # Background behind the text: the clouds (coloured) around it, spread over the text (normalized blur)
    clouds = (chroma >= 10) & (luma > 110) & ~dilate(core, 3)
    weight = clouds.astype(np.float64)
    background = np.stack([box_blur(pixels[..., c] * weight, 24) for c in range(3)], axis=2)
    background /= np.maximum(box_blur(weight, 24), 1e-6)[..., None]
    # Coverage: 1 on the text, from the darkening of the clouds on the outline's outer edge (the outline is black)
    background_luma = background @ [0.299, 0.587, 0.114]
    alpha = np.clip(1.0 - luma / np.maximum(background_luma, 1.0), 0.0, 1.0)
    alpha[dilate(letters, 3) | (luma < 60)] = 1.0
    alpha[~dilate(core, 8) | (alpha < 0.05)] = 0.0
    # pixel = alpha * text + (1 - alpha) * clouds  ->  premultiplied text = pixel - (1 - alpha) * clouds
    color = np.clip(pixels - (1.0 - alpha[..., None]) * background, 0.0, 255.0)
    color = np.minimum(color, alpha[..., None] * 255.0)
    rgba = np.concatenate([color, alpha[..., None] * 255.0], axis=2)
    return np.rint(rgba).astype(np.uint8)


def encode(rgba):
    """Run-length pairs [count][R G B A] (count 1-255), base64."""
    flat = rgba.reshape(-1, 4)
    keys = flat.astype(np.uint32) @ np.array([1 << 24, 1 << 16, 1 << 8, 1], dtype=np.uint32)
    out = bytearray()
    i, n = 0, len(keys)
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


def pulse(widths, frame_step):
    """Period (s) of the text's pulse, from its widths on captures frame_step frames apart (least-squares sine)."""
    w = np.array(widths, dtype=np.float64)
    t = np.arange(len(w)) * frame_step / 29.97
    best = None
    for period in np.arange(0.3, 2.0, 0.005):
        basis = np.stack([np.ones_like(t), np.sin(2 * np.pi * t / period), np.cos(2 * np.pi * t / period)], axis=1)
        coefficients = np.linalg.lstsq(basis, w, rcond=None)[0]
        error = ((basis @ coefficients - w) ** 2).sum()
        if best is None or error < best[0]:
            best = (error, period)
    return best[1]


def main():
    target = sys.argv[1]
    entries = []
    pulse_shape = None
    for argument in sys.argv[2:]:
        language, files = argument.split(':', 1)
        captures = [read_ppm(f) for f in files.split(',')]
        boxes = [text_box(c) for c in captures]
        widths = [b[2] - b[0] for b in boxes]
        largest = int(np.argmax(widths))
        image = captures[largest]
        x0, y0, x1, y1 = boxes[largest]
        margin = 6
        x0, y0, x1, y1 = x0 - margin, y0 - margin, x1 + margin, y1 + margin
        rgba = cut_out(image[y0:y1, x0:x1])
        height, width = image.shape[:2]
        if pulse_shape is None:
            # Smallest scale and period of the pulse (first language's captures, 2 frames apart)
            pulse_shape = (height, min(widths) / max(widths), pulse(widths, 2))
        encoded = encode(rgba)
        center_x, center_y = (x0 + x1) / 2 / width, (y0 + y1) / 2 / height
        print(f'{language}: {x1 - x0}x{y1 - y0}, centre {center_x:.4f} {center_y:.4f}, largest capture {largest + 1}/'
              f'{len(captures)}, widths {min(widths)}-{max(widths)}, {len(encoded)} base64 characters')
        lines = [encoded[i:i + 112] for i in range(0, len(encoded), 112)]
        entries.append(f'    {{ "{language}", {x1 - x0}, {y1 - y0}, {center_x:.4f}f, {center_y:.4f}f,\n'
                       + '\n'.join(f'        "{l}"' for l in lines) + ' },')

    capture_height, smallest, period = pulse_shape
    header = f'''// Generated by tools/texte_video.py from captures of the game's title screen (tools/texte_video.sh)
#pragma once

// Run-length pairs [count][R G B A], base64; premultiplied alpha
struct SkipText
{{
    const char* language;    // folder of the movies (movies\\<language>\\...)
    unsigned width, height;  // at the largest of the pulse, in pixels of a SKIP_TEXT_CAPTURE_HEIGHT-pixel picture
    float centerX, centerY;  // where the title screen shows it (fractions of the picture)
    const char* runs;
}};

// The first one is used for other languages
constexpr SkipText SKIP_TEXTS[] = {{
{chr(10).join(entries)}
}};

// Height of the captured picture; pulse of the title screen's text: it grows and shrinks around its centre between
// SKIP_TEXT_SMALLEST and 1 of its size, at a constant speed, once every SKIP_TEXT_PERIOD seconds
constexpr float SKIP_TEXT_CAPTURE_HEIGHT = {capture_height}.0f;
constexpr float SKIP_TEXT_SMALLEST = {smallest:.4f}f, SKIP_TEXT_PERIOD = {period:.3f}f;
'''
    open(target, 'w').write(header)


main()
