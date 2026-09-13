#!/usr/bin/env python3
# =============================================================================
# compare_captures.py — the run-twice determinism proof for a benchmark
# capture (issues #974, #1239).
#
# A manifest's `Tolerance.RepeatRmse` is a CLAIM about reproducibility. This is
# what turns it into a measurement: capture the same manifest twice into two
# directories, diff them attachment by attachment, and print the per-attachment
# RMSE in 0..255 units alongside the tolerance the manifest declares.
#
#   OloEngine-Tests.exe --olo-capture-manifest=<m> --olo-capture-out=<dirA>
#   OloEngine-Tests.exe --olo-capture-manifest=<m> --olo-capture-out=<dirB>
#   python tools/benchmark/compare_captures.py <dirA> <dirB>
#
# Exit code is non-zero when any attachment exceeds the tolerance recorded in
# the first directory's result.json, so this can gate a determinism check
# without anyone re-reading the numbers by hand.
#
# WHY RMSE IN 0..255 AND NOT A PERCENTAGE: that is the unit the existing
# manifests' Tolerance comments are written in, and the unit the golden-image
# machinery uses. Keeping one unit means a tolerance can be compared against a
# golden threshold without a conversion nobody would remember to do.
#
# .hdr attachments are compared in their own float units and reported
# separately — clamping them into 0..255 first would hide exactly the
# out-of-range differences an HDR export exists to preserve.
# =============================================================================

import argparse
import json
import pathlib
import sys

import numpy as np
from PIL import Image


def load_png(path):
    with Image.open(path) as img:
        return np.asarray(img.convert("RGBA"), dtype=np.float64)


def load_hdr(path):
    """Minimal Radiance .hdr reader: header, resolution line, RLE scanlines.

    Written here rather than pulled from a dependency because the only thing
    this tool needs is "are these two files the same numbers", and a partial
    reader that fails loudly on an unexpected encoding is safer than a silent
    approximation.
    """
    data = path.read_bytes()
    # Header ends at a blank line; the resolution line follows.
    idx = data.find(b"\n\n")
    if idx < 0:
        raise ValueError(f"{path.name}: no header terminator")
    rest = data[idx + 2:]
    eol = rest.find(b"\n")
    res = rest[:eol].decode("ascii").split()
    if len(res) != 4 or res[0] != "-Y" or res[2] != "+X":
        raise ValueError(f"{path.name}: unsupported resolution line {res!r}")
    height, width = int(res[1]), int(res[3])
    body = rest[eol + 1:]

    out = np.zeros((height, width, 4), dtype=np.uint8)
    pos = 0
    for y in range(height):
        if pos + 4 > len(body):
            raise ValueError(f"{path.name}: truncated at scanline {y}")
        if body[pos] == 2 and body[pos + 1] == 2 and (body[pos + 2] << 8 | body[pos + 3]) == width:
            pos += 4
            for c in range(4):
                x = 0
                while x < width:
                    count = body[pos]
                    pos += 1
                    if count > 128:  # a run
                        out[y, x:x + count - 128, c] = body[pos]
                        x += count - 128
                        pos += 1
                    else:  # a literal span
                        out[y, x:x + count, c] = np.frombuffer(body[pos:pos + count], dtype=np.uint8)
                        x += count
                        pos += count
        else:  # flat (non-RLE) scanline
            out[y] = np.frombuffer(body[pos:pos + width * 4], dtype=np.uint8).reshape(width, 4)
            pos += width * 4

    rgbe = out.astype(np.float64)
    exponent = rgbe[..., 3]
    scale = np.where(exponent > 0, np.exp2(exponent - 136.0), 0.0)
    return rgbe[..., :3] * scale[..., None]


def rmse(a, b):
    if a.shape != b.shape:
        return None
    return float(np.sqrt(np.mean((a - b) ** 2)))


def compare(dir_a, dir_b):
    result_a = json.loads((dir_a / "result.json").read_text(encoding="utf-8"))
    tolerance = float(result_a.get("determinism", {}).get("repeatRmseTolerance", 0.0))
    manifest_id = result_a.get("manifest", {}).get("id", dir_a.name)

    files = sorted(p for p in dir_a.rglob("*") if p.suffix in (".png", ".hdr"))
    if not files:
        print(f"no attachments found under {dir_a}", file=sys.stderr)
        return 2

    print(f"{manifest_id}: {len(files)} attachment(s), declared RepeatRmse tolerance {tolerance}")
    print(f"  A: {dir_a}\n  B: {dir_b}\n")

    worst = 0.0
    worst_name = ""
    failures = 0
    identical = 0
    for path_a in files:
        rel = path_a.relative_to(dir_a)
        path_b = dir_b / rel
        if not path_b.exists():
            print(f"  MISSING in B: {rel}")
            failures += 1
            continue
        if path_a.read_bytes() == path_b.read_bytes():
            identical += 1
            continue
        try:
            if path_a.suffix == ".png":
                value = rmse(load_png(path_a), load_png(path_b))
                unit = "/255"
            else:
                value = rmse(load_hdr(path_a), load_hdr(path_b))
                unit = " (float radiance)"
        except Exception as exc:
            print(f"  UNREADABLE {rel}: {exc}")
            failures += 1
            continue
        if value is None:
            print(f"  SIZE MISMATCH {rel}")
            failures += 1
            continue
        over = path_a.suffix == ".png" and value > tolerance
        print(f"  {'OVER ' if over else '     '}{rel}: RMSE {value:.6f}{unit}")
        if over:
            failures += 1
        if path_a.suffix == ".png" and value > worst:
            worst, worst_name = value, str(rel)

    print(f"\n  {identical}/{len(files)} byte-identical")
    if worst_name:
        print(f"  worst PNG RMSE: {worst:.6f}/255 ({worst_name})")
    else:
        print("  every differing attachment was HDR or none differed")
    print(f"  {failures} attachment(s) outside tolerance or unreadable")
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser(description="Diff two benchmark capture result directories")
    ap.add_argument("dir_a", type=pathlib.Path)
    ap.add_argument("dir_b", type=pathlib.Path)
    args = ap.parse_args()
    return compare(args.dir_a, args.dir_b)


if __name__ == "__main__":
    sys.exit(main())
