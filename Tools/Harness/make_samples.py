#!/usr/bin/env python3
"""Generates synthetic test images for the harness (no dependencies beyond macOS Python).

Writes into Tools/Harness/samples/:
  scene.png  - fake portrait: sharp "subject" over a blurry, darker background
  depth.png  - matching depth map (white = near)
"""
import math
import os
import struct
import zlib

W, H = 960, 540
OUT = os.path.join(os.path.dirname(__file__), "samples")
os.makedirs(OUT, exist_ok=True)


def png(path, rows):
    raw = b"".join(b"\x00" + bytes(r) for r in rows)
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    open(path, "wb").write(data)


def smoothstep(a, b, x):
    t = min(1.0, max(0.0, (x - a) / (b - a)))
    return t * t * (3 - 2 * t)


scene, depth = [], []
cx, cy = W * 0.5, H * 0.52
for y in range(H):
    srow, drow = [], []
    for x in range(W):
        # background: soft gradient with a window-like glow, dim
        bg = 40 + 70 * (1 - y / H) + 40 * math.exp(-(((x - W * 0.22) / 120) ** 2 + ((y - H * 0.3) / 90) ** 2))
        # subject: head + shoulders silhouette with texture (so "focus" cue sees detail)
        head = math.hypot((x - cx) / 105, (y - (cy - 110)) / 135)
        body = math.hypot((x - cx) / 260, (y - (cy + 190)) / 190)
        m = max(1 - smoothstep(0.92, 1.0, head), 1 - smoothstep(0.92, 1.0, body))
        eyes = 90 * math.exp(-(((x - cx - 40) / 14) ** 2 + ((y - cy + 125) / 8) ** 2)) + 90 * math.exp(-(((x - cx + 40) / 14) ** 2 + ((y - cy + 125) / 8) ** 2)) + 80 * math.exp(-(((x - cx) / 30) ** 2 + ((y - cy + 60) / 8) ** 2))
        tex = 185 - eyes + 18 * math.sin(x * 0.09 + y * 0.05)
        shade = 0.55 + 0.45 * (1 - head if head < 1 else 0.6)
        v = bg * (1 - m) + tex * shade * m
        v = max(0, min(255, int(v)))
        srow += [v, v, v]
        # depth: subject near (bright), gentle floor gradient behind it
        d = (0.25 + 0.2 * (y / H)) * (1 - m) + (0.75 + 0.25 * (1 - head if head < 1 else 0.3)) * m
        d = max(0, min(255, int(d * 255)))
        drow += [d, d, d]
    scene.append(srow)
    depth.append(drow)

png(os.path.join(OUT, "scene.png"), scene)
png(os.path.join(OUT, "depth.png"), depth)
print("wrote", OUT)
