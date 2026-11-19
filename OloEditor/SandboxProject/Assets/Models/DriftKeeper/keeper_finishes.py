"""Bake cloth grain, salt wear and facial pigmentation onto the shared LOD charts."""

import numpy as np
from struct import pack
from zlib import compress, crc32

FINISHES = {
    "KeeperSkin": "WeatheredSkin",
    "KeeperOilskin": "Oilcloth",
    "KeeperCap": "CapWool",
    "KeeperGansey": "Gansey",
    "KeeperLeather": "Leather",
    "KeeperTwill": "Twill",
}


def png(path, rgb):
    def chunk(tag, data):
        return pack(">I", len(data)) + tag + data + pack(">I", crc32(tag + data))

    data = np.uint8(np.clip(rgb, 0, 1) * 255 + 0.5)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", pack(">IIBBBBB", data.shape[1], data.shape[0], 8, 2, 0, 0, 0))
        + chunk(b"IDAT", compress(b"".join(b"\x00" + r.tobytes() for r in data)))
        + chunk(b"IEND", b"")
    )


def noise(rng, u, v, n):
    g = rng.normal(0, 1, (n, n))
    x, y = u * n, v * n
    i, j = x.astype(int), y.astype(int)
    x = x - i
    y = y - j
    x = x * x * (3 - 2 * x)
    y = y * y * (3 - 2 * y)
    a = g[j % n, i % n] * (1 - x) + g[j % n, (i + 1) % n] * x
    b = g[(j + 1) % n, i % n] * (1 - x) + g[(j + 1) % n, (i + 1) % n] * x
    return a * (1 - y) + b * y


def bake(out, palette):
    rng = np.random.default_rng(1871)
    n = 1024
    v, u = (np.indices((n, n)) + 0.5) / n
    for material, name in FINISHES.items():
        base, rough, _ = palette[material]
        coarse = noise(rng, u, v, 8)
        speck = noise(rng, u, v, 173)
        fine = rng.normal(0, 1, (n, n))
        warp = np.sin(2 * np.pi * (193 * u + 0.12 * np.sin(v * 94)))
        weft = np.sin(2 * np.pi * (159 * v + 0.14 * np.sin(u * 73)))
        pattern = 0.55 * warp + 0.45 * weft
        if material == "KeeperSkin":
            angle = (u - 0.5) * 2 * np.pi
            y = 1.750 - 0.232 * v
            cheeks = np.exp(-(((abs(angle) - 0.55) / 0.35) ** 2) - ((y - 1.616) / 0.024) ** 2)
            spots = np.maximum(0, speck - 1.10)
            value = 1 + 0.026 * coarse + 0.014 * fine - 0.19 * spots
            color = np.array(base) * value[..., None]
            nose = np.exp(-((angle / 0.22) ** 2) - ((y - 1.595) / 0.014) ** 2)
            flush = np.clip(cheeks + 0.65 * nose, 0, 1.4) * (0.80 + 0.20 * noise(rng, u, v, 39))
            color *= 1 + flush[..., None] * np.array([0.20, -0.16, -0.20])
            # Tiny irregular capillaries in sun-exposed cheeks. Their contrast
            # tapers into the temple instead of laying a uniform red mask.
            capillary = np.exp(-(((np.sin(26 * angle + 15 * noise(rng, u, v, 14))) / 0.105) ** 2))
            color *= 1 + (flush * capillary)[..., None] * np.array([0.07, -0.09, -0.08])
            relief = 0.45 * speck + 0.35 * fine
        elif material == "KeeperGansey":
            loops = np.cos(2 * np.pi * (54 * u + 0.24 * np.cos(2 * np.pi * 45 * v)))
            value = 1 + 0.20 * loops + 0.12 * pattern + 0.035 * fine
            relief = loops * 2 + pattern
            color = np.array(base) * value[..., None]
        else:
            wear = np.clip(0.25 * coarse + 0.19 * noise(rng, u, v, 32), 0, 0.7)
            value = 1 + 0.065 * coarse + 0.06 * speck + 0.048 * pattern + 0.025 * fine
            color = np.array(base) * value[..., None] + wear[..., None] * np.array([0.045, 0.038, 0.030])
            relief = (0.85 if material != "KeeperLeather" else 0.25) * pattern + 0.55 * speck
        # 8-bit sRGB base colour and linear tangent-space normal. The small
        # relief amplitudes survive a close-up without sparkling at distance.
        srgb = np.where(color < 0.0031308, 12.92 * color, 1.055 * np.maximum(color, 0) ** (1 / 2.4) - 0.055)
        png(out / (name + "Color.png"), srgb)
        ny, nx = np.gradient(relief)
        normal = np.stack((-0.11 * nx, 0.11 * ny, np.ones_like(nx)), axis=-1)
        normal /= np.linalg.norm(normal, axis=-1)[..., None]
        png(out / (name + "Normal.png"), normal * 0.5 + 0.5)


def dress_materials(file):
    images = file.doc.setdefault("images", [])
    tex = file.doc.setdefault("textures", [])

    def texture(name):
        images.append(dict(uri=name + ".png"))
        tex.append(dict(source=len(images) - 1))
        return dict(index=len(tex) - 1)

    for mat in file.doc["materials"]:
        name = mat["name"]
        if name in FINISHES:
            pbr = mat["pbrMetallicRoughness"]
            pbr["baseColorFactor"] = [1, 1, 1, 1]
            pbr["baseColorTexture"] = texture(FINISHES[name] + "Color")
            mat["normalTexture"] = texture(FINISHES[name] + "Normal")
        if name == "KeeperGlass":
            mat["alphaMode"] = "BLEND"
            pbr = mat["pbrMetallicRoughness"]
            pbr.update(baseColorFactor=[0.76, 0.72, 0.57, 0.35], metallicFactor=0, roughnessFactor=0.13)
            mat["extensions"] = {
                "KHR_materials_transmission": {"transmissionFactor": 0.78},
                "KHR_materials_ior": {"ior": 1.51},
            }
            file.doc["extensionsUsed"] = ["KHR_materials_transmission", "KHR_materials_ior"]
        if name == "KeeperWick":
            mat["emissiveFactor"] = [1, 0.41, 0.065]
