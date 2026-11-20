"""Material maps for an island: two splatmaps (eight layers) and the macro map.

The eight layers are one shared library (`layers.py`); every island uses the same textures. What
makes a basalt stack black and a sandstone table red is the MACRO MAP: an RGBA8 image over the
whole tile that the terrain shader multiplies into the blended surface.

    albedo *= macro.rgb * (255 / 128)      byte 128 is neutral, the ceiling is about x2
    wet     = 1 - macro.a                  alpha 255 is dry; wet darkens and smooths

So the macro map carries everything that is bigger than a texture tile: the island's own rock and
sand colour, the black tide band, the pale barnacle band above it, lichen and guano, the bedding
of a sandstone cliff, damp gullies, and occlusion in the clefts.
"""
import numpy as np

from . import fields as f

LAYERS = ("Seabed", "Sand", "Shingle", "Turf", "Heath", "Rock", "RockBedded", "Scree")
SEABED, SAND, SHINGLE, TURF, HEATH, ROCK, BEDDED, SCREE = range(8)

# The mean LINEAR albedo each library layer is normalised to (layers.py). Island colours are
# stated as targets; the macro tint is target / neutral.
NEUTRAL = {
    SEABED: (0.30, 0.29, 0.24),
    SAND: (0.42, 0.37, 0.28),
    SHINGLE: (0.22, 0.21, 0.20),
    TURF: (0.095, 0.150, 0.050),
    HEATH: (0.110, 0.095, 0.060),
    ROCK: (0.20, 0.195, 0.19),
    BEDDED: (0.30, 0.22, 0.16),
    SCREE: (0.21, 0.20, 0.19),
}

# Per-island LINEAR albedo targets. Real surfaces are darker than a colour picker suggests:
# grass reflects about a tenth of the light, wet basalt a twentieth, dry beach sand two fifths.
PALETTES = {
    "Ridgeback": {SAND: (0.36, 0.31, 0.24), SHINGLE: (0.24, 0.23, 0.22), TURF: (0.085, 0.145, 0.045),
                  HEATH: (0.125, 0.090, 0.075), ROCK: (0.215, 0.205, 0.20), BEDDED: (0.215, 0.205, 0.20),
                  SCREE: (0.25, 0.24, 0.23)},
    "Sisters": {SAND: (0.34, 0.30, 0.24), SHINGLE: (0.27, 0.25, 0.23), TURF: (0.090, 0.150, 0.050),
                HEATH: (0.135, 0.105, 0.070), ROCK: (0.26, 0.225, 0.20), BEDDED: (0.26, 0.225, 0.20),
                SCREE: (0.28, 0.25, 0.22)},
    "Stacks": {SAND: (0.16, 0.15, 0.14), SHINGLE: (0.09, 0.09, 0.095), TURF: (0.075, 0.135, 0.040),
               HEATH: (0.085, 0.105, 0.045), ROCK: (0.050, 0.050, 0.056), BEDDED: (0.050, 0.050, 0.056),
               SCREE: (0.075, 0.075, 0.08)},
    "Mesa": {SAND: (0.44, 0.27, 0.16), SHINGLE: (0.30, 0.20, 0.14), TURF: (0.150, 0.150, 0.060),
             HEATH: (0.200, 0.135, 0.070), ROCK: (0.34, 0.19, 0.11), BEDDED: (0.36, 0.185, 0.10),
             SCREE: (0.33, 0.20, 0.125)},
    "Dunes": {SAND: (0.50, 0.45, 0.34), SHINGLE: (0.30, 0.28, 0.24), TURF: (0.115, 0.165, 0.055),
              HEATH: (0.215, 0.205, 0.105), ROCK: (0.22, 0.21, 0.20), BEDDED: (0.22, 0.21, 0.20),
              SCREE: (0.36, 0.32, 0.25)},
    "Atoll": {SAND: (0.62, 0.58, 0.47), SHINGLE: (0.46, 0.43, 0.36), TURF: (0.090, 0.155, 0.050),
              HEATH: (0.150, 0.130, 0.070), ROCK: (0.36, 0.33, 0.27), BEDDED: (0.38, 0.35, 0.28),
              SCREE: (0.44, 0.41, 0.33)},
}
# The tile's deep floor matches the scene's flat seafloor plane (Drift.olo "Seafloor",
# AlbedoColor 0.30/0.29/0.24), so the tile's square edge has nothing to show against.
SEAFLOOR = (0.30, 0.29, 0.24)


def _norm(weights):
    total = np.sum(weights, axis=0)
    if float(total.min()) <= 1e-6:
        # A texel no layer claims is not "empty": the shader's height blend then gives every
        # layer a share and the ground there is the average of all eight textures.
        raise RuntimeError(f"{int((total <= 1e-6).sum())} texels have no layer weight")
    return weights / total


def bake(name, h, cell, x, z, aux, flow, talus, seed):
    """Returns (splat0 u8 HxWx4, splat1 u8 HxWx4, macro u8 HxWx4, layer weights 8xHxW)."""
    slope = f.slope_degrees(h, cell)
    d = f.signed_shore_distance(h, cell)
    n_big = f.fbm(x, z, seed + 500, 46.0, 4)
    n_mid = f.fbm(x, z, seed + 501, 13.0, 4)
    n_fine = f.fbm(x, z, seed + 502, 3.2, 3)
    flow_n = np.clip(np.log1p(f.blur(flow, 1.2)) / 4.5, 0.0, 1.0)
    sandy = aux.get("sand")
    bedded = aux["rock_kind"] > 0.5
    beach = np.clip(aux["beach"], 0.0, 1.0)
    w = np.zeros((8,) + h.shape)

    # Rock: anything too steep to hold soil. The threshold wanders so the edge is not a contour.
    steep = f.smoothstep(35.0, 46.0, slope + 5.0 * n_mid + 3.0 * n_fine)
    if "solid" in aux:  # Stacks: the platform and the stack tops are bare rock at any slope
        steep = np.maximum(steep, f.smoothstep(-1.5, 0.2, h) * (1.0 - f.smoothstep(2.6, 4.5, h)))
    rock_i = BEDDED if bedded.any() else ROCK
    # Scree: where the thermal pass laid material down, below a face.
    scree = f.smoothstep(0.25, 0.9, f.blur(talus, 1.5)) * (1.0 - steep) * f.smoothstep(0.6, 2.0, h)

    # The shore zone: sand or shingle from the surf line up to the storm berm.
    shore_top = 2.3 + 0.5 * n_mid
    shore = (1.0 - f.smoothstep(shore_top, shore_top + 0.9, h)) * f.smoothstep(-3.2, -1.6, h) * (1.0 - steep)
    sand_beach = name in ("Mesa", "Dunes", "Atoll")
    # Soil: turf where it is damp and sheltered, heath where it is high, dry and exposed.
    top = max(float(h.max()), 1.0)
    wx, wz = aux["wind"]
    gz, gx = np.gradient(f.blur(h, 3.0), cell)
    windward = np.clip(-(gx * wx + gz * wz) * 1.4, -1.0, 1.0)  # +1 faces the wind
    exposure = np.clip(0.9 * h / top + 0.35 * windward - 0.9 * flow_n + 0.45 * n_big, 0.0, 1.0)
    soil = (1.0 - steep) * (1.0 - scree) * f.smoothstep(shore_top - 0.2, shore_top + 1.0, h)
    heath = f.smoothstep(0.42, 0.62, exposure + 0.18 * n_mid)

    if sandy is not None and name == "Dunes":
        # Marram holds the lee and the hollows; the windward faces and crests are bare sand.
        mobile = np.clip(0.55 * windward + 0.9 * f.smoothstep(18.0, 34.0, slope) + 0.5 * n_big
                         + 0.35 * f.smoothstep(40.0, 5.0, d), 0.0, 1.0)
        veg = soil * (1.0 - f.smoothstep(0.38, 0.62, mobile))
        machair = f.smoothstep(4.6, 3.2, h) * f.smoothstep(34.0, 60.0, d)
        w[TURF] = veg * np.clip(machair + 0.25, 0.0, 1.0)
        w[HEATH] = veg * (1.0 - np.clip(machair + 0.25, 0.0, 1.0))
        w[SAND] = np.maximum(1.0 - veg, 0.0) * f.smoothstep(-3.5, -1.8, h)
    elif sandy is not None:  # Atoll
        motu = sandy * f.smoothstep(1.2, 2.2, h + 0.4 * n_mid)
        w[HEATH] = motu * 0.7
        w[TURF] = motu * 0.3 * f.smoothstep(-0.2, 0.3, n_big)
        reef_d = aux["reef_d"]
        flat = (1.0 - sandy) * f.smoothstep(-1.5, -0.9, h) * f.smoothstep(-aux["flat_in"] * 1.2, -aux["flat_in"] * 0.5, reef_d)
        w[BEDDED] = flat  # the reef flat: coral rock awash
        lagoon = (reef_d < 0.0) * (1.0 - flat)
        w[SAND] = np.maximum(sandy * (1.0 - motu), lagoon * 1.0)
        w[SEABED] = (reef_d >= 0.0) * f.smoothstep(-1.5, -5.0, h)
    else:
        w[rock_i] = steep
        w[SCREE] = scree
        w[SAND if sand_beach else SHINGLE] = shore * np.maximum(beach, 0.25)
        w[rock_i] += shore * (1.0 - np.maximum(beach, 0.25))  # a rock shore between the pockets
        w[TURF] = soil * (1.0 - heath)
        w[HEATH] = soil * heath
        if name == "Stacks":
            # Only the block tops carry turf; the stack tops and the platform stay bare.
            keep = f.smoothstep(38.0, 46.0, h)
            w[TURF] *= keep
            w[HEATH] *= keep
            w[rock_i] += (1.0 - keep) * soil

    if sandy is None:
        deep = f.smoothstep(-1.8, -6.0, h)
        w *= (1.0 - deep)
        w[SEABED] += deep
    else:
        # Whatever the sand rules above leave unclaimed: seabed under water, sand above it.
        w = np.maximum(w, 0.0)
        unclaimed = np.clip(1.0 - np.sum(w, axis=0), 0.0, 1.0)
        w[SEABED] += unclaimed * (h < -0.5)
        w[SAND] += unclaimed * (h >= -0.5)
    w = _norm(np.maximum(w, 0.0))

    # ── Macro colour ──────────────────────────────────────────────────────────────────────
    palette = PALETTES[name]
    tint = np.zeros(h.shape + (3,))
    for layer in range(8):
        target = SEAFLOOR if layer == SEABED else palette[layer]
        ratio = np.array(target) / np.array(NEUTRAL[layer])
        tint += w[layer][..., None] * ratio
    rockish = w[ROCK] + w[BEDDED]
    veg = w[TURF] + w[HEATH]

    def mul(mask, colour):
        nonlocal tint
        tint *= 1.0 + np.clip(mask, 0.0, 1.0)[..., None] * (np.array(colour) - 1.0)

    # Vegetation: lush and dark where water gathers, bleached on the exposed crests.
    mul(veg * flow_n, (0.72, 0.86, 0.62))
    mul(veg * f.smoothstep(0.45, 0.9, exposure), (1.28, 1.14, 0.86))
    mul(veg * f.smoothstep(0.15, 0.6, n_big), (0.9, 0.96, 0.84))
    # The intertidal ladder on rock: black algae, pale barnacles, orange lichen above the spray.
    tide = rockish + w[SHINGLE] + w[SCREE]
    mul(tide * f.smoothstep(-1.6, -0.4, h) * (1.0 - f.smoothstep(0.9, 1.5, h + 0.25 * n_mid)), (0.36, 0.38, 0.34))
    mul(rockish * f.smoothstep(1.0, 1.5, h) * (1.0 - f.smoothstep(2.1, 3.0, h + 0.4 * n_mid)) * f.smoothstep(-0.2, 0.25, n_fine),
        (1.32, 1.28, 1.12))
    lichen = rockish * f.smoothstep(2.8, 4.5, h) * (1.0 - f.smoothstep(9.0, 16.0, h)) * f.smoothstep(0.18, 0.42, n_mid + 0.5 * n_fine)
    mul(lichen, (1.75, 1.3, 0.5) if name != "Mesa" else (1.1, 1.05, 0.95))
    # Grey-green crust lichen higher up, in patches.
    mul(rockish * f.smoothstep(8.0, 20.0, h) * f.smoothstep(0.1, 0.4, n_big), (1.12, 1.16, 1.02))
    if name in ("Stacks", "Sisters"):
        # Guano: white on the ledges birds stand on, streaked down the face below them.
        ledge = rockish * (1.0 - f.smoothstep(18.0, 42.0, slope)) * f.smoothstep(7.0, 12.0, h)
        streak = f.smoothstep(0.35, 0.7, f.fbm(x * 3.2, z * 3.2 + h * 0.6, seed + 510, 9.0, 3) * 0.5 + 0.5)
        amount = np.clip(ledge * f.smoothstep(-0.05, 0.3, n_mid) + 0.55 * f.blur(ledge, 5.0) * streak * rockish, 0.0, 1.0)
        white = np.array((0.62, 0.61, 0.57)) / np.array(palette[ROCK])
        tint = f.lerp(tint, np.minimum(white, 1.99), (amount * (0.85 if name == "Stacks" else 0.5))[..., None])
    if bedded.any():
        # Bedding: the cliff's colour follows height, in beds of uneven thickness that dip gently.
        bed_h = h + 0.035 * (x - z) + 2.2 * f.fbm(x, z, seed + 520, 90.0, 2)
        beds = (np.sin(bed_h * 0.83) * 0.5 + np.sin(bed_h * 2.31 + 1.3) * 0.3 + np.sin(bed_h * 5.9 + 0.4) * 0.2)
        mul(rockish * f.smoothstep(0.1, 0.7, beds), (1.22, 1.16, 1.08))
        mul(rockish * f.smoothstep(0.1, 0.7, -beds), (0.74, 0.70, 0.70))
        if name == "Mesa":
            # Desert varnish: dark streaks hanging from the rim.
            varnish = f.smoothstep(0.3, 0.7, f.fbm(x * 2.6, z * 2.6, seed + 521, 7.0, 3) * 0.5 + 0.5)
            mul(rockish * f.smoothstep(55.0, 72.0, slope) * varnish, (0.62, 0.58, 0.60))
    # Sand: damp and darker toward the water, a strandline of weed along the last high tide.
    sandish = w[SAND] + w[SHINGLE]
    strand = np.exp(-((h - (1.25 + 0.18 * n_mid)) / 0.16) ** 2) * f.smoothstep(-0.1, 0.25, n_fine + 0.3 * n_mid)
    mul(sandish * strand, (0.50, 0.46, 0.36))
    mul(sandish * f.smoothstep(0.2, 0.75, n_big) * f.smoothstep(2.0, 4.0, h), (1.08, 1.06, 1.0))
    # Occlusion baked from the relief itself: clefts and gully floors are darker.
    cavity = np.clip((f.blur(h, 9.0 / cell * 0.5) - h) / 2.2, 0.0, 1.0) * f.smoothstep(0.0, 1.5, h)
    mul(cavity, (0.62, 0.62, 0.64))
    # Slow variation so no hillside is one colour.
    tint *= (1.0 + 0.085 * n_big + 0.05 * n_mid)[..., None]

    # Wetness: the swash zone, and damp gully floors on rock and soil.
    wet = 1.0 - f.smoothstep(0.25 + 0.15 * n_mid, 0.95 + 0.2 * n_mid, h)
    wet = np.maximum(wet, 0.45 * flow_n * f.smoothstep(0.5, 0.85, flow_n) * (1.0 - sandish))
    alpha = 1.0 - 0.9 * np.clip(wet, 0.0, 1.0)

    macro = np.concatenate([np.clip(tint * 128.0, 0.0, 255.0), (alpha * 255.0)[..., None]], axis=-1)
    macro = np.round(macro).astype(np.uint8)
    q = np.round(np.moveaxis(w, 0, -1) * 255.0).astype(np.uint8)
    return q[..., 0:4].copy(), q[..., 4:8].copy(), macro, w


def mean_albedo(w, macro):
    """Linear albedo the shader will show, from the layers' neutral means: for previews and checks."""
    base = np.zeros(w.shape[1:] + (3,))
    for layer in range(8):
        base += w[layer][..., None] * np.array(NEUTRAL[layer])
    return base * (macro[..., :3].astype(np.float64) / 128.0) * f.lerp(1.0, 0.55, 1.0 - macro[..., 3:4] / 255.0)
