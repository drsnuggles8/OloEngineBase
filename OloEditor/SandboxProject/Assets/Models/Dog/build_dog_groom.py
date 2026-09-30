"""The showcase dog's coat, grown in Blender and exported as Alembic hair curves (issue #1533).

    blender -b --factory-startup --python build_dog_groom.py -- [dog_dir] [abc_path]

Imports Dog.gltf (build_dog.py's output, the mesh the engine binds the coat to) and Dog.rig.json,
grows the coat on it, and writes one hair Curves object per coat group into an Alembic archive,
by default .dog-groom/Dog.abc at the repository root. build_dog.py runs it last. The engine cooks
the archive (AlembicGroomImporter) into the shipped Assets/Grooms/Dog/Dog.ologroom:

    OLO_DOG_EXPORT=1 OloEngine-Tests --gtest_filter=DogShowcaseEvidenceTest.ExportsTheLiveScene

and draws it straight from the .abc with OLO_DOG_ABC=1, for look development. The .abc is a ~60 MB
build output: git-ignored, and outside SandboxProject/Assets, where the editor registers whatever it
finds.

WHAT THE .abc CARRIES. Blender's Alembic exporter writes a hair object's points, curve sizes and
widths, and its CUSTOM PROPERTIES as `.userProperties` -- but no UVs and no custom attributes for
curves. So everything else a coat group is rides on the curves datablock as custom properties,
which AlembicGroomImporter reads:

    groom_role        int, the GroomCoatRole (1 undercoat, 2 guard hair, 3 whisker, 4 long hair)
    groom_tint        3 floats, the root tint;  groom_tip_tint  3 floats, relative to it
    groom_clump, groom_curl_radius, groom_curl_frequency, groom_wave_amplitude,
    groom_wave_frequency, groom_stiffness      one float each, GroomCoatGroupDesc's fields
    groom_guide       one int per curve: 1 marks a simulation guide
    groom_root_uv     two floats per curve: the root's UV on the pelt, in the ENGINE's convention
                      (glTF's: v down), which keys the coat colour map and the clump cells

The per-curve two are ALSO kept as curve attributes (groom_guide, and Blender's own
surface_uv_coordinate), which follow the curves through sculpting; export_groom_abc() rebuilds the
properties from them, so a coat combed by hand in Blender exports consistently. Run from a live
session: open a scene with the coat, then build_dog_groom.export_groom_abc(path).

THE RECIPE is the one DogShowcaseEvidenceTest grew in C++ until #1533 moved it here: regions from
the skin's bones and the rig, a screened-smoothed flow field, per-region layers of undercoat,
guard hair and long hair, Voronoi locks, root darkening and sun-bleached tips. It runs in the
ENGINE's frame (metres, Y up, the head toward +Z, the dog's left +X) -- the frame its numbers were
tuned in -- and the curves go back to Blender's (Z up, the head toward -Y) to be exported.
"""
import json
import math
import os
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SEED = 1533


def default_abc_path():
    """.dog-groom/Dog.abc at the repository root (the directory holding CMakePresets.json)."""
    d = HERE
    while not os.path.exists(os.path.join(d, "CMakePresets.json")):
        up = os.path.dirname(d)
        if up == d:
            return os.path.join(HERE, "Dog.abc")
        d = up
    return os.path.join(d, ".dog-groom", "Dog.abc")

# ---------------------------------------------------------------------------------------------
# Frames and hashing
# ---------------------------------------------------------------------------------------------


def to_engine(p):
    """Blender (Z up, head -Y) -> engine/glTF (Y up, head +Z)."""
    p = np.asarray(p)
    return np.stack([p[..., 0], p[..., 2], -p[..., 1]], axis=-1)


def to_blender(p):
    p = np.asarray(p)
    return np.stack([p[..., 0], -p[..., 2], p[..., 1]], axis=-1)


def hash01(a, b):
    """The coat's deterministic hash in [0, 1) (the C++ recipe's Hash01, bit for bit)."""
    with np.errstate(over="ignore"):
        a = np.asarray(a, dtype=np.uint32)
        b = np.asarray(b, dtype=np.uint32)
        h = (a * np.uint32(0x9E3779B1)) ^ (b * np.uint32(0x85EBCA77)) ^ np.uint32(0xC2B2AE3D)
        h = h ^ (h >> np.uint32(15))
        h = h * np.uint32(0x2C1B3C6D)
        h = h ^ (h >> np.uint32(12))
        h = h * np.uint32(0x297A2D39)
        h = h ^ (h >> np.uint32(15))
    return (h >> np.uint32(8)).astype(np.float32) * np.float32(1.0 / 16777216.0)


def _normalize(v, eps=1e-20):
    n = np.linalg.norm(v, axis=-1, keepdims=True)
    return np.where(n > math.sqrt(eps), v / np.maximum(n, 1e-30), 0.0)


def _smoothstep01(t):
    t = np.clip(t, 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def _env_floats(name, defaults):
    """Comma-separated floats from the environment over `defaults` (look-development A/B)."""
    raw = os.environ.get(name)
    out = list(defaults)
    if raw:
        for i, part in enumerate(raw.split(",")[:len(out)]):
            try:
                out[i] = float(part)
            except ValueError:
                pass
    return out


# ---------------------------------------------------------------------------------------------
# Regions
# ---------------------------------------------------------------------------------------------

REGIONS = ["muzzle", "face", "eyerim", "brow", "cheek", "skull", "earouter", "earinner", "lid", "neck",
           "chestruff", "body", "belly", "leg", "legback", "paw", "tailtop", "tailplume"]
R = {name: i for i, name in enumerate(REGIONS)}
HEAD_REGIONS = {R["muzzle"], R["face"], R["eyerim"], R["brow"], R["cheek"], R["skull"], R["lid"]}
# OLO_DOG_REGION_DEBUG=1 paints each region one flat hue (the evidence also drops the colour map).
REGION_DEBUG_HUES = [(1.0, 0.2, 0.2), (1.0, 0.6, 0.1), (0.0, 0.0, 0.0), (1.0, 0.0, 1.0), (0.0, 1.0, 0.6),
                     (1.0, 1.0, 0.2), (0.2, 1.0, 0.2), (0.1, 0.5, 0.1), (1.0, 1.0, 1.0), (0.2, 0.5, 1.0),
                     (0.7, 0.2, 1.0), (0.8, 0.8, 0.8), (0.2, 1.0, 1.0), (0.5, 0.3, 0.1), (1.0, 0.3, 0.8),
                     (0.3, 0.3, 0.3), (0.1, 0.1, 0.8), (0.9, 0.9, 0.5)]

UNDERCOAT, GUARD, WHISKER, LONGHAIR = 1, 2, 3, 4
ROLE_SUFFIX = {UNDERCOAT: "undercoat", GUARD: "guard", WHISKER: "whiskers", LONGHAIR: "longhair"}

FORWARD = np.array((0.0, 0.0, 1.0))
UP = np.array((0.0, 1.0, 0.0))
SIDE = np.array((1.0, 0.0, 0.0))  # cross(up, forward): the dog's left


class Frame:
    """The landmarks the recipe measures from, in the engine's frame."""

    def __init__(self, rig, heads):
        self.eye_centre = np.array([rig["eyes"]["L"]["centre"], rig["eyes"]["R"]["centre"]], dtype=np.float64)
        self.gaze = _normalize(np.array([rig["eyes"]["L"]["gaze"], rig["eyes"]["R"]["gaze"]], dtype=np.float64))
        self.socket_radius = float(rig.get("socketRadius", rig["eyeRadius"] * 1.15))
        self.eye_mid = self.eye_centre.mean(axis=0)
        self.eye_rim_radius = self.socket_radius + 0.012
        self.tail_root = heads["tail_01"]
        self.tail_tip = heads["tail_06"]
        self.ear_top = heads["ear_L_01"][1]
        self.ear_bottom = heads["ear_L_03"][1] - 0.04
        self.elbow_y = heads["forearm_L"][1]
        self.stifle_y = heads["shin_L"][1]


def classify(c, n, bone, is_lid, fr):
    """Per-triangle region (the C++ recipe's CollectSurface): c centroids, n face normals, bone the
    first corner's dominant bone name, is_lid the DogLid triangles."""
    k = len(c)
    up = n @ UP
    fwd = n @ FORWARD
    where = np.full(k, R["body"], dtype=np.int32)

    def head_region(idx):
        cc = c[idx]
        rel = cc - fr.eye_mid
        ahead = rel @ FORWARD
        above = rel @ UP
        eye = np.where((cc[:, 0] >= 0.0)[:, None], fr.eye_centre[0], fr.eye_centre[1])
        from_eye = cc - eye
        eye_up = from_eye @ UP
        eye_ahead = from_eye @ FORWARD
        out = np.full(len(idx), R["face"], dtype=np.int32)
        skull = ((up[idx] > 0.45) & (ahead < 0.02)) | ((ahead < -0.01) & (above > -0.01))
        out[skull] = R["skull"]
        out[(above < -0.006) & (ahead < 0.012) & (ahead > -0.07)] = R["cheek"]
        brow = (eye_up > 0.022) & (eye_up < 0.05) & (np.abs(from_eye[:, 0]) < 0.03) & (eye_ahead > -0.03) & (eye_ahead < 0.02)
        out[brow] = R["brow"]
        out[np.linalg.norm(from_eye, axis=1) < fr.eye_rim_radius] = R["eyerim"]
        out[(ahead > 0.012) & (above < 0.0)] = R["muzzle"]
        return out

    def leg_region(idx):
        return np.where(fwd[idx] < -0.35, R["legback"], R["leg"]).astype(np.int32)

    starts = lambda p: np.array([b.startswith(p) for b in bone])
    is_head = np.array([b in ("head", "jaw") for b in bone]) | starts("brow") | starts("lid") | starts("tongue")
    is_ear = starts("ear_")
    is_neck = starts("neck")
    is_spine3 = np.array([b == "spine_03" for b in bone])
    is_trunk = np.array([b in ("spine_01", "spine_02", "pelvis", "root") for b in bone])
    is_shoulder = starts("scapula") | starts("upperarm")
    is_thigh = starts("thigh")
    is_lower = starts("forearm") | starts("shin")
    is_foot = starts("wrist") | starts("hock") | starts("paw")
    is_tail = starts("tail")

    idx = np.nonzero(is_head)[0]
    where[idx] = head_region(idx)
    # Above the ear's root line the ear-root bones carry skull skin; the flaps hang outside it.
    idx = np.nonzero(is_ear)[0]
    on_skull = (np.abs(c[idx, 0]) < 0.075) | (c[idx, 1] > fr.ear_top - 0.005)
    out_face = (n[idx] @ SIDE) * np.where(c[idx, 0] >= 0.0, 1.0, -1.0) > 0.0
    ear = np.where(out_face, R["earouter"], R["earinner"]).astype(np.int32)
    if on_skull.any():
        ear[on_skull] = head_region(idx[on_skull])
    where[idx] = ear
    idx = np.nonzero(is_neck)[0]
    where[idx] = np.where((fwd[idx] > 0.35) | (up[idx] < -0.45), R["chestruff"], R["neck"])
    idx = np.nonzero(is_spine3)[0]
    where[idx] = np.where((fwd[idx] > 0.30) | (up[idx] < -0.50), R["chestruff"], R["body"])
    idx = np.nonzero(is_trunk)[0]
    where[idx] = np.where(up[idx] < -0.55, R["belly"], R["body"])
    idx = np.nonzero(is_shoulder)[0]
    above_elbow = c[idx, 1] > fr.elbow_y + 0.02
    where[idx] = np.where(above_elbow, np.where((fwd[idx] > 0.30) | (up[idx] < -0.50), R["chestruff"], R["body"]),
                          leg_region(idx))
    idx = np.nonzero(is_thigh)[0]
    where[idx] = np.where(c[idx, 1] > fr.stifle_y + 0.02, np.where(up[idx] < -0.55, R["belly"], R["body"]),
                          leg_region(idx))
    idx = np.nonzero(is_lower)[0]
    where[idx] = leg_region(idx)
    idx = np.nonzero(is_foot)[0]
    where[idx] = np.where(c[idx, 1] < 0.075, R["paw"], leg_region(idx))
    # The plume grows up the tail's sides, under the top coat's edge.
    idx = np.nonzero(is_tail)[0]
    where[idx] = np.where(up[idx] < 0.25, R["tailplume"], R["tailtop"])
    where[is_lid] = R["lid"]
    return where


# ---------------------------------------------------------------------------------------------
# The flow field
# ---------------------------------------------------------------------------------------------


def comb_direction(region, at, fr):
    """The direction each region is combed, per root (the C++ recipe's CombDirection)."""
    back, down = -FORWARD, -UP
    outward = SIDE[None, :] * np.clip(at[:, 0] / 0.025, -1.0, 1.0)[:, None]
    tail = _normalize(fr.tail_tip - fr.tail_root)
    k = len(at)
    # Every region not named below -- the body's -- is combed back and a little down. Left at
    # zero, the smoothing held it there and the back's coat stood straight out of the skin.
    d = np.tile(back + down * 0.35, (k, 1))

    def put(r, v):
        m = region == R[r]
        if m.any():
            d[m] = v[m] if np.ndim(v) == 2 else v

    put("muzzle", back + outward * 0.35 + down * 0.15)
    put("face", back + outward * 0.35 + down * 0.2)
    put("brow", UP * 0.8 + outward * 0.6 + back * 0.4)
    put("eyerim", back + outward * 0.3)
    put("cheek", back * 0.8 + down * 0.6 + outward * 0.3)
    put("skull", np.tile(back + down * 0.1, (k, 1)))
    put("earouter", np.tile(down + back * 0.15, (k, 1)))
    put("earinner", np.tile(down + back * 0.15, (k, 1)))
    lid = region == R["lid"]
    if lid.any():
        # Away from the eye opening, turned half toward the back corner: purely radial, the lid
        # fur stood out round the eye like a sea urchin's spines.
        e = np.where(at[lid, 0] >= 0.0, 0, 1)
        r = at[lid] - fr.eye_centre[e]
        r = r - fr.gaze[e] * np.sum(r * fr.gaze[e], axis=1, keepdims=True)
        radial = np.where((np.sum(r * r, axis=1) > 1e-10)[:, None], _normalize(r), back)
        d[lid] = radial + (back + outward[lid] * 0.3) * 0.9
    put("neck", np.tile(back * 0.6 + down * 0.8, (k, 1)))
    put("chestruff", np.tile(down + FORWARD * 0.3, (k, 1)))
    put("belly", np.tile(down + back * 0.25, (k, 1)))
    put("leg", np.tile(down + back * 0.1, (k, 1)))
    put("legback", np.tile(down + back * 0.45, (k, 1)))
    put("paw", np.tile(FORWARD + down * 0.6, (k, 1)))
    put("tailtop", np.tile(tail, (k, 1)))
    # The flag hangs: combed down more than back, so the strands grown on the tail's sides fall.
    put("tailplume", np.tile(tail * 0.55 + down, (k, 1)))
    d = _normalize(d)
    # On the head the coat parts round each eye.
    head = np.isin(region, [R["muzzle"], R["face"], R["brow"], R["cheek"], R["skull"], R["eyerim"]])
    if head.any():
        eye = np.where((at[head, 0] >= 0.0)[:, None], fr.eye_centre[0], fr.eye_centre[1])
        away = at[head] - eye
        dist = np.linalg.norm(away, axis=1)
        w = np.clip((0.065 - dist) / 0.035, 0.0, 1.0)
        bent = _normalize(d[head] + (away / np.maximum(dist, 1e-6)[:, None]) * (1.6 * w * w)[:, None])
        d[head] = np.where(((dist > 1e-6) & (w > 0.0))[:, None], bent, d[head])
    return d


def smooth_flow_field(p, n, area, where, fr, log=print):
    """The recipe's flow field: per-triangle region combs, screened-smoothed across seams so the
    coat turns at a region boundary instead of parting along it. Lids keep their radial comb."""
    passes = int(os.environ.get("OLO_DOG_FLOW_PASSES", "24"))
    anchor = 0.12
    cen = p.mean(axis=1)
    comb = comb_direction(where, cen, fr)
    comb = comb - n * np.sum(comb * n, axis=1, keepdims=True)
    authored = np.where((np.sum(comb * comb, axis=1) > 1e-6)[:, None], _normalize(comb), 0.0)
    # Vertex-sharing adjacency by quantised position (UV and material seams split vertices).
    t = len(p)
    q = np.round(p.reshape(-1, 3) * 20000.0).astype(np.int64)
    _, key = np.unique(q, axis=0, return_inverse=True)
    key = key.ravel()
    tri = np.repeat(np.arange(t), 3)
    order = np.argsort(key, kind="stable")
    ks, ts = key[order], tri[order]
    bounds = np.flatnonzero(np.diff(ks)) + 1
    starts = np.concatenate([[0], bounds])
    ends = np.concatenate([bounds, [len(ks)]])
    a_list, b_list = [], []
    sizes = ends - starts
    for m in np.unique(sizes):
        if m < 2:
            continue
        sel = starts[sizes == m]
        grp = ts[sel[:, None] + np.arange(m)[None, :]]  # (G, m)
        ii, jj = np.meshgrid(np.arange(m), np.arange(m), indexing="ij")
        off = ii != jj
        a_list.append(grp[:, ii[off]].ravel())
        b_list.append(grp[:, jj[off]].ravel())
    pairs = np.unique(np.stack([np.concatenate(a_list), np.concatenate(b_list)], axis=1), axis=0)
    a, b = pairs[:, 0], pairs[:, 1]
    lid = where == R["lid"]
    keep = ~lid[b]
    a, b = a[keep], b[keep]
    fixed = lid | (np.sum(authored * authored, axis=1) < 0.5)
    field = authored.copy()
    wb = area[b]
    for _ in range(passes):
        mean = np.zeros((t, 3))
        for axis in range(3):
            mean[:, axis] = np.bincount(a, weights=field[b, axis] * wb, minlength=t)
        has = np.sum(mean * mean, axis=1) > 1e-20
        m = np.where(has[:, None], _normalize(mean), field)
        d = authored * anchor + m * (1.0 - anchor)
        d = d - n * np.sum(d * n, axis=1, keepdims=True)
        ok = np.sum(d * d, axis=1) > 1e-8
        nxt = np.where(ok[:, None], _normalize(d), authored)
        field = np.where(fixed[:, None], authored, nxt)
    log(f"[groom] flow field: {t} triangles, {len(a)} neighbour pairs, {passes} passes")
    return field


# ---------------------------------------------------------------------------------------------
# The recipe
# ---------------------------------------------------------------------------------------------


def layer(where, role, per_m2, length, diameter, points, **kw):
    lay = dict(where=R[where], role=role, per_m2=per_m2, length=length, diameter=diameter, points=points,
               lift=0.3, droop=0.05, clump=0.0, wander=0.10, curl_radius=0.0, curl_frequency=0.0,
               wave_amplitude=0.0, wave_frequency=0.0, stiffness=1.0, tint=(1.0, 1.0, 1.0),
               tip_tint=(1.0, 1.0, 1.0), ramp=None, ramp_base=1.0, guide_every=12, suffix=None,
               lock_radius=0.0, lock_amount=0.0, fine_lock_radius=0.0, fine_lock_amount=0.0,
               tip_width=0.2, eye_ramp=False)
    lay.update(kw)
    return lay


def dog_coat_recipe():
    """THE COAT. A strand stands for a lock (widths carry coverage): short and dense on the face,
    muzzle and paws; feathering on the ears, chest, belly and the backs of the legs; a flag on the
    tail. Scruffy: gathered into locks, waved, curled in the undercoat. The numbers are the ones
    the C++ recipe was graded with through #1533's look development; the comments there are here."""
    under = (0.95, 0.95, 0.95)  # a touch darker inside the coat
    bleached = (1.06, 1.06, 1.06)  # sun-lightened tips
    L = []
    # --- the face: dense and lifted, so it reads as fur and not as skin at a close-up ---
    L.append(layer("muzzle", UNDERCOAT, 420000.0, 0.012, 4.0e-4, 5, lift=0.45, droop=0.0, clump=0.1, stiffness=3.0,
                   tint=under, guide_every=40))
    L.append(layer("muzzle", GUARD, 160000.0, 0.018, 5.0e-4, 6, lift=0.5, droop=0.0, clump=0.3, wander=0.12,
                   stiffness=3.0, tip_tint=bleached, guide_every=40))
    L.append(layer("face", UNDERCOAT, 320000.0, 0.016, 4.5e-4, 5, lift=0.45, droop=0.0, clump=0.15, stiffness=3.0,
                   tint=under, guide_every=40))
    L.append(layer("face", GUARD, 160000.0, 0.028, 5.5e-4, 7, lift=0.45, droop=0.01, clump=0.4, wander=0.12,
                   stiffness=3.0, tip_tint=bleached, guide_every=30))
    # Right up to the dark lid: at 6 mm and 38/cm^2 the rim left a bald halo round each eye.
    L.append(layer("eyerim", UNDERCOAT, 600000.0, 0.016, 4.5e-4, 5, lift=0.25, droop=0.0, stiffness=3.0, tint=under,
                   guide_every=40))
    L.append(layer("eyerim", GUARD, 170000.0, 0.028, 5.0e-4, 6, lift=0.20, droop=0.005, wander=0.12, stiffness=3.0,
                   guide_every=40))
    L.append(layer("brow", UNDERCOAT, 300000.0, 0.016, 4.5e-4, 5, lift=0.5, droop=0.0, stiffness=3.0, tint=under,
                   guide_every=40))
    L.append(layer("brow", GUARD, 220000.0, 0.034, 6.0e-4, 8, lift=0.75, droop=0.015, clump=0.5, wander=0.10,
                   wave_amplitude=0.002, wave_frequency=25.0, stiffness=2.0, tip_tint=bleached, guide_every=20))
    L.append(layer("cheek", UNDERCOAT, 280000.0, 0.020, 4.5e-4, 6, lift=0.45, droop=0.02, clump=0.15, tint=under,
                   guide_every=30))
    L.append(layer("cheek", GUARD, 150000.0, 0.042, 6.0e-4, 9, lift=0.5, droop=0.05, clump=0.55, wander=0.16,
                   wave_amplitude=0.003, wave_frequency=20.0, stiffness=1.5, tip_tint=bleached, guide_every=12))
    L.append(layer("skull", UNDERCOAT, 220000.0, 0.016, 4.5e-4, 5, lift=0.3, droop=0.01, tint=under, guide_every=30))
    L.append(layer("skull", GUARD, 100000.0, 0.028, 5.5e-4, 7, lift=0.25, droop=0.02, clump=0.3, wave_amplitude=0.002,
                   wave_frequency=30.0, tip_tint=bleached, guide_every=16))
    # The lids wear the face's fur up to the dark rim; bald they read as goggles. Flat and loosely
    # gathered, or it stands out in spikes.
    L.append(layer("lid", UNDERCOAT, 600000.0, 0.016, 4.0e-4, 4, lift=0.10, droop=0.0, stiffness=3.0, tint=under,
                   guide_every=40))
    L.append(layer("lid", GUARD, 110000.0, 0.028, 4.0e-4, 5, lift=0.10, droop=0.0, stiffness=3.0, guide_every=40))
    # --- the ears: long, wavy feathering outside, short inside ---
    L.append(layer("earouter", UNDERCOAT, 220000.0, 0.014, 4.0e-4, 5, lift=0.3, droop=0.02, tint=under, guide_every=30))
    L.append(layer("earouter", LONGHAIR, 180000.0, 0.10, 5.0e-4, 14, lift=0.18, droop=0.09, clump=0.55,
                   wave_amplitude=0.005, wave_frequency=22.0, stiffness=0.35, tip_tint=bleached, ramp="ear",
                   ramp_base=0.5, guide_every=8))
    L.append(layer("earinner", UNDERCOAT, 150000.0, 0.008, 3.0e-4, 4, lift=0.2, droop=0.02, tint=under, guide_every=30))
    # --- neck, chest ruff, body, belly ---
    L.append(layer("neck", UNDERCOAT, 100000.0, 0.025, 4.5e-4, 8, lift=0.45, droop=0.04, curl_radius=0.0010,
                   curl_frequency=40.0, tint=under, guide_every=20))
    L.append(layer("neck", GUARD, 45000.0, 0.050, 6.0e-4, 10, lift=0.3, droop=0.08, clump=0.45, wave_amplitude=0.003,
                   wave_frequency=18.0, tip_tint=bleached, guide_every=12))
    L.append(layer("chestruff", LONGHAIR, 70000.0, 0.10, 6.0e-4, 16, lift=0.2, droop=0.12, clump=0.5,
                   wave_amplitude=0.005, wave_frequency=15.0, stiffness=0.4, tip_tint=bleached, guide_every=8))
    L.append(layer("body", UNDERCOAT, 90000.0, 0.022, 4.5e-4, 8, lift=0.45, droop=0.04, curl_radius=0.0012,
                   curl_frequency=40.0, stiffness=1.5, tint=under, guide_every=20))
    L.append(layer("body", GUARD, 40000.0, 0.055, 6.0e-4, 10, lift=0.3, droop=0.10, clump=0.45, wave_amplitude=0.0035,
                   wave_frequency=18.0, stiffness=1.5, tip_tint=bleached, guide_every=12))
    L.append(layer("belly", LONGHAIR, 55000.0, 0.08, 5.5e-4, 14, lift=0.15, droop=0.15, clump=0.5, wave_amplitude=0.004,
                   wave_frequency=16.0, stiffness=0.4, tip_tint=bleached, guide_every=8))
    # --- legs and paws ---
    L.append(layer("leg", UNDERCOAT, 150000.0, 0.016, 4.0e-4, 5, lift=0.3, droop=0.02, tint=under, guide_every=30))
    L.append(layer("leg", GUARD, 60000.0, 0.026, 4.5e-4, 6, lift=0.25, droop=0.03, tip_tint=bleached, guide_every=20))
    L.append(layer("legback", LONGHAIR, 60000.0, 0.075, 5.0e-4, 12, lift=0.2, droop=0.12, clump=0.5,
                   wave_amplitude=0.004, wave_frequency=18.0, stiffness=0.4, tip_tint=bleached, guide_every=8))
    L.append(layer("paw", UNDERCOAT, 250000.0, 0.012, 3.5e-4, 4, lift=0.25, droop=0.02, stiffness=3.0, tint=under,
                   guide_every=40))
    L.append(layer("paw", GUARD, 80000.0, 0.020, 4.0e-4, 5, lift=0.25, droop=0.03, clump=0.4, stiffness=3.0,
                   guide_every=30))
    # --- the tail: shorter on top, a long flag below ---
    L.append(layer("tailtop", GUARD, 120000.0, 0.07, 5.5e-4, 12, lift=0.25, droop=0.08, clump=0.4,
                   wave_amplitude=0.003, wave_frequency=20.0, tip_tint=bleached, guide_every=10))
    # A retriever's flag HANGS from the tail and trails it through a wag (Ole's review): lifted
    # 0.22, drooping 0.11 and stiff at 0.8 it stood out sideways as a fan, a broom's head.
    L.append(layer("tailplume", LONGHAIR, 220000.0, 0.175, 6.0e-4, 18, lift=0.08, droop=0.35, clump=0.55,
                   wave_amplitude=0.009, wave_frequency=12.0, stiffness=0.5, tip_tint=bleached, ramp="tail",
                   ramp_base=0.5, tip_width=0.28, guide_every=6))
    # Strays: sparse, long and wandering. A scruffy dog's silhouette is broken by the odd hair.
    for where in ("skull", "neck", "body", "chestruff", "cheek"):
        L.append(layer(where, GUARD, 3500.0, 0.06, 4.5e-4, 10, lift=0.8, droop=0.06, wander=0.9, wave_amplitude=0.004,
                       wave_frequency=15.0, tip_tint=bleached, guide_every=6, suffix="strays"))

    # Locks: guard and long hair gather into visible locks with finer tufts inside; the undercoat
    # into small tufts only. The engine's own clump reads as combed, so it is off where the curves
    # carry their locks.
    for lay in L:
        if lay["suffix"]:
            continue
        lay["clump"] = 0.0
        w = REGIONS[lay["where"]]
        if lay["role"] == UNDERCOAT:
            lay.update(fine_lock_radius=0.0028, fine_lock_amount=0.35)
            continue
        if w in ("muzzle", "eyerim", "paw"):
            lay.update(lock_radius=0.009, lock_amount=0.9)
        elif w == "lid":
            lay.update(lock_radius=0.004, lock_amount=0.4)
        elif w == "tailtop":
            lay.update(lock_radius=0.012, lock_amount=0.55, fine_lock_radius=0.003, fine_lock_amount=0.4)
        elif w == "tailplume":
            lay.update(lock_radius=0.028, lock_amount=0.88, fine_lock_radius=0.003, fine_lock_amount=0.45)
        elif w in ("face", "brow", "skull"):
            lay.update(lock_radius=0.014, lock_amount=0.9, fine_lock_radius=0.0022, fine_lock_amount=0.4)
        else:
            lay.update(lock_radius=0.0275 if lay["role"] == LONGHAIR else 0.02, lock_amount=0.9,
                       fine_lock_radius=0.0032, fine_lock_amount=0.5)

    # OLO_DOG_TAIL="lockRadius,lockAmount,lengthScale,droop,waveAmplitude,tipWidth,lift,stiffness"
    if os.environ.get("OLO_DOG_TAIL"):
        for lay in L:
            if lay["where"] == R["tailplume"] and not lay["suffix"]:
                v = _env_floats("OLO_DOG_TAIL", [lay["lock_radius"], lay["lock_amount"], 1.0, lay["droop"],
                                                 lay["wave_amplitude"], lay["tip_width"], lay["lift"], lay["stiffness"]])
                lay.update(lock_radius=v[0], lock_amount=v[1], length=lay["length"] * v[2], droop=v[3],
                           wave_amplitude=v[4], tip_width=v[5], lift=v[6], stiffness=v[7])

    # OLO_DOG_FINE="count,width,tip,wander,muzzleLength,rootTint" re-tunes the fine pass below.
    fine_count, fine_width, fine_tip, fine_wander, muzzle_length, root_tint = _env_floats(
        "OLO_DOG_FINE", [2.0, 0.55, 0.06, 0.07, 0.65, 0.6])

    # ROOT DARKENING: what a coat's locks are read by. Every strand darkens toward its root and
    # guard and long hair lighten toward a sun-bleached tip, so a lock's tip catches the light and
    # the gap between two locks goes dark.
    tip_tint = 1.15
    for lay in L:
        if lay["suffix"]:
            continue
        root, tip = root_tint, tip_tint
        w = REGIONS[lay["where"]]
        # ...except where it reads as a line: round the eye, down the tail, on the plume (cream,
        # bleached and backlit, its tips glowed white), and the few millimetres of muzzle.
        if w == "lid":
            root, tip = 0.70, 1.08
        elif w == "eyerim":
            root = 0.72
        elif w == "tailtop":
            root = 0.75
        elif w == "tailplume":
            root, tip = 0.75, 0.98
        elif w == "muzzle":
            root = 0.85
        lay["tint"] = (root, root, root)
        if lay["role"] != UNDERCOAT:
            lay["tip_tint"] = (tip / root,) * 3

    # COVERAGE WHERE THE PELT SHOWED: over a magenta pelt the skin showed at the crown, the topline,
    # the tail's top and the paws, so the count rises there and those coats lie a little flatter.
    more = {"skull": 1.8, "body": 1.8, "neck": 1.5, "tailtop": 1.5, "paw": 1.5, "leg": 1.3, "chestruff": 1.3}
    for lay in L:
        m = more.get(REGIONS[lay["where"]], 1.0)
        if lay["suffix"] or m <= 1.0:
            continue
        lay["per_m2"] *= m
        lay["lift"] = max(0.12, lay["lift"] - 0.1)

    # FINE WHERE THE CAMERA GETS CLOSE (Ole's review): twice the strands at half the width keep the
    # coverage and the fibre area the self-shadow is baked from, and make each strand a line rather
    # than a stick. Tips taper to a point; the face's strands wander less.
    close = {"muzzle", "face", "eyerim", "brow", "cheek", "skull", "lid", "neck", "chestruff"}
    for lay in L:
        w = REGIONS[lay["where"]]
        if w in close and not lay["suffix"]:
            lay["per_m2"] *= fine_count
            lay["diameter"] *= fine_width
            lay["wander"] = min(lay["wander"], fine_wander)
        if lay["suffix"]:  # the strays thinner and fewer: they were the sticks a close-up saw first
            lay["per_m2"] *= 0.5
            lay["diameter"] *= fine_width
        lay["tip_width"] = min(lay["tip_width"], fine_tip * 2.0 if w == "tailplume" else fine_tip)
        # THE MUZZLE is a few millimetres of dense fur lying flat along it.
        if w == "muzzle" and not lay["suffix"]:
            lay["length"] *= muzzle_length
            lay["lift"] = min(lay["lift"], 0.25)
            lay.update(lock_radius=0.004, lock_amount=0.5)
    # The eye ramp on every head layer, strays included: a long stray beside an eye is a hair in it.
    for lay in L:
        if lay["where"] in HEAD_REGIONS:
            lay["eye_ramp"] = True
    return L


def group_name(lay):
    return f"{REGIONS[lay['where']]}_{lay['suffix'] or ROLE_SUFFIX[lay['role']]}"


# ---------------------------------------------------------------------------------------------
# Growing
# ---------------------------------------------------------------------------------------------


def ramp_at(lay, fr, root, normal):
    if lay["ramp"] == "ear":
        t = np.clip((fr.ear_top - root[:, 1]) / max(fr.ear_top - fr.ear_bottom, 1e-3), 0.0, 1.0)
        return lay["ramp_base"] + (1.0 - lay["ramp_base"]) * t
    if lay["ramp"] == "tail":
        axis = fr.tail_tip - fr.tail_root
        t = np.clip(((root - fr.tail_root) @ axis) / max(float(axis @ axis), 1e-6), 0.0, 1.0)
        scale = 1.0
        if lay["where"] == R["tailplume"]:
            # Round the tail too: longest straight down, no longer than the top coat at the sides.
            scale = 0.45 + 0.55 * _smoothstep01((0.25 - normal @ UP) / 0.85)
        return (lay["ramp_base"] + (1.0 - lay["ramp_base"]) * t) * scale
    return np.ones(len(root))


def eye_ramp_at(fr, root):
    """The face's fur shortens toward each eye opening, measured from the socket."""
    d = np.minimum(np.linalg.norm(root - fr.eye_centre[0], axis=1),
                   np.linalg.norm(root - fr.eye_centre[1], axis=1)) - fr.socket_radius
    return 0.30 + 0.70 * _smoothstep01(d / 0.022)


def apply_locks(pts, radius, amount, seed):
    """Gather strands into Voronoi locks: centres drawn by a greedy Poisson-disc pass over a hashed
    order, each strand's outer part pulled toward its nearest centre's shape carried to its own
    root, the pull closing toward the tip. Roots never move. pts (N, P, 3), edited in place."""
    n = len(pts)
    if radius <= 0.0 or amount <= 0.0 or n < 2:
        return
    from mathutils import kdtree

    roots = pts[:, 0, :]
    order = np.argsort(hash01(np.arange(n, dtype=np.uint32), seed), kind="stable")
    cells = np.floor(roots / radius).astype(np.int64)
    grid = {}
    r2 = radius * radius
    centres = []
    rl = roots.tolist()
    cl = cells.tolist()
    for i in order.tolist():
        cx, cy, cz = cl[i]
        x, y, z = rl[i]
        found = False
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    lst = grid.get((cx + dx, cy + dy, cz + dz))
                    if lst is None:
                        continue
                    for (px, py, pz) in lst:
                        if (px - x) ** 2 + (py - y) ** 2 + (pz - z) ** 2 <= r2:
                            found = True
                            break
                    if found:
                        break
                if found:
                    break
            if found:
                break
        if not found:
            grid.setdefault((cx, cy, cz), []).append((x, y, z))
            centres.append(i)
    tree = kdtree.KDTree(len(centres))
    for k, i in enumerate(centres):
        tree.insert(rl[i], k)
    tree.balance()
    nearest = np.empty(n, dtype=np.int64)
    dist = np.empty(n)
    for i in range(n):
        _co, k, d = tree.find(rl[i])
        nearest[i] = centres[k]
        dist[i] = d
    src = pts.copy()
    move = nearest != np.arange(n)
    idx = np.nonzero(move)[0]
    c = nearest[idx]
    p_count = pts.shape[1]
    t = np.arange(p_count) / (p_count - 1)
    w_t = t * t * (3.0 - 2.0 * t)
    edge = 1.0 - 0.35 * np.clip(dist[idx] / radius, 0.0, 1.0)
    root_offset = src[idx, 0, :] - src[c, 0, :]
    target = src[c] + root_offset[:, None, :] * (1.0 - t)[None, :, None]
    w = (amount * edge)[:, None] * w_t[None, :]
    w[:, 0] = 0.0
    pts[idx] = src[idx] + (target - src[idx]) * w[:, :, None]


def grow_layer(lay, li, tris, fr, flow, density):
    """One layer's strands: roots area-weighted and hashed on its region's triangles."""
    sel = np.nonzero(tris["where"] == lay["where"])[0]
    if len(sel) == 0:
        return None
    expected = tris["area"][sel] * lay["per_m2"] * density
    count = np.floor(expected).astype(np.int64)
    count += (hash01(sel.astype(np.uint32), SEED * 131 + li) < (expected - count)).astype(np.int64)
    total = int(count.sum())
    if total == 0:
        return None
    t = np.repeat(sel, count)
    s = np.concatenate([np.arange(c) for c in count]) if total else np.zeros(0, dtype=np.int64)
    with np.errstate(over="ignore"):
        salt = (t.astype(np.uint32) * np.uint32(977) + s.astype(np.uint32) * np.uint32(31) +
                np.uint32(li * 7919) + np.uint32(SEED))
    a = hash01(salt, 1).astype(np.float64)
    b = hash01(salt, 2).astype(np.float64)
    flip = a + b > 1.0
    a = np.where(flip, 1.0 - a, a)
    b = np.where(flip, 1.0 - b, b)
    c = 1.0 - a - b
    P, UV, N = tris["p"][t], tris["uv"][t], tris["n"][t]
    root = P[:, 0] * c[:, None] + P[:, 1] * a[:, None] + P[:, 2] * b[:, None]
    uv = UV[:, 0] * c[:, None] + UV[:, 1] * a[:, None] + UV[:, 2] * b[:, None]
    length = lay["length"] * ramp_at(lay, fr, root, N)
    if lay["eye_ramp"]:
        length = length * eye_ramp_at(fr, root)
    length = length * (0.75 + 0.5 * hash01(salt, 3))
    wander = np.stack([hash01(salt, 4), hash01(salt, 5), hash01(salt, 6)], axis=1).astype(np.float64) - 0.5
    comb = flow[t]
    direction = _normalize(N * lay["lift"] + comb + wander * lay["wander"])
    pc = lay["points"]
    pts = np.zeros((total, pc, 3))
    widths = np.zeros((total, pc))
    p = root.copy()
    step = length / (pc - 1)
    for k in range(pc):
        along = k / (pc - 1)
        pts[:, k] = p
        widths[:, k] = lay["diameter"] * (1.0 - (1.0 - lay["tip_width"]) * along)
        direction = _normalize(direction + comb * 0.2 - UP * lay["droop"])
        # Never back into the skin: the strand keeps a little height over its root's plane.
        over = np.sum(direction * N, axis=1)
        floor_lift = 0.12 * (1.0 - along)
        low = over < floor_lift
        if low.any():
            direction[low] = _normalize(direction[low] + N[low] * (floor_lift - over[low])[:, None])
        p = p + direction * step[:, None]
    guide = (np.arange(total) % lay["guide_every"]) == 0
    apply_locks(pts, lay["lock_radius"], lay["lock_amount"], SEED * 977 + li)
    apply_locks(pts, lay["fine_lock_radius"], lay["fine_lock_amount"], SEED * 1543 + li)
    return dict(points=pts, widths=widths, uv=uv, guide=guide)


# ---------------------------------------------------------------------------------------------
# Blender: the surface in, the curves out
# ---------------------------------------------------------------------------------------------


def import_dog(dog_dir, log=print):
    import bpy

    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=os.path.join(dog_dir, "Dog.gltf"))
    body = next(o for o in bpy.data.objects if o.type == "MESH" and o.name.startswith("Dog"))
    arm = next(o for o in bpy.data.objects if o.type == "ARMATURE")
    log(f"[groom] imported {body.name} ({len(body.data.polygons)} faces) and {arm.name} ({len(arm.data.bones)} bones)")
    return body, arm


def surface_of(body, arm, rig, log=print):
    """The pelt's triangles in the engine's frame, with their regions (the lid and the skin grow,
    the bare skins do not)."""
    me = body.data
    me.calc_loop_triangles()
    nt = len(me.loop_triangles)
    vi = np.zeros(nt * 3, dtype=np.int32)
    li = np.zeros(nt * 3, dtype=np.int32)
    mat = np.zeros(nt, dtype=np.int32)
    me.loop_triangles.foreach_get("vertices", vi)
    me.loop_triangles.foreach_get("loops", li)
    me.loop_triangles.foreach_get("material_index", mat)
    vi, li = vi.reshape(-1, 3), li.reshape(-1, 3)
    mw = np.array(body.matrix_world)
    co = np.zeros(len(me.vertices) * 3)
    me.vertices.foreach_get("co", co)
    co = co.reshape(-1, 3) @ mw[:3, :3].T + mw[:3, 3]
    co_e = to_engine(co)
    uvl = np.zeros(len(me.loops) * 2)
    me.uv_layers[0].data.foreach_get("uv", uvl)
    uvl = uvl.reshape(-1, 2)
    uvl[:, 1] = 1.0 - uvl[:, 1]  # the engine's convention (glTF's): v down
    cn = np.zeros(len(me.loops) * 3)
    me.corner_normals.foreach_get("vector", cn)
    cn = to_engine(cn.reshape(-1, 3) @ mw[:3, :3].T)
    names = [m.name if m else "" for m in me.materials]
    skin = np.array([names[m].startswith("DogSkin") for m in mat])
    lid = np.array([names[m].startswith("DogLid") for m in mat])
    keep = skin | lid
    p = co_e[vi]
    n = np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0])
    area = 0.5 * np.linalg.norm(n, axis=1)
    keep &= area > 1e-10
    n = _normalize(n)
    shading = cn[li].sum(axis=1)
    n = np.where((np.sum(n * shading, axis=1) < 0.0)[:, None], -n, n)
    # The dominant bone of each triangle's first corner, as the engine's skin weights name it.
    groups = {vg.index: vg.name for vg in body.vertex_groups}
    dominant = []
    for v in me.vertices:
        best = max(v.groups, key=lambda g: g.weight, default=None)
        dominant.append(groups.get(best.group, "") if best is not None else "")
    heads = {b.name: to_engine(np.array(arm.matrix_world @ b.head_local)) for b in arm.data.bones}
    fr = Frame(rig, heads)
    idx = np.nonzero(keep)[0]
    bone = [dominant[vi[t, 0]] for t in idx]
    tris = dict(p=p[idx], uv=uvl[li[idx]], n=n[idx], area=area[idx])
    tris["where"] = classify(tris["p"].mean(axis=1), tris["n"], bone, lid[idx], fr)
    per = np.bincount(tris["where"], weights=tris["area"], minlength=len(REGIONS)) * 1e4
    log("[groom] pelt: " + " ".join(f"{REGIONS[r]}={per[r]:.0f}cm2" for r in range(len(REGIONS)) if per[r] > 0))
    return tris, fr


def grow_coat(tris, fr, log=print):
    density = float(os.environ.get("OLO_DOG_DENSITY", "1") or 1.0)
    flow = smooth_flow_field(tris["p"], tris["n"], tris["area"], tris["where"], fr, log=log)
    groups = []
    for li, lay in enumerate(dog_coat_recipe()):
        t0 = time.time()
        grown = grow_layer(lay, li, tris, fr, flow, density)
        if grown is None:
            continue
        grown["layer"] = lay
        groups.append(grown)
        log(f"[groom] {group_name(lay)}: {len(grown['points'])} strands x {lay['points']}, "
            f"{int(grown['guide'].sum())} guides ({time.time() - t0:.1f}s)")
    total = sum(len(g["points"]) for g in groups)
    log(f"[groom] coat: {total} strands in {len(groups)} groups")
    return groups


def make_curves(groups, body, collection_name="DogGroom"):
    """One hair Curves object per group, attached to the pelt, carrying its coat as properties."""
    import bpy

    col = bpy.data.collections.get(collection_name) or bpy.data.collections.new(collection_name)
    if col.name not in bpy.context.scene.collection.children:
        bpy.context.scene.collection.children.link(col)
    debug = os.environ.get("OLO_DOG_REGION_DEBUG") == "1"
    objects = []
    for g in groups:
        lay = g["layer"]
        name = group_name(lay)
        n, pc = g["points"].shape[:2]
        cv = bpy.data.hair_curves.new(name)
        cv.add_curves([pc] * n)
        cv.attributes["position"].data.foreach_set("vector", to_blender(g["points"]).reshape(-1).astype(np.float32))
        rad = cv.attributes.get("radius") or cv.attributes.new("radius", "FLOAT", "POINT")
        rad.data.foreach_set("value", (0.5 * g["widths"]).reshape(-1).astype(np.float32))
        cv.set_types(type="POLY")
        guide = cv.attributes.new("groom_guide", "INT", "CURVE")
        guide.data.foreach_set("value", g["guide"].astype(np.int32))
        suv = cv.attributes.new("surface_uv_coordinate", "FLOAT2", "CURVE")
        uv_b = g["uv"].copy()
        uv_b[:, 1] = 1.0 - uv_b[:, 1]  # Blender's convention for the attachment
        suv.data.foreach_set("vector", uv_b.reshape(-1).astype(np.float32))
        cv.surface = body
        if body.data.uv_layers:
            cv.surface_uv_map = body.data.uv_layers[0].name
        cv["groom_role"] = int(lay["role"])
        cv["groom_tint"] = list(REGION_DEBUG_HUES[lay["where"]]) if debug else [float(x) for x in lay["tint"]]
        cv["groom_tip_tint"] = [1.0, 1.0, 1.0] if debug else [float(x) for x in lay["tip_tint"]]
        cv["groom_clump"] = float(lay["clump"])
        cv["groom_curl_radius"] = float(lay["curl_radius"])
        cv["groom_curl_frequency"] = float(lay["curl_frequency"])
        cv["groom_wave_amplitude"] = float(lay["wave_amplitude"])
        cv["groom_wave_frequency"] = float(lay["wave_frequency"])
        cv["groom_stiffness"] = float(lay["stiffness"])
        ob = bpy.data.objects.new(name, cv)
        col.objects.link(ob)
        objects.append(ob)
    return objects


def export_groom_abc(path, collection_name="DogGroom", log=print):
    """Write the coat collection to Alembic. The per-curve properties are rebuilt from the curve
    attributes first, so curves added, deleted or combed in Blender export consistently."""
    import bpy

    col = bpy.data.collections[collection_name]
    for ob in col.objects:
        cv = ob.data
        n = len(cv.curves)
        guide = np.zeros(n, dtype=np.int32)
        if "groom_guide" in cv.attributes:
            cv.attributes["groom_guide"].data.foreach_get("value", guide)
        uv = np.zeros(n * 2, dtype=np.float32)
        if "surface_uv_coordinate" in cv.attributes:
            cv.attributes["surface_uv_coordinate"].data.foreach_get("vector", uv)
        uv = uv.reshape(-1, 2)
        uv[:, 1] = 1.0 - uv[:, 1]
        cv["groom_guide"] = guide.tolist()
        cv["groom_root_uv"] = uv.reshape(-1).tolist()
    # The collection, not the selection: `selected` exports nothing from a background session.
    t0 = time.time()
    bpy.ops.wm.alembic_export(filepath=path, collection=col.name, selected=False, start=1, end=1, export_hair=True,
                              export_particles=False, export_custom_properties=True, uvs=False, normals=False,
                              vcolors=False, orcos=False, face_sets=False, flatten=False, evaluation_mode="RENDER")
    log(f"[groom] wrote {path} ({os.path.getsize(path) / 1e6:.1f} MB, {time.time() - t0:.1f}s)")


def build(dog_dir, abc_path, log=print):
    """The whole coat: a fresh scene with the exported dog, the coat grown on it, the archive."""
    with open(os.path.join(dog_dir, "Dog.rig.json"), encoding="utf-8") as fh:
        rig = json.load(fh)
    body, arm = import_dog(dog_dir, log=log)
    tris, fr = surface_of(body, arm, rig, log=log)
    groups = grow_coat(tris, fr, log=log)
    make_curves(groups, body)
    os.makedirs(os.path.dirname(abc_path), exist_ok=True)
    export_groom_abc(abc_path, log=log)


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    dog_dir = os.path.abspath(argv[0]) if argv else HERE
    abc_path = os.path.abspath(argv[1]) if len(argv) > 1 else default_abc_path()
    t0 = time.time()

    def log(msg):
        print(f"[{time.time() - t0:7.1f}s] {msg}", flush=True)

    build(dog_dir, abc_path, log=log)
    log("done")


if __name__ == "__main__":
    main()
