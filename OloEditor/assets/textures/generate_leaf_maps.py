#!/usr/bin/env python3
"""Generate the leaf surface maps that go with grass.png — issue #1234.

Three 512x512 maps, matching grass.png's resolution and UV layout, for a foliage
layer's Normal Map / Roughness Map / Thickness Map:

  leaf_normal.png     tangent-space normals: each blade a shallow trough
                      running along its length. Blue-dominant, LINEAR (never sRGB).
  leaf_roughness.png  greyscale, red channel read. Dry and rough at the tips,
                      slightly waxier down a blade's spine near the root.
  leaf_thickness.png  greyscale, red channel read. THE ONE THAT MATTERS for
                      transmission: thick where the blades overlap near the
                      root, thin at the tips — so a backlit clump glows at its
                      tips and stays dark at its base, which is what real grass
                      does.

FULLY DETERMINISTIC — closed-form functions of (u, v), no RNG, no seed. That is
deliberate: the outputs are committed and a visual-evidence test renders them,
so a generator that drifted between runs would move a golden with no source
change (see docs/agent-rules/procedural-generator-golden-coupling.md).

No third-party dependencies: the PNG writer below is stdlib zlib + struct.

    python OloEditor/assets/textures/generate_leaf_maps.py
"""

import math
import os
import struct
import zlib

SIZE = 512
HERE = os.path.dirname(os.path.abspath(__file__))


def write_png(path, width, height, channels, rows):
    """rows: list of bytes objects, one per scanline, already `channels` wide."""
    color_type = {1: 0, 3: 2, 4: 6}[channels]
    raw = b"".join(b"\x00" + row for row in rows)  # filter type 0 per scanline

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, color_type, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)
    print(f"  {os.path.basename(path)}  {width}x{height}x{channels}  {len(png)} bytes")


def clamp01(x):
    return 0.0 if x < 0.0 else (1.0 if x > 1.0 else x)


# grass.png is a CLUMP: blades sprout from the bottom centre and fan out, over a
# fully transparent background. So the maps are built in the clump's own polar
# frame rather than on a single-leaf midrib — a centre-line model would put a
# thick spine straight up the middle of the empty sky between two blades.
ORIGIN_U = 0.5
ORIGIN_V = 1.02   # just below the image, where the blades converge
BLADE_COUNT = 26.0


def polar(u, v):
    """(radius 0..~1 from the clump root, blade phase in -1..1 across a blade)."""
    du = (u - ORIGIN_U) * 2.0
    dv = (ORIGIN_V - v)
    r = math.sqrt(du * du + dv * dv)
    theta = math.atan2(du, max(dv, 1e-4))     # 0 straight up, +- towards the sides
    phase = math.sin(theta * BLADE_COUNT)     # one full cycle per blade
    return clamp01(r), phase


def main():
    normal_rows, rough_rows, thick_rows = [], [], []

    for y in range(SIZE):
        v = y / (SIZE - 1.0)          # 0 at the top (tips), 1 at the bottom (root)
        nrow = bytearray()
        rrow = bytearray()
        trow = bytearray()
        for x in range(SIZE):
            u = x / (SIZE - 1.0)
            r, phase = polar(u, v)
            # 1 at the root, 0 at the tips.
            root = clamp01(1.0 - r)
            # 1 along a blade's spine, 0 at its margins.
            spine = clamp01(1.0 - abs(phase))

            # ---- thickness: THE transmission driver, and the shape that makes
            # a backlit clump read correctly. Thick where blades overlap near
            # the root, thin at the tips; each blade a little thicker down its
            # spine than at its margins. A backlit clump then glows at the tips
            # and stays dark at the base, which is what real grass does.
            th = 0.16 + 0.62 * (root * root) + 0.22 * spine * (0.35 + 0.65 * root)
            trow.append(int(round(clamp01(th) * 255.0)))

            # ---- roughness: dry, rough tips; slightly waxier down a blade's
            # spine near the root. Multiplies the layer's authored Roughness, so
            # it stays near 1 and never turns a blade into a mirror.
            rg = 0.78 + 0.20 * (1.0 - root) - 0.12 * spine * root
            rrow.append(int(round(clamp01(rg) * 255.0)))

            # ---- normal: each blade is a shallow trough/ridge running along its
            # length, so the slope is ACROSS the blade — the derivative of the
            # phase, which is cos(theta * BLADE_COUNT).
            across = math.cos(math.atan2((u - ORIGIN_U) * 2.0, max(ORIGIN_V - v, 1e-4)) * BLADE_COUNT)
            nx = across * 0.38 * (0.35 + 0.65 * root)
            ny = -0.10 * (1.0 - root)     # tips lean away from the root
            nz = math.sqrt(max(1e-4, 1.0 - nx * nx - ny * ny))
            inv = 1.0 / math.sqrt(nx * nx + ny * ny + nz * nz)
            nrow += bytes((int(round((nx * inv * 0.5 + 0.5) * 255.0)),
                           int(round((ny * inv * 0.5 + 0.5) * 255.0)),
                           int(round((nz * inv * 0.5 + 0.5) * 255.0))))

        normal_rows.append(bytes(nrow))
        rough_rows.append(bytes(rrow))
        thick_rows.append(bytes(trow))

    print("Writing leaf maps (issue #1234):")
    write_png(os.path.join(HERE, "leaf_normal.png"), SIZE, SIZE, 3, normal_rows)
    write_png(os.path.join(HERE, "leaf_roughness.png"), SIZE, SIZE, 1, rough_rows)
    write_png(os.path.join(HERE, "leaf_thickness.png"), SIZE, SIZE, 1, thick_rows)


if __name__ == "__main__":
    main()
