#!/usr/bin/env python3
"""Reproducible generator for PikaView transparent PNG and multi-frame Windows ICO.

Reads assets/icons/pikaview-source.png (exact copy of user-supplied PikaView.png),
extracts exterior-connected off-white background transparency with edge decontamination,
and writes:
  - assets/icons/pikaview.png (887x887 RGBA transparent PNG)
  - assets/icons/ozviewer.ico (multi-frame 32-bit RGBA ICO: 16,20,24,32,40,48,64,128,256)
"""

from __future__ import annotations

import argparse
from collections import deque
from pathlib import Path
from PIL import Image

ICON_SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
BG_RGB = (246.0, 246.0, 246.0)
FLOOD_THRESHOLD = 38.0
SOFT_EDGE_LOW = 12.0
SOFT_EDGE_HIGH = 52.0


def extract_exterior_transparency(img: Image.Image) -> Image.Image:
    rgba = img.convert("RGBA")
    w, h = rgba.size
    px = rgba.load()

    def color_dist(r: int, g: int, b: int) -> float:
        dr = r - BG_RGB[0]
        dg = g - BG_RGB[1]
        db = b - BG_RGB[2]
        return (dr * dr + dg * dg + db * db) ** 0.5

    # BFS flood-fill from all 4 edges to find exterior-connected background pixels
    visited = [[False] * w for _ in range(h)]
    exterior = [[False] * w for _ in range(h)]
    q: deque[tuple[int, int]] = deque()

    for x in range(w):
        for y in (0, h - 1):
            if not visited[y][x]:
                r, g, b, _ = px[x, y]
                if color_dist(r, g, b) <= FLOOD_THRESHOLD:
                    visited[y][x] = True
                    exterior[y][x] = True
                    q.append((x, y))
    for y in range(h):
        for x in (0, w - 1):
            if not visited[y][x]:
                r, g, b, _ = px[x, y]
                if color_dist(r, g, b) <= FLOOD_THRESHOLD:
                    visited[y][x] = True
                    exterior[y][x] = True
                    q.append((x, y))

    while q:
        cx, cy = q.popleft()
        for nx, ny in ((cx - 1, cy), (cx + 1, cy), (cx, cy - 1), (cx, cy + 1)):
            if 0 <= nx < w and 0 <= ny < h and not visited[ny][nx]:
                visited[ny][nx] = True
                r, g, b, _ = px[nx, ny]
                if color_dist(r, g, b) <= FLOOD_THRESHOLD:
                    exterior[ny][nx] = True
                    q.append((nx, ny))

    # Dilate exterior mask by 2px to catch anti-aliased boundary pixels
    boundary = [[False] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            if exterior[y][x]:
                continue
            near_ext = False
            for dy in range(-2, 3):
                ny = y + dy
                if 0 <= ny < h:
                    for dx in range(-2, 3):
                        nx = x + dx
                        if 0 <= nx < w and exterior[ny][nx]:
                            near_ext = True
                            break
                if near_ext:
                    break
            if near_ext:
                boundary[y][x] = True

    out = rgba.copy()
    opx = out.load()
    for y in range(h):
        for x in range(w):
            if exterior[y][x]:
                opx[x, y] = (0, 0, 0, 0)
            elif boundary[y][x]:
                r, g, b, a = px[x, y]
                d = color_dist(r, g, b)
                if d <= SOFT_EDGE_LOW:
                    opx[x, y] = (0, 0, 0, 0)
                elif d < SOFT_EDGE_HIGH:
                    alpha_f = (d - SOFT_EDGE_LOW) / (SOFT_EDGE_HIGH - SOFT_EDGE_LOW)
                    alpha_f = max(0.0, min(1.0, alpha_f))
                    # Decontaminate off-white matte from edge pixel
                    nr = int(max(0, min(255, round((r - BG_RGB[0] * (1.0 - alpha_f)) / alpha_f))))
                    ng = int(max(0, min(255, round((g - BG_RGB[1] * (1.0 - alpha_f)) / alpha_f))))
                    nb = int(max(0, min(255, round((b - BG_RGB[2] * (1.0 - alpha_f)) / alpha_f))))
                    na = int(round(alpha_f * (a / 255.0) * 255.0))
                    opx[x, y] = (nr, ng, nb, na)
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate PikaView PNG and ICO assets.")
    parser.add_argument(
        "--source",
        type=Path,
        default=Path(__file__).resolve().parent / "pikaview-source.png",
    )
    parser.add_argument(
        "--out-png",
        type=Path,
        default=Path(__file__).resolve().parent / "pikaview.png",
    )
    parser.add_argument(
        "--out-ico",
        type=Path,
        default=Path(__file__).resolve().parent / "ozviewer.ico",
    )
    args = parser.parse_args()

    src = Image.open(args.source)
    transparent = extract_exterior_transparency(src)
    transparent.save(args.out_png, format="PNG")

    # Save multi-frame ICO with all requested sizes
    transparent.save(
        args.out_ico,
        format="ICO",
        sizes=[(s, s) for s in ICON_SIZES],
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
