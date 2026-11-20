"""Bake cloth grain, salt wear and facial pigmentation onto the shared LOD charts."""

from pathlib import Path
import numpy as np
from struct import pack
from zlib import compress, crc32

from keeper_face import mouth_seam, skull_section

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


SCANS = {
    "KeeperCap": "poly_wool_herringbone",
    "KeeperLeather": "brown_leather",
    "KeeperOilskin": "denim_fabric_04",
    "KeeperTwill": "denim_fabric_04",
}


def scan_channels(archive, key, size):
    # The colour scan supplies only luminance, allowing cloth dye and leather
    # dressing to be authored independently. The source aspect ratios vary.
    luma = archive[key + "_value"]
    rows = np.minimum((np.arange(size) + 0.5) * luma.shape[0] / size, luma.shape[0] - 1).astype(int)
    cols = np.minimum((np.arange(size) + 0.5) * luma.shape[1] / size, luma.shape[1] - 1).astype(int)
    select = np.ix_(rows, cols)
    values = luma[select].astype(float) / 255
    xy = archive[key + "_xy"][select].astype(float) / 127.5 - 1
    return values, xy, archive[key + "_rough"][select].astype(float) / 255


def knit_yarn(u, v):
    # Twist two helical legs around each other into a V-shaped knit loop. The
    # diamond repeats in raised purl stitches across the front of the gansey.
    col = u * 64
    row = v * 77
    xx = col % 1 - 0.5
    yy = row % 1
    diamond = abs((col / 16) % 1 - 0.5) * 2 + abs((row / 16) % 1 - 0.5) * 2
    purl = (abs(diamond - 1) < 0.145) | ((row.astype(int) % 38) < 2)
    axis = 0.075 + 0.29 * yy
    leg = np.exp(-(((abs(xx) - axis) / 0.155) ** 2))
    bridge = np.exp(-(((yy - 0.14) / 0.15) ** 2)) * np.exp(-((xx / 0.36) ** 4))
    horizontal = np.exp(-(((yy - 0.46 - 0.12 * np.cos(2 * np.pi * xx)) / 0.21) ** 2))
    yarn = np.where(purl, horizontal, np.maximum(leg, bridge))
    twist = np.cos(2 * np.pi * (2.2 * yy + 2.9 * abs(xx)))
    return yarn, twist


def bake(out, palette):
    rng = np.random.default_rng(1871)
    n = 1024
    v, u = (np.indices((n, n)) + 0.5) / n
    source = Path(__file__).with_name("SurfaceSources.zip")
    with np.load(source, allow_pickle=False) as archive:
        scans = {k: scan_channels(archive, k, n) for k in set(SCANS.values())}
    for material, name in FINISHES.items():
        base, rough, _ = palette[material]
        coarse = noise(rng, u, v, 8)
        speck = noise(rng, u, v, 173)
        fine = rng.normal(0, 1, (n, n))
        if material == "KeeperSkin":
            angle = (u - 0.5) * 2 * np.pi
            y = 1.750 - 0.232 * v
            cheeks = np.exp(-(((abs(angle) - 0.55) / 0.35) ** 2) - ((y - 1.616) / 0.024) ** 2)
            widths = np.array([skull_section(height)[0] for height in y[:, 0]])
            x = widths[:, None] * np.sin(angle)
            ax = np.abs(x)
            nose = np.exp(-((angle / 0.215) ** 2) - ((y - 1.595) / 0.017) ** 2)
            face = np.clip(1.8 * np.cos(angle), 0, 1)
            mids = noise(rng, u, v, 42)
            pores = noise(rng, u, v, 261)
            sun = np.clip(0.23 + cheeks + 0.65 * nose, 0, 1) * face
            orbital = face * np.exp(-(((ax - 0.032) / 0.019) ** 2) - ((y - 1.626) / 0.013) ** 2)
            redness = sun * (0.64 + 0.20 * mids + 0.13 * coarse)
            spots = np.clip((speck - 1.04) * 1.12, 0, 1) * (0.35 + 0.65 * sun)
            # Blood colour is concentrated beside the nose and over the cheek
            # bones; a cooler, more transparent band follows the lower orbits.
            color = (
                np.array(base)
                * np.array([1.0, 1.10, 1.16])
                * (1 + 0.047 * coarse + 0.028 * mids + 0.011 * fine)[..., None]
            )
            color *= 1 + redness[..., None] * np.array([0.16, -0.13, -0.13])
            color *= 1 + orbital[..., None] * np.array([-0.145, -0.135, -0.075])
            color *= 1 - spots[..., None] * np.array([0.20, 0.32, 0.37])
            capillary = np.exp(-((np.sin(35 * angle + 8 * noise(rng, u, v, 19)) / 0.11) ** 2))
            color *= 1 + (redness * capillary)[..., None] * np.array([0.045, -0.10, -0.10])
            # Wrinkles are discontinuous folds in relief. Their lengths follow
            # the brow and orbicularis rather than repeating around the skull.
            furrows = np.zeros_like(u)
            for cy in (1.665, 1.673, 1.683):
                trace = cy + 0.13 * ax + 0.0019 * np.sin(x * 68) + 0.00055 * mids
                furrows += np.exp(-(((y - trace) / 0.00068) ** 2) - ((ax - 0.028) / 0.028) ** 2)
            for off, slope in ((-0.003, -0.44), (0.0004, -0.09), (0.0035, 0.29)):
                trace = 1.633 + off + slope * (ax - 0.049) + 0.00050 * mids
                furrows += np.exp(-(((y - trace) / 0.00062) ** 2) - ((ax - 0.052) / 0.010) ** 2)
            for side in (-1, 1):
                ridge = side * 0.005 + 0.15 * (y - 1.653) * side + 0.0008 * mids
                furrows += 0.72 * np.exp(-(((x - ridge) / 0.0008) ** 2) - ((y - 1.654) / 0.008) ** 2)
            furrows *= face * np.clip(0.72 + 0.35 * mids, 0.18, 1.2)
            color *= 1 - 0.052 * furrows[..., None]
            # Soft feathered vermilion and vertical cracks share the face's UV
            # coordinates across the lip-material boundary.
            seam = mouth_seam(x)
            lip = face * np.exp(
                -(((y - seam) / (0.0042 + 0.0012 * np.exp(-(((ax - 0.008) / 0.010) ** 2)))) ** 4)
                - (ax / 0.025) ** 6
            )
            color *= 1 + lip[..., None] * np.array([-0.17, -0.39, -0.36])
            cracks = lip * np.maximum(0, np.cos(2700 * x + 2.5 * mids) - 0.4)
            relief = 0.65 * speck + 0.45 * pores + 0.16 * fine - 2.6 * furrows - 0.85 * cracks
            # Sun exposure dries the cheeks; a tighter specular highlight
            # follows the bridge, alae and lip margins.
            gloss = np.clip(
                0.54 + 0.035 * mids + 0.035 * speck + 0.025 * cheeks - 0.18 * nose - 0.20 * lip, 0.29, 0.70
            )
            ao = np.clip(0.99 - 0.09 * np.maximum(furrows, 0) - 0.055 * cracks, 0.79, 1)
            scan_xy = 0.0
        elif material == "KeeperGansey":
            yarn, twist = knit_yarn(u, v)
            color = (
                np.array([0.024, 0.043, 0.052])
                * (0.78 + 0.29 * yarn + 0.052 * twist * yarn + 0.042 * speck + 0.036 * coarse)[..., None]
            )
            relief = 3.8 * yarn + 0.55 * twist * yarn + 0.35 * fine
            gloss = np.clip(0.89 + 0.045 * yarn + 0.028 * speck, 0.76, 0.98)
            ao = 0.84 + 0.15 * yarn
            scan_xy = 0.0
        else:
            scan, xy, roughness = scans[SCANS[material]]
            grain = np.clip((scan - scan.mean()) / max(0.03, scan.std()), -3, 3)
            # Thick oil fills the twill valleys. Salt-bloom and high rubbed
            # fibres scatter light while sheltered wax retains a broad sheen.
            if material == "KeeperOilskin":
                salt = np.clip(0.39 * coarse + 0.19 * noise(rng, u, v, 33) - 0.16, 0, 0.85)
                scuff = np.maximum(grain - 0.38, 0) * (0.13 + 0.87 * salt)
                colour_scale = 0.91 + 0.069 * grain + 0.060 * coarse + 0.040 * speck
                color = (
                    np.array(base) * colour_scale[..., None]
                    + scuff[..., None] * [0.043, 0.036, 0.023]
                    + salt[..., None] * [0.055, 0.046, 0.034]
                )
                gloss = np.clip(
                    0.36 + 0.17 * salt + 0.075 * scuff + 0.045 * (roughness - 0.5) + 0.018 * speck, 0.28, 0.72
                )
                scan_xy = 0.53 * xy
                ao = np.clip(0.98 + 0.035 * grain, 0.85, 1)
            elif material == "KeeperLeather":
                polish = np.clip(0.55 + 0.28 * coarse - 0.19 * grain, 0, 1)
                color = np.array(base) * (0.93 + 0.12 * grain + 0.07 * coarse)[..., None]
                color += np.maximum(grain - 1, 0)[..., None] * [0.025, 0.014, 0.008]
                gloss = np.clip(0.51 - 0.19 * polish + 0.15 * roughness + 0.024 * speck, 0.27, 0.67)
                ao = np.clip(0.97 + 0.048 * grain, 0.79, 1)
                scan_xy = 0.86 * xy
            else:
                color = np.array(base) * (0.98 + 0.17 * grain + 0.035 * coarse)[..., None]
                gloss = np.clip(0.76 + 0.13 * roughness + 0.033 * speck, 0.66, 0.98)
                ao = np.clip(0.97 + 0.038 * grain, 0.81, 1)
                scan_xy = 0.87 * xy
            relief = 0.20 * speck + 0.12 * fine
        # 8-bit sRGB base colour and linear tangent-space normal. The small
        # relief amplitudes survive a close-up without sparkling at distance.
        srgb = np.where(color < 0.0031308, 12.92 * color, 1.055 * np.maximum(color, 0) ** (1 / 2.4) - 0.055)
        png(out / (name + "Color.png"), srgb)
        ny, nx = np.gradient(relief)
        normal = np.stack((-0.11 * nx, 0.11 * ny, np.ones_like(nx)), axis=-1)
        normal[:, :, :2] += scan_xy
        normal /= np.linalg.norm(normal, axis=-1)[..., None]
        png(out / (name + "Normal.png"), normal * 0.5 + 0.5)
        png(out / (name + "ORM.png"), np.stack((ao, gloss, np.zeros_like(gloss)), axis=-1))


def dress_materials(file):
    images = file.doc.setdefault("images", [])
    tex = file.doc.setdefault("textures", [])

    slots = {}

    def texture(name):
        if name not in slots:
            images.append(dict(uri=name + ".png"))
            tex.append(dict(source=len(images) - 1))
            slots[name] = len(tex) - 1
        return dict(index=slots[name])

    finishes = {**FINISHES, "KeeperLip": "WeatheredSkin", "KeeperSeam": "Oilcloth"}
    for mat in file.doc["materials"]:
        name = mat["name"]
        if name in finishes:
            finish = finishes[name]
            tint = [0.72, 0.68, 0.66, 1] if name == "KeeperSeam" else [1, 1, 1, 1]
            orm = texture(finish + "ORM")
            mat["pbrMetallicRoughness"].update(
                baseColorFactor=tint,
                roughnessFactor=1,
                baseColorTexture=texture(finish + "Color"),
                metallicRoughnessTexture=orm,
            )
            mat["normalTexture"] = texture(finish + "Normal")
            mat["occlusionTexture"] = dict(orm, strength=0.72)
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
