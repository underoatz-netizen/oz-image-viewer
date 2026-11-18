#!/usr/bin/env python3
"""Deterministic regression test for the PikaView application icon assets.

Verifies:
  * The pinned user-supplied source (pikaview-source.png) and transparent master
    (pikaview.png) match their SHA-256 digests.
  * The legacy OZ monogram assets (ozviewer.svg, generate-ozviewer-icon.ps1)
    are retired and README.md embeds pikaview.png.
  * generate-pikaview-icon.py leaks no personal paths.
  * ozviewer.ico declares all required sizes (16, 20, 24, 32, 40, 48, 64, 128,
    256) as 32-bit RGBA PNG frames with transparent exterior corners.
  * Re-running generate-pikaview-icon.py in a scratch directory reproduces both
    pikaview.png and ozviewer.ico byte-for-byte.

Exits 77 (CTest skip) if Pillow is unavailable.
"""
from __future__ import annotations

import hashlib
import io
import os
import re
import struct
import subprocess
import sys
import tempfile

try:
    from PIL import Image
except ImportError:
    print("Pillow is not installed; skipping icon_asset_tests.")
    raise SystemExit(77)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICONS_DIR = os.path.join(ROOT, "assets", "icons")
SOURCE_PNG = os.path.join(ICONS_DIR, "pikaview-source.png")
MASTER_PNG = os.path.join(ICONS_DIR, "pikaview.png")
ICON_ICO = os.path.join(ICONS_DIR, "ozviewer.ico")
GENERATOR = os.path.join(ICONS_DIR, "generate-pikaview-icon.py")
README_MD = os.path.join(ROOT, "README.md")

SOURCE_SHA256 = "e7a63de9c678845c291f35a5f75f8d59916986d8103a4ea7c85e34cc27d5ca7e"
MASTER_SHA256 = "f50e87c0915098db246546424210a3fb8b323fdf8b8e0c26297724e4403f039d"
EXPECTED_SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)
REQUIRED_SIZES = (16, 20, 24, 32, 40, 48, 64, 256)

_failures: list[str] = []


def check(condition: bool, message: str) -> bool:
    if not condition:
        _failures.append(message)
    return bool(condition)


def sha256_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 16), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_ico(path: str):
    with open(path, "rb") as handle:
        data = handle.read()
    reserved, kind, count = struct.unpack("<HHH", data[:6])
    entries = []
    offset = 6
    for _ in range(count):
        (width, height, colors, reserved_byte, planes, bits,
         size, image_offset) = struct.unpack("<BBBBHHII", data[offset:offset + 16])
        offset += 16
        entries.append({
            "width": width if width else 256,
            "height": height if height else 256,
            "colors": colors,
            "reserved": reserved_byte,
            "planes": planes,
            "bits": bits,
            "size": size,
            "offset": image_offset,
            "payload": data[image_offset:image_offset + size],
        })
    frames = {}
    for entry in entries:
        image = Image.open(io.BytesIO(entry["payload"])).convert("RGBA")
        frames[entry["width"]] = image
    return reserved, kind, count, entries, frames


def check_sources() -> None:
    if check(os.path.isfile(SOURCE_PNG), f"missing source png: {SOURCE_PNG}"):
        digest = sha256_file(SOURCE_PNG)
        check(digest == SOURCE_SHA256,
              f"pikaview-source.png digest mismatch: expected {SOURCE_SHA256}, got {digest}")

    if check(os.path.isfile(MASTER_PNG), f"missing pikaview.png: {MASTER_PNG}"):
        digest = sha256_file(MASTER_PNG)
        check(digest == MASTER_SHA256,
              f"pikaview.png digest mismatch: expected {MASTER_SHA256}, got {digest}")
        with Image.open(MASTER_PNG) as img:
            check(img.size == (887, 887), f"pikaview.png should be 887x887, got {img.size}")
            check(img.mode == "RGBA", f"pikaview.png should be RGBA, got {img.mode}")

    check(not os.path.exists(os.path.join(ICONS_DIR, "ozviewer.svg")),
          "legacy ozviewer.svg must be retired/removed")
    check(not os.path.exists(os.path.join(ICONS_DIR, "generate-ozviewer-icon.ps1")),
          "legacy generate-ozviewer-icon.ps1 must be retired/removed")

    if check(os.path.isfile(README_MD), f"missing README.md: {README_MD}"):
        with open(README_MD, "r", encoding="utf-8") as handle:
            readme = handle.read()
        check("ozviewer.svg" not in readme, "README.md still references retired ozviewer.svg")
        check("assets/icons/pikaview.png" in readme, "README.md must embed assets/icons/pikaview.png")


def check_generator() -> None:
    if not check(os.path.isfile(GENERATOR), f"missing generator: {GENERATOR}"):
        return
    with open(GENERATOR, "r", encoding="utf-8") as handle:
        text = handle.read()
    for match in re.finditer(r"[A-Za-z]:[\\/]{1,2}", text):
        check(False, f"generator leaks absolute path: {match.group(0)}")
    for forbidden in ("Pictures", "Users\\", "AppData"):
        check(forbidden not in text, f"generator leaks personal fragment: {forbidden!r}")


def check_ico() -> None:
    if not check(os.path.isfile(ICON_ICO), f"missing ico: {ICON_ICO}"):
        return
    reserved, kind, count, entries, frames = parse_ico(ICON_ICO)
    check(reserved == 0, f"ICO reserved should be 0, got {reserved}")
    check(kind == 1, f"ICO type should be 1, got {kind}")
    check(count == len(EXPECTED_SIZES), f"ICO should declare {len(EXPECTED_SIZES)} frames, got {count}")
    widths = [e["width"] for e in entries]
    check(widths == list(EXPECTED_SIZES), f"ICO widths {widths} != {list(EXPECTED_SIZES)}")
    for req in REQUIRED_SIZES:
        check(req in frames, f"ICO missing required size {req}")
    for entry in entries:
        check(entry["width"] == entry["height"], f"non-square frame {entry['width']}x{entry['height']}")
        check(entry["bits"] == 32, f"frame {entry['width']} bpp={entry['bits']} != 32")
        check(entry["payload"].startswith(b"\x89PNG\r\n\x1a\n"),
              f"frame {entry['width']} is not PNG-encoded")
    for size, img in frames.items():
        w, h = img.size
        check((w, h) == (size, size), f"decoded frame size {(w, h)} != {(size, size)}")
        px = img.load()
        for cx, cy in ((0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1)):
            check(px[cx, cy][3] == 0, f"frame {size} corner ({cx},{cy}) is not transparent: alpha={px[cx, cy][3]}")


def check_reproducible() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        out_png = os.path.join(tmp, "pikaview.png")
        out_ico = os.path.join(tmp, "ozviewer.ico")
        proc = subprocess.run(
            [sys.executable, GENERATOR, "--source", SOURCE_PNG,
             "--out-png", out_png, "--out-ico", out_ico],
            capture_output=True, text=True, check=False)
        check(proc.returncode == 0,
              f"generator failed rc={proc.returncode}: {proc.stderr}")
        if proc.returncode == 0:
            check(sha256_file(out_png) == sha256_file(MASTER_PNG),
                  "regenerated pikaview.png differs from committed asset")
            check(sha256_file(out_ico) == sha256_file(ICON_ICO),
                  "regenerated ozviewer.ico differs from committed asset")


def main() -> int:
    check_sources()
    check_generator()
    check_ico()
    check_reproducible()
    if _failures:
        for msg in _failures:
            print(f"FAIL: {msg}", file=sys.stderr)
        return 1
    print("icon_asset_tests: OK (all PikaView PNG/ICO checks passed)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
