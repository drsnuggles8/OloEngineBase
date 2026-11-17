"""The Drift boat generator: an original Nordic clinker (lapstrake) boat with one square sail.

Run headless (Blender 5.x), from the repository root:

    blender -b --factory-startup --python OloEditor/SandboxProject/Assets/Models/DriftBoat/build_drift_boat.py -- [out_dir] [preview_dir]

and it writes, into out_dir (default: this directory), everything the engine loads:

    DriftBoat.gltf + .bin            the hull, deck, mast and standing rigging (LOD 0)
    DriftBoat_LOD1 / _LOD2           the same boat at range, on the same textures
    DriftBoatSail.gltf (+ LODs)      the yard and sail, origined on the mast axis
    DriftBoatRudder.gltf             the side rudder and tiller, origined on its shaft
    DriftBoatLine.gltf               one metre of rope along +Z, for the sheets and braces
    DriftBoatHull*.png, DriftBoatRig*.png   colour, normal and occlusion-roughness-metallic maps of
                                     the two materials (the hull; the sail, rudder and lines)
    DriftBoat.rig.json               the measured numbers the scene is built from

Nothing is hand-edited after generation. README.md describes the design and the contract with
the engine.

The hull is a function of the parameters in HULL: length, beam, sheer, rocker, freeboard,
deadrise, and the number, widths and overlap of the strakes. Every strake is a real plank lying on
the one below it, so the laps show in the silhouette and in the lighting.

Blender does two things here: it traces the ambient occlusion (Cycles) and renders the previews.
The geometry, the UV atlas, the colour, roughness and height painting and the glTF files are this
script's own, so every level of detail lands on the same texels and a rebuild is byte-stable.
"""

import json
import math
import os
import struct
import sys
import time
import zlib

import numpy as np

# ----------------------------------------------------------------------------
# ENGINE coordinates throughout: metres, +Y up, the bow faces +Z, +X is PORT (the engine's
# starboard is forward x up, which for a +Z bow is -X; see Scene/EntityFacing.h). y = 0 is the
# waterline the hull floats at when it displaces HULL["mass"]; z = 0 is its centre of buoyancy.
# glTF uses the same frame. Blender is Z up, so a point (x, y, z) here is Blender's (x, -z, y).
# ----------------------------------------------------------------------------

F64 = np.float64
F32 = np.float32

HULL = {
    "mass": 2600.0,            # kg: the displacement the game's rigid body has (Drift.olo)
    "water_density": 1000.0,   # kg/m^3: BuoyancyComponent's FluidDensity in the scene
    "length": 7.20,            # m, between the stem rabbets at the sheer
    "beam": 2.60,              # m, over the planking at the sheer amidships
    "depth": 1.14,             # m, rabbet to sheer amidships
    "sheer_rise_bow": 0.86,    # m, the sheer at the bow above the sheer amidships
    "sheer_rise_stern": 0.70,
    "sheer_end_slope_deg": 30.0,   # how steeply the sheer runs into each stem
    "rocker": 0.05,            # m, the keel's rise at its ends
    "keel_start": 1.30,        # m from the aft end to where the keel ends and the sternpost begins
    "keel_end": 1.45,          # m from the fore end to where the keel ends and the stem begins
    "rake_bow_deg": 16.0,      # the stem's lean from vertical where the sheer meets it
    "rake_stern_deg": 12.0,
    "entry_deg": 33.0,         # half-angle of the sheer line in plan at the bow
    "exit_deg": 35.0,          # the same at the stern
    "deadrise_deg": 19.0,      # the garboard's rise from horizontal amidships
    "flare_deg": 13.0,         # the topside's lean outward at the sheer amidships
    "bilge": 0.44,             # how firm the turn of the bilge is (Bezier handle lengths)
    "strakes": 7,              # planks a side
    "strake_widths": (1.12, 1.04, 1.0, 1.0, 1.0, 0.98, 0.90),  # relative girth of each, keel up
    "plank": 0.022,            # m, plank thickness
    "lap": 0.040,              # m, how far each strake overlaps the one below
    "hood_gamma": 1.08,        # how the hood ends spread up the stems (1 = evenly by height)
    "half_siding": 0.020,      # m, half-breadth of the planking where it lands on stem and keel
    "keel_depth": 0.13,        # m, keel below the rabbet
    "keel_siding": 0.085,      # m, keel and stem thickness at the rabbet
    "stem_head_bow": 0.66,     # m, the stem head above the sheer
    "stem_head_stern": 0.52,
}

RIG = {
    "mast_z": -0.50,           # m, the mast aft of the centre of buoyancy
    "deck_y": 0.30,            # m, the main deck above the waterline
    "aft_deck_z": -2.05,       # the raised after deck starts here
    "aft_deck_y": 0.62,
    "fore_deck_z": 2.20,
    "fore_deck_y": 0.70,
    "mast_truck_y": 5.25,      # m, the masthead above the waterline
    "mast_radius": 0.075,      # m at the deck
    "yard_y": 4.32,
    "yard_length": 4.70,
    "sail_head": 4.30,         # m, sail width at the yard
    "sail_foot": 4.62,
    "sail_hoist": 2.92,        # m, head to foot
    "sail_belly": 0.40,        # m, how far the cloth stands forward of the yard
    "rudder_z": -2.55,
    "rudder_max_deg": 28.0,
}

# What the boat has to FEEL like, whatever hull is drawn (tuned for Drift in issue #899 and
# measured on that tuning). physics_from_hull() turns these and the hull's measurements into the
# component values Drift.olo authors, so a new hull changes the numbers and not the game.
GAME_FEEL = {
    "submergence_ramp": 0.8,          # m: BuoyancyComponent's SubmergenceRamp (a probe is fully wet this deep)
    "heel_stiffness": 62141.0,        # N/rad: roll stiffness about the centre of mass, per metre the sail's force acts
                                      # above it. It is what sets how far a given wind lays her over (1.2 deg in the
                                      # breeze, 4.4 in the gale), and a gust that doubles the wind quadruples the force.
    "heel_lever": 1.4815,             # half-beams: how far above the centre of mass the sail's force acts (2.0 m on a
                                      # 1.35 m half-beam). Weight x half-beam / lever is the side force that lifts her
                                      # windward bilge out of the water, after which she goes over fast: 17 kN.
    "hull_grip": 0.877,               # how much of the box's resting immersion the hull drag takes to bite fully
    "weather_helm": 5.73e-5,          # 1/(kg m): how far abaft the centre of mass the sail's force acts, per unit of
                                      # yaw inertia (0.36 m on a 6283 kg m^2 body). It is what makes a loaded sail
                                      # nudge the bow up into the wind, and a gust round her up instead of bearing
                                      # her away.
    "surge_damping": 0.0748,          # 1/s: buoyancy's share of the forward damping (LinearDrag x wet fraction)
    "roll_decay": 1.217,              # 1/s: how fast a roll dies (AngularDrag x wet fraction x mass / roll inertia)
    "yaw_time_constant": 1.098,       # s: how long the helm takes to answer (yaw inertia / (YawDrag x mass))
    "turn_rate": 0.944,               # rad/s: steady turn at full rudder (MaxRudderTorque / (YawDrag x mass))
}

# Where each coarser level of detail takes over (metres from the camera), and the view the
# scene is played through: a level is judged by the silhouette it draws at the distance it
# first appears, one pixel of that view at a time.
LOD_DISTANCES = (30.0, 90.0)
VIEW_FOV_Y = 1.1                 # rad: Drift's camera
VIEW_HEIGHT_PX = 1080
LOD_MIN_SILHOUETTE = 0.90        # least overlap (intersection over union) a level may have with level 0

ATLAS_SIZE = int(os.environ.get("OLO_BOAT_ATLAS", "4096"))       # the hull's maps
SAIL_ATLAS_SIZE = int(os.environ.get("OLO_BOAT_SAIL_ATLAS", "2048"))
AO_SAMPLES = int(os.environ.get("OLO_BOAT_AO_SAMPLES", "48"))
ATLAS_PAD = 6


def unit(v):
    v = np.asarray(v, dtype=F64)
    n = np.linalg.norm(v, axis=-1, keepdims=True)
    return v / np.maximum(n, 1e-12)


def bezier(p0, p1, p2, p3, t):
    t = np.asarray(t, dtype=F64)[..., None]
    s = 1.0 - t
    return s * s * s * p0 + 3 * s * s * t * p1 + 3 * s * t * t * p2 + t * t * t * p3


def fair(p0, d0, p3, d3, a, b, t):
    """A Bezier leaving p0 along d0 and arriving at p3 along d3, its handles reaching a and b of
    the way to where the two tangents cross. Convex by construction: no hollow, no S."""
    p0, d0, p3, d3 = (np.asarray(q, dtype=F64) for q in (p0, d0, p3, d3))
    m = np.array([[d0[0], d3[0]], [d0[1], d3[1]]])
    s, _u = np.linalg.solve(m, p3 - p0)
    x = p0 + s * d0
    return bezier(p0, p0 + a * (x - p0), p3 + b * (x - p3), p3, t)


def smoothstep(e0, e1, x):
    t = np.clip((np.asarray(x, dtype=F64) - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def polyline_lengths(p):
    d = np.linalg.norm(np.diff(p, axis=0), axis=1)
    return np.concatenate([[0.0], np.cumsum(d)])


def resample(pts, n):
    """n points evenly spaced by arc length along a polyline."""
    pts = np.asarray(pts, dtype=F64)
    s = polyline_lengths(pts)
    t = np.linspace(0.0, s[-1], n)
    return np.stack([np.interp(t, s, pts[:, k]) for k in range(pts.shape[1])], axis=1)


def name_seed(name):
    return zlib.crc32(name.encode("utf-8")) & 0xFFFFFFFF


def name_rand(name, salt=0):
    return ((name_seed(name) ^ (salt * 0x9E3779B1)) & 0xFFFFFFFF) / 4294967296.0


class Curve2:
    """A plane curve y(x) sampled from a parametric polyline whose x is monotonic."""

    def __init__(self, pts):
        pts = np.asarray(pts, dtype=F64)
        if pts[0, 0] > pts[-1, 0]:
            pts = pts[::-1]
        self.x = pts[:, 0]
        self.y = pts[:, 1]
        assert np.all(np.diff(self.x) > -1e-12), "curve is not a function of x"

    def __call__(self, x):
        return np.interp(x, self.x, self.y)


# ----------------------------------------------------------------------------
# The hull form. Built in a construction frame (y = 0 at the rabbet amidships, z = 0 at mid
# length), then floated: float_at_mass() finds the waterline and the centre of buoyancy and every
# *_e accessor below answers in the engine frame that puts the origin on them.
# ----------------------------------------------------------------------------


class HullForm:
    def __init__(self, p=HULL):
        self.p = dict(p)
        L = p["length"]
        self.za, self.zf = -0.5 * L, 0.5 * L
        self.zm = 0.0                       # station of maximum beam and lowest sheer
        self.zka = self.za + p["keel_start"]
        self.zkf = self.zf - p["keel_end"]
        self.b0 = p["half_siding"]
        self.y_shift = 0.0
        self.z_shift = 0.0
        ys_m = p["depth"]
        self.ys_bow = ys_m + p["sheer_rise_bow"]
        self.ys_stern = ys_m + p["sheer_rise_stern"]
        tt = np.linspace(0.0, 1.0, 400)

        def sheer_half(z_end, y_end):
            sgn = np.sign(z_end - self.zm)
            s = math.radians(p["sheer_end_slope_deg"])
            return fair([self.zm, ys_m], [sgn, 0.0], [z_end, y_end], [sgn * math.cos(s), math.sin(s)], 0.80, 0.50, tt)

        fore = sheer_half(self.zf, self.ys_bow)
        aft = sheer_half(self.za, self.ys_stern)
        self.sheer = Curve2(np.concatenate([aft[::-1], fore[1:]]))

        half = 0.5 * p["beam"] - p["plank"]     # the moulded surface is the planking's inside

        def plan_half(z_end, angle_deg):
            sgn = np.sign(z_end - self.zm)
            a = math.radians(angle_deg)
            return fair([self.zm, half], [sgn, 0.0], [z_end, self.b0], [sgn * math.cos(a), -math.sin(a)], 0.86, 0.52, tt)

        fore = plan_half(self.zf, p["entry_deg"])
        aft = plan_half(self.za, p["exit_deg"])
        self.plan = Curve2(np.concatenate([aft[::-1], fore[1:]]))

        zc = 0.5 * (self.zka + self.zkf)
        hk = 0.5 * (self.zkf - self.zka)
        zk = np.linspace(self.zka, self.zkf, 120)
        yk = p["rocker"] * ((zk - zc) / hk) ** 2
        keel = np.stack([zk, yk], axis=1)
        self.y_keel_end = float(p["rocker"])
        slope_k = 2.0 * p["rocker"] / hk

        def stem(z_k, z_end, y_end, rake_deg, sgn):
            r = math.radians(rake_deg)
            return fair([z_k, self.y_keel_end], [sgn, slope_k], [z_end, y_end], [sgn * math.sin(r), math.cos(r)], 0.78, 0.56, tt)

        self.stem_bow_pts = stem(self.zkf, self.zf, self.ys_bow, p["rake_bow_deg"], 1.0)
        self.stem_stern_pts = stem(self.zka, self.za, self.ys_stern, p["rake_stern_deg"], -1.0)
        self.rabbet = Curve2(np.concatenate([self.stem_stern_pts[::-1], keel[1:-1], self.stem_bow_pts]))

        w = np.asarray(p["strake_widths"][: p["strakes"]], dtype=F64)
        assert len(w) == p["strakes"], "strake_widths needs one entry per strake"
        self.lands = np.concatenate([[0.0], np.cumsum(w) / np.sum(w)])
        self._section_cache = {}

    # -- lines (construction frame) -------------------------------------------

    def fullness(self, z):
        """1 amidships, falling to 0 at the stems: how much shape the sections carry."""
        z = np.asarray(z, dtype=F64)
        zeta = np.where(z >= self.zm, (z - self.zm) / (self.zf - self.zm), (z - self.zm) / (self.za - self.zm))
        return 1.0 - np.clip(zeta, 0.0, 1.0) ** 2.6

    def gmin(self, z):
        """The part of the virtual girth the rising stem has taken at station z. A land ends
        (its hood end) where gmin reaches its girth, so the hood ends climb the stem in turn."""
        z = np.asarray(z, dtype=F64)
        yr = self.rabbet(z)
        gam = self.p["hood_gamma"]
        g = np.zeros_like(z)
        g = np.where(z > self.zkf, np.clip((yr - self.y_keel_end) / (self.ys_bow - self.y_keel_end), 0.0, 1.0) ** gam, g)
        g = np.where(z < self.zka, np.clip((yr - self.y_keel_end) / (self.ys_stern - self.y_keel_end), 0.0, 1.0) ** gam, g)
        return g

    def land_end(self, g, bow):
        """The station where the land at virtual girth g meets the stem (bisection on gmin)."""
        lo, hi = (self.zkf, self.zf) if bow else (self.za, self.zka)
        for _ in range(60):
            mid = 0.5 * (lo + hi)
            below = self.gmin(np.array([mid]))[0] < g
            if bow:
                lo, hi = (mid, hi) if below else (lo, mid)
            else:
                lo, hi = (lo, mid) if below else (mid, hi)
        return 0.5 * (lo + hi)

    def section(self, z, n=49):
        """The moulded half-section at station z: a polyline of (x, y) from the rabbet to the
        sheer, with its cumulative girth."""
        key = (round(float(z), 9), n)
        hit = self._section_cache.get(key)
        if hit is not None:
            return hit
        p = self.p
        p0 = np.array([self.b0, float(self.rabbet(z))])
        p3 = np.array([float(self.plan(z)), float(self.sheer(z))])
        chord = p3 - p0
        clen = float(np.linalg.norm(chord))
        if clen < 1e-6:
            pts = np.repeat(p0[None, :], n, axis=0)
            out = (pts, np.zeros(n))
            self._section_cache[key] = out
            return out
        f = float(self.fullness(z))
        th = math.atan2(chord[1], chord[0])
        mid_chord = math.atan2(p["depth"], 0.5 * p["beam"] - p["plank"] - self.b0)
        a0 = (mid_chord - math.radians(p["deadrise_deg"])) * f
        a3 = (math.radians(90.0 - p["flare_deg"]) - mid_chord) * f
        d0 = np.array([math.cos(th - a0), math.sin(th - a0)])
        d3 = np.array([math.cos(th + a3), math.sin(th + a3)])
        k = p["bilge"]
        pts = bezier(p0, p0 + k * clen * d0, p3 - k * 0.92 * clen * d3, p3, np.linspace(0.0, 1.0, n))
        out = (pts, polyline_lengths(pts))
        self._section_cache[key] = out
        return out

    def section_at(self, z, g):
        """Points and outward unit normals (in the section plane) of the moulded surface at
        station z for virtual girths g (array). Girths below gmin(z) clamp to the rabbet."""
        pts, s = self.section(z)
        g = np.asarray(g, dtype=F64)
        gm = float(self.gmin(np.array([z]))[0])
        tau = np.clip((g - gm) / max(1.0 - gm, 1e-9), 0.0, 1.0)
        girth = s[-1]
        if girth < 1e-9:
            xy = np.repeat(pts[:1], len(g), axis=0)
            return xy, np.tile(np.array([1.0, 0.0]), (len(g), 1)), girth, gm
        sv = tau * girth
        x = np.interp(sv, s, pts[:, 0])
        y = np.interp(sv, s, pts[:, 1])
        e = 1e-3 * girth
        lo, hi = np.clip(sv - e, 0, girth), np.clip(sv + e, 0, girth)
        t = unit(np.stack([np.interp(hi, s, pts[:, 0]) - np.interp(lo, s, pts[:, 0]),
                           np.interp(hi, s, pts[:, 1]) - np.interp(lo, s, pts[:, 1])], axis=1))
        nrm = np.stack([t[:, 1], -t[:, 0]], axis=1)
        return np.stack([x, y], axis=1), nrm, girth, gm

    def stations(self, n, power=1.0):
        """n stations from stern to bow, clustered toward the stems."""
        t = np.linspace(0.0, 1.0, n)
        c = 0.5 - 0.5 * np.cos(np.pi * t)
        t = (1.0 - power) * t + power * c
        return self.za + (self.zf - self.za) * t

    # -- hydrostatics ----------------------------------------------------------

    def immersed(self, w, n=161):
        """Displaced volume, its centroid, and the waterplane at waterline y = w (construction
        frame), from the planking's outside plus the keel below the rabbet."""
        p = self.p
        zs = self.stations(n, 0.6)
        area = np.zeros(n)
        mom_y = np.zeros(n)
        half_b = np.zeros(n)
        for i, z in enumerate(zs):
            pts, _ = self.section(float(z), 97)
            seg = np.diff(pts, axis=0)
            if not np.any(np.abs(seg) > 1e-9):
                continue
            nrm = unit(np.stack([seg[:, 1], -seg[:, 0]], axis=1))
            nv = np.zeros_like(pts)
            nv[:-1] += nrm
            nv[1:] += nrm
            out = pts + unit(nv) * p["plank"]
            x, y = out[:, 0], out[:, 1]
            yc = np.minimum(y, w)
            dy = np.diff(yc)
            xm = 0.5 * (x[1:] + x[:-1])
            ym = 0.5 * (yc[1:] + yc[:-1])
            a = float(np.sum(xm * dy))
            yr = float(self.rabbet(z))
            kd = p["keel_depth"]
            kh = max(min(w, yr) - (yr - kd), 0.0)
            ka = 0.5 * p["keel_siding"] * kh
            area[i] = 2.0 * (a + ka)
            mom_y[i] = 2.0 * (float(np.sum(xm * dy * ym)) + ka * (yr - kd + 0.5 * kh))
            if y[0] < w < y[-1]:
                half_b[i] = float(np.interp(w, y, x))
        vol = float(np.trapezoid(area, zs))
        if vol <= 0.0:
            return {"volume": 0.0}
        wet = zs[half_b > 0.0]
        return {
            "volume": vol,
            "lcb": float(np.trapezoid(area * zs, zs) / vol),
            "vcb": float(np.trapezoid(mom_y, zs) / vol),
            "beam_wl": 2.0 * float(half_b.max()),
            "lwl": float(wet[-1] - wet[0]) if len(wet) else 0.0,
            "z_wl_aft": float(wet[0]) if len(wet) else 0.0,
            "z_wl_fore": float(wet[-1]) if len(wet) else 0.0,
            "waterplane_area": 2.0 * float(np.trapezoid(half_b, zs)),
            "waterplane_inertia": (2.0 / 3.0) * float(np.trapezoid(half_b ** 3, zs)),
        }

    def float_at_mass(self):
        """Find the waterline the hull floats at, and return the measurements at it."""
        target = self.p["mass"] / self.p["water_density"]
        lo, hi = -self.p["keel_depth"], self.p["depth"]
        for _ in range(50):
            mid = 0.5 * (lo + hi)
            if self.immersed(mid)["volume"] < target:
                lo = mid
            else:
                hi = mid
        w = 0.5 * (lo + hi)
        h = self.immersed(w)
        h["waterline_above_rabbet"] = w
        self.y_shift = -w
        self.z_shift = -h["lcb"]
        self.hydro = h
        return h

    # -- the engine frame -------------------------------------------------------

    def E(self, x, yc, zc):
        """Construction (x, y, z) -> engine frame."""
        return np.stack([np.asarray(x, dtype=F64), np.asarray(yc, dtype=F64) + self.y_shift,
                         np.asarray(zc, dtype=F64) + self.z_shift], axis=-1)

    def zc(self, ze):
        return np.asarray(ze, dtype=F64) - self.z_shift

    def sheer_e(self, ze):
        """Sheer height (engine y) at engine station ze."""
        return self.sheer(self.zc(ze)) + self.y_shift

    def plan_e(self, ze):
        """Moulded sheer half-breadth at engine station ze."""
        return self.plan(self.zc(ze))

    def half_breadth_e(self, ze, ye):
        """Moulded half-breadth of the hull at engine station ze and height ye (0 below the
        rabbet, the sheer's above it)."""
        pts, _ = self.section(float(self.zc(ze)))
        yc = float(ye) - self.y_shift
        if yc <= pts[0, 1]:
            return 0.0
        return float(np.interp(yc, pts[:, 1], pts[:, 0]))

    def section_e(self, ze, n=25):
        """The moulded section at engine station ze as engine (x, y) points, rabbet to sheer,
        with outward normals."""
        pts, s = self.section(float(self.zc(ze)))
        sv = np.linspace(0.0, s[-1], n)
        x = np.interp(sv, s, pts[:, 0])
        y = np.interp(sv, s, pts[:, 1]) + self.y_shift
        q = np.stack([x, y], axis=1)
        t = unit(np.gradient(q, axis=0))
        return q, np.stack([t[:, 1], -t[:, 0]], axis=1)

    @property
    def stern_e(self):
        return self.za + self.z_shift

    @property
    def bow_e(self):
        return self.zf + self.z_shift


# ----------------------------------------------------------------------------
# Mesh building. Everything is a GRID of points: quads between neighbours, smooth within the
# grid, a hard edge wherever two grids meet. Each grid is one chart in the texture atlas, so its
# UVs are its own arc lengths in metres and a plank's grain runs along it.
# ----------------------------------------------------------------------------


class Grid:
    __slots__ = ("name", "pos", "nrm", "uv", "kind", "density", "wrap_v", "tint", "atlas_uv", "chart", "flipped")

    def __init__(self, name, pos, kind, density=1.0, wrap_v=False, tint=0.0, chart=None):
        self.name = name
        self.pos = np.asarray(pos, dtype=F64)
        self.kind = kind
        self.density = density
        self.wrap_v = wrap_v
        self.tint = tint
        self.chart = chart or name
        self.nrm = None
        self.uv = None
        self.atlas_uv = None
        self.flipped = False


def grid_normals(pos, wrap_v=False):
    """Smooth per-vertex normals of a grid, as d/du x d/dv (central differences)."""
    du = np.gradient(pos, axis=0)
    if wrap_v:
        ring = pos[:, :-1]
        dv_ring = 0.5 * (np.roll(ring, -1, axis=1) - np.roll(ring, 1, axis=1))
        dv = np.concatenate([dv_ring, dv_ring[:, :1]], axis=1)
    else:
        dv = np.gradient(pos, axis=1)
    n = np.cross(du, dv)
    ln = np.linalg.norm(n, axis=-1, keepdims=True)
    bad = ln[..., 0] < 1e-14
    if np.any(bad) and not np.all(bad):
        # A degenerate vertex (a pointed plank end, a cap's centre) takes the grid's mean normal.
        mean = unit(np.sum(np.where(bad[..., None], 0.0, n / np.maximum(ln, 1e-14)), axis=(0, 1)))
        n = np.where(bad[..., None], mean, n)
        ln = np.linalg.norm(n, axis=-1, keepdims=True)
    return n / np.maximum(ln, 1e-14)


def grid_uv(pos):
    """Chart coordinates in metres: u is the arc length along the grid's middle line, v the arc
    length across at each station, so a tapering plank keeps its texture scale."""
    nu, nv = pos.shape[:2]
    u = polyline_lengths(pos[:, nv // 2])
    dv = np.linalg.norm(np.diff(pos, axis=1), axis=2)
    v = np.concatenate([np.zeros((nu, 1)), np.cumsum(dv, axis=1)], axis=1)
    return np.stack([np.repeat(u[:, None], nv, axis=1), v], axis=2)


class MeshBuilder:
    def __init__(self):
        self.grids = []

    def add(self, name, pos, kind, outward=None, away_from=None, density=1.0, wrap_v=False, tint=None, chart=None):
        """Add a grid. `outward` (a vector, or one per vertex) or `away_from` (a point) says which
        way the surface faces; the grid is flipped to agree."""
        pos = np.asarray(pos, dtype=F64)
        assert pos.ndim == 3 and pos.shape[0] >= 2 and pos.shape[1] >= 2, name
        g = Grid(name, pos, kind, density, wrap_v, name_rand(chart or name) if tint is None else tint, chart)
        n = grid_normals(g.pos, wrap_v)
        if away_from is not None:
            outward = g.pos - np.asarray(away_from, dtype=F64)
        if outward is not None:
            if float(np.sum(n * np.broadcast_to(np.asarray(outward, dtype=F64), n.shape))) < 0.0:
                g.pos = g.pos[:, ::-1].copy()
                g.flipped = True
                n = grid_normals(g.pos, wrap_v)
        g.nrm = n
        g.uv = grid_uv(g.pos)
        self.grids.append(g)
        return g

    def mirrored(self, name, pos, kind, outward=None, **kw):
        """Add a grid and its mirror image across the centreplane (x -> -x)."""
        mx = np.array([-1.0, 1.0, 1.0])
        self.add(name + "_P", pos, kind, outward=outward, **kw)
        self.add(name + "_S", np.asarray(pos, dtype=F64) * mx, kind,
                 outward=None if outward is None else np.asarray(outward, dtype=F64) * mx, **kw)

    def triangles(self):
        return sum(2 * (g.pos.shape[0] - 1) * (g.pos.shape[1] - 1) for g in self.grids)

    def arrays(self):
        """Flat vertex, normal, quad and owning-grid arrays of every grid."""
        vs, ns, quads, owner = [], [], [], []
        base = 0
        for gi, g in enumerate(self.grids):
            nu, nv = g.pos.shape[:2]
            vs.append(g.pos.reshape(-1, 3))
            ns.append(g.nrm.reshape(-1, 3))
            i, j = np.meshgrid(np.arange(nu - 1), np.arange(nv - 1), indexing="ij")
            a = base + i * nv + j
            quads.append(np.stack([a, a + nv, a + nv + 1, a + 1], axis=-1).reshape(-1, 4))
            owner.append(np.full((nu - 1) * (nv - 1), gi))
            base += nu * nv
        return np.concatenate(vs), np.concatenate(ns), np.concatenate(quads), np.concatenate(owner)


def frames_along(path, up=(0.0, 1.0, 0.0), fixed=False):
    """Tangent, normal and binormal along a path. The normal starts as `up` made square to the
    path and is carried along it (or re-derived from `up` at every point when fixed)."""
    path = np.asarray(path, dtype=F64)
    t = unit(np.gradient(path, axis=0))
    up = np.asarray(up, dtype=F64)
    n = np.zeros_like(path)

    def square(v, tan):
        v = v - tan * np.dot(v, tan)
        if np.linalg.norm(v) < 1e-6:
            alt = np.array([1.0, 0.0, 0.0]) if abs(tan[0]) < 0.9 else np.array([0.0, 0.0, 1.0])
            v = alt - tan * np.dot(alt, tan)
        return unit(v)

    n[0] = square(up, t[0])
    for i in range(1, len(path)):
        n[i] = square(up if fixed else n[i - 1], t[i])
    return t, n, np.cross(t, n)


def tube(mb, name, path, radius, kind, sides=8, caps=(True, True), up=(0.0, 1.0, 0.0), **kw):
    """A round bar along a path, with an optional flat cap at each end."""
    path = np.asarray(path, dtype=F64)
    r = np.broadcast_to(np.asarray(radius, dtype=F64), (len(path),))
    t, n, b = frames_along(path, up)
    a = np.linspace(0.0, 2.0 * np.pi, sides + 1)
    ring = np.cos(a)[None, :, None] * n[:, None, :] + np.sin(a)[None, :, None] * b[:, None, :]
    pos = path[:, None, :] + r[:, None, None] * ring
    pos[:, -1] = pos[:, 0]
    mb.add(name, pos, kind, outward=ring, wrap_v=True, **kw)
    for end, flag, sgn in ((0, caps[0], -1.0), (len(path) - 1, caps[1], 1.0)):
        if flag and r[end] > 1e-5:
            cap = np.stack([np.repeat(path[end][None, :], sides + 1, axis=0), pos[end]], axis=0)
            kw2 = dict(kw)
            kw2["density"] = 0.5 * kw.get("density", 1.0)
            mb.add(f"{name}_cap{0 if end == 0 else 1}", cap, kind, outward=sgn * t[end], **kw2)


def sweep(mb, name, path, profile, kind, up=(0.0, 1.0, 0.0), fixed=True, smooth=False, caps=(True, True),
          faces=None, scale=None, **kw):
    """A closed profile swept along a path. `profile` is (m, 2) points in (binormal, normal)
    coordinates, counter-clockwise; each edge is its own hard-edged face unless smooth. `faces`
    picks which edges to build (the hidden ones cost atlas for nothing); `scale` (n,) tapers it."""
    path = np.asarray(path, dtype=F64)
    prof = np.asarray(profile, dtype=F64)
    t, n, b = frames_along(path, up, fixed)
    s = np.ones(len(path)) if scale is None else np.asarray(scale, dtype=F64)
    pts = (path[:, None, :] + s[:, None, None] * (prof[None, :, 0, None] * b[:, None, :] + prof[None, :, 1, None] * n[:, None, :]))
    m = len(prof)
    centre = path[:, None, :]
    if smooth:
        ring = np.concatenate([pts, pts[:, :1]], axis=1)
        mb.add(name, ring, kind, outward=ring - centre, wrap_v=True, **kw)
    else:
        for e in range(m):
            if faces is not None and e not in faces:
                continue
            pair = np.stack([pts[:, e], pts[:, (e + 1) % m]], axis=1)
            mid = 0.5 * (prof[e] + prof[(e + 1) % m])
            out = mid[0] * b + mid[1] * n
            mb.add(f"{name}_f{e}", pair, kind, outward=out[:, None, :], **kw)
    for end, flag, sgn in ((0, caps[0], -1.0), (len(path) - 1, caps[1], 1.0)):
        if flag:
            ring = np.concatenate([pts[end], pts[end][:1]], axis=0)
            cap = np.stack([np.repeat(path[end][None, :], m + 1, axis=0), ring], axis=0)
            kw2 = dict(kw)
            kw2["density"] = 0.5 * kw.get("density", 1.0)
            mb.add(f"{name}_cap{0 if end == 0 else 1}", cap, kind, outward=sgn * t[end], **kw2)


def rect_profile(half_w, half_h, chamfer=0.0):
    if chamfer <= 0.0:
        return np.array([[-half_w, -half_h], [half_w, -half_h], [half_w, half_h], [-half_w, half_h]])
    c = chamfer
    return np.array([[-half_w + c, -half_h], [half_w - c, -half_h], [half_w, -half_h + c], [half_w, half_h - c],
                     [half_w - c, half_h], [-half_w + c, half_h], [-half_w, half_h - c], [-half_w, -half_h + c]])


def box(mb, name, centre, half, kind, yaw=0.0, **kw):
    """An axis-aligned box (optionally yawed about +Y), six hard-edged faces."""
    c = np.asarray(centre, dtype=F64)
    hx, hy, hz = half
    cy, sy = math.cos(yaw), math.sin(yaw)
    rot = np.array([[cy, 0.0, sy], [0.0, 1.0, 0.0], [-sy, 0.0, cy]])
    for axis, sgn, tag in ((0, 1, "px"), (0, -1, "nx"), (1, 1, "py"), (1, -1, "ny"), (2, 1, "pz"), (2, -1, "nz")):
        o = [0, 1, 2]
        o.remove(axis)
        hs = [hx, hy, hz]
        quad = np.zeros((2, 2, 3))
        for i, a in enumerate((-1.0, 1.0)):
            for j, bb in enumerate((-1.0, 1.0)):
                q = np.zeros(3)
                q[axis] = sgn * hs[axis]
                q[o[0]] = a * hs[o[0]]
                q[o[1]] = bb * hs[o[1]]
                quad[i, j] = c + rot @ q
        nrm = np.zeros(3)
        nrm[axis] = sgn
        mb.add(f"{name}_{tag}", quad, kind, outward=rot @ nrm, **kw)


def lathe(mb, name, base, profile, kind, sides=16, smooth=True, **kw):
    """A profile of (radius, height) turned about the vertical through `base`."""
    base = np.asarray(base, dtype=F64)
    prof = np.asarray(profile, dtype=F64)
    a = np.linspace(0.0, 2.0 * np.pi, sides + 1)
    ring = np.stack([np.cos(a), np.zeros_like(a), np.sin(a)], axis=1)
    pos = base + prof[:, None, 0, None] * ring[None, :, :] + prof[:, None, 1, None] * np.array([0.0, 1.0, 0.0])
    pos[:, -1] = pos[:, 0]
    axis_pts = base + prof[:, None, 1, None] * np.array([0.0, 1.0, 0.0])
    if smooth:
        mb.add(name, pos, kind, outward=pos - axis_pts, wrap_v=True, **kw)
    else:
        for k in range(len(prof) - 1):
            seg = pos[k:k + 2]
            d = prof[k + 1] - prof[k]
            out = d[1] * ring[None, :, :] - d[0] * np.array([0.0, 1.0, 0.0])
            mb.add(f"{name}_s{k}", seg, kind, outward=np.broadcast_to(out, seg.shape), wrap_v=True, **kw)


# ----------------------------------------------------------------------------
# Planking
# ----------------------------------------------------------------------------


def build_strakes(mb, hull, lod=0):
    """Every strake as a solid plank: flat across its width, its lower edge lying outside the
    strake below by one plank thickness, the lap fading out toward the hood ends (the gains)."""
    p = hull.p
    th, lap = p["plank"], p["lap"]
    bevel = 0.004
    base = hull.stations((132, 56, 26)[lod], 0.75)
    for i in range(p["strakes"]):
        g_lo, g_hi = hull.lands[i], hull.lands[i + 1]
        top = g_hi > 1.0 - 1e-9
        z_a = hull.za if top else hull.land_end(g_hi, bow=False)
        z_f = hull.zf if top else hull.land_end(g_hi, bow=True)
        zs = np.concatenate([[z_a], base[(base > z_a + 1e-4) & (base < z_f - 1e-4)], [z_f]])
        rows = {k: [] for k in ("outer", "bevel", "bottom", "inner", "top")}
        hint = []
        for z in zs:
            z = float(z)
            gm = float(hull.gmin(np.array([z]))[0])
            _, _, girth, _ = hull.section_at(z, np.array([g_hi]))
            lap_g = lap * (1.0 - gm) / max(girth, 1e-6)
            gl = min(max(g_lo - lap_g, gm) if i > 0 else gm, g_hi)
            xy, nrm, _, _ = hull.section_at(z, np.array([gl, g_hi]))
            # How much of the lap is left: 1 in the body, 0 once the strake below has ended.
            lapf = float(smoothstep(0.0, 1.0, (g_lo - gm) / (0.75 * (g_hi - g_lo)))) if i > 0 else 0.0
            l_in = xy[0] + nrm[0] * th * lapf
            u_in = xy[1]
            d = u_in - l_in
            width = float(np.linalg.norm(d))
            dirv = d / width if width > 1e-7 else np.array([-nrm[1][1], nrm[1][0]])
            m = np.array([dirv[1], -dirv[0]])
            u_out = u_in + th * m
            l_out = l_in + th * m
            b = min(bevel, 0.3 * width)
            c1 = l_out + dirv * b
            c2 = l_out - m * b

            def P(q, z=z):
                return hull.E(q[0], q[1], z)

            rows["outer"].append([P(c1 if lod == 0 else l_out), P(u_out)])
            rows["bevel"].append([P(c2), P(c1)])
            rows["bottom"].append([P(l_in), P(c2 if lod == 0 else l_out)])
            rows["inner"].append([P(u_in), P(l_in)])
            rows["top"].append([P(u_out), P(u_in)])
            hint.append(m)
        hint = np.asarray(hint)
        out3 = np.stack([hint[:, 0], hint[:, 1], np.zeros(len(hint))], axis=1)[:, None, :]
        dn3 = np.array([0.0, -1.0, 0.0])
        kind = "plank_paint" if i == p["strakes"] - 1 else "plank_tar"
        mb.mirrored(f"strake{i}_outer", np.asarray(rows["outer"]), kind, outward=out3)
        if lod == 0:
            mb.mirrored(f"strake{i}_bevel", np.asarray(rows["bevel"]), kind, outward=out3 + dn3, density=0.6)
        # A strake's inside shows only where it stands above the deck (or beyond the decks, in
        # the eyes of the boat). One that lies wholly under the boards gets few texels.
        inner = np.asarray(rows["inner"])
        shown = np.array([(q[2] < hull.stern_e + 0.30) or (q[2] > hull.bow_e - 0.34) or (q[1] > deck_height(q[2]) - 0.03)
                          for q in inner.reshape(-1, 3)])
        inside_density = 1.0 if float(np.mean(shown)) > 0.04 else 0.25
        if lod <= 1:
            mb.mirrored(f"strake{i}_bottom", np.asarray(rows["bottom"]), kind, outward=dn3 - 0.2 * out3, density=0.6)
            mb.mirrored(f"strake{i}_top", np.asarray(rows["top"]), "plank_inside", outward=-dn3, density=0.6 * inside_density)
        mb.mirrored(f"strake{i}_inner", inner, "plank_inside", outward=-out3, density=inside_density)


# ----------------------------------------------------------------------------
# Keel and stems: one backbone from the sternpost head, down, along the keel, up to the stem head
# ----------------------------------------------------------------------------


def backbone_path(hull, lod=0):
    """The rabbet line as a parametric curve in (z, y), construction frame, carried above the
    sheer by the two stem heads. Returns the points, a part tag and how far up each head it is."""
    p = hull.p
    tt = np.linspace(0.0, 1.0, 40)

    def head(stem_pts, rise, sgn):
        s = stem_pts[-1]
        tan = unit(stem_pts[-1] - stem_pts[-3])
        tip = s + np.array([sgn * 0.10 * rise, rise])
        curl = math.radians(13.0)
        return fair(s, tan, tip, [-sgn * math.sin(curl), math.cos(curl)], 0.62, 0.62, tt)

    bow_head = head(hull.stem_bow_pts, p["stem_head_bow"], 1.0)
    stern_head = head(hull.stem_stern_pts, p["stem_head_stern"], -1.0)
    n_head, n_stem, n_keel = ((16, 46, 30), (8, 20, 12), (4, 10, 6))[lod]
    zk = np.linspace(hull.zka, hull.zkf, n_keel)
    keel = np.stack([zk, hull.rabbet(zk)], axis=1)
    parts = [resample(stern_head, n_head)[::-1], resample(hull.stem_stern_pts, n_stem)[::-1][1:], keel[1:],
             resample(hull.stem_bow_pts, n_stem)[1:], resample(bow_head, n_head)[1:]]
    pts = np.concatenate(parts)
    tag = np.concatenate([np.full(len(q), k) for k, q in enumerate(parts)])
    head_frac = np.zeros(len(pts))
    n0 = len(parts[0])
    head_frac[:n0] = np.linspace(1.0, 0.0, n0)
    n4 = len(parts[4])
    head_frac[-n4:] = np.linspace(0.0, 1.0, n4 + 1)[1:]
    return pts, tag, head_frac


def build_backbone(mb, hull, lod=0):
    p = hull.p
    pts, tag, head = backbone_path(hull, lod)
    tan = unit(np.gradient(pts, axis=0))
    out = np.stack([tan[:, 1], -tan[:, 0]], axis=1)       # outward in the centreplane: down / fore / aft
    n = len(pts)
    s = polyline_lengths(pts)
    on_keel = (tag == 2).astype(F64)
    keel_w = smoothstep(0.0, 0.5, np.minimum(s - s[tag == 2][0], s[tag == 2][-1] - s)) * on_keel
    taper = 1.0 - 0.50 * head ** 1.6                                    # the heads thin toward their tips
    depth = (0.105 + (p["keel_depth"] - 0.105) * keel_w) * taper       # moulded depth outside the rabbet
    side_r = 0.5 * p["keel_siding"] * taper                             # half-siding at the rabbet
    side_o = side_r * (0.50 + 0.22 * keel_w)                            # at the outer face
    inner = (0.050 + 0.02 * smoothstep(0.0, 0.2, head)) * taper        # how far it stands inside the planking
    side_i = side_r * 0.94

    def ring(offset_out, half):
        c = pts + out * offset_out[:, None]
        return hull.E(half, c[:, 1], c[:, 0])

    mx = np.array([-1.0, 1.0, 1.0])
    r_in, r_rab, r_out = ring(-inner, side_i), ring(np.zeros(n), side_r), ring(depth, side_o)
    out3 = np.stack([np.zeros(n), out[:, 1], out[:, 0]], axis=1)[:, None, :]
    px = np.array([1.0, 0.0, 0.0])
    mb.mirrored("backbone_side", np.stack([r_rab, r_out], axis=1), "timber_tar", outward=px + 0.3 * out3)
    mb.mirrored("backbone_inside", np.stack([r_in, r_rab], axis=1), "timber_tar", outward=px - 0.3 * out3, density=0.5)
    mb.add("backbone_face", np.stack([r_out, r_out * mx], axis=1), "timber_tar", outward=out3)
    mb.add("backbone_top", np.stack([r_in, r_in * mx], axis=1), "timber_tar", outward=-out3, density=0.5)
    for end, name in ((0, "backbone_cap_stern"), (n - 1, "backbone_cap_bow")):
        quad = np.array([[r_in[end], r_in[end] * mx], [r_out[end], r_out[end] * mx]])
        mb.add(name, quad, "timber_tar", outward=np.array([0.0, 1.0, 0.0]), density=0.5)
    # Where the heads end, in the engine frame (the forestay lands on the bow one).
    tip_bow = hull.E(0.0, pts[-1, 1], pts[-1, 0])
    tip_stern = hull.E(0.0, pts[0, 1], pts[0, 0])
    return {"stem_head_bow": tip_bow.tolist(), "stem_head_stern": tip_stern.tolist(),
            "keel_bottom_y": float(-p["keel_depth"] + hull.y_shift)}


# ----------------------------------------------------------------------------
# Gunwales, frames, beams, decks
# ----------------------------------------------------------------------------


def sheer_line(hull, z0, z1, n, inset=0.0, drop=0.0):
    """Points along the sheer between engine stations z0 and z1, moved inboard by `inset` and
    down by `drop` (port side)."""
    z = np.linspace(z0, z1, n)
    x = hull.plan_e(z) - inset
    y = hull.sheer_e(z) - drop
    return np.stack([x, y, z], axis=1)


def build_gunwales(mb, hull, lod=0):
    p = hull.p
    n = (72, 30, 14)[lod]
    z0, z1 = hull.stern_e + 0.22, hull.bow_e - 0.24
    # The inwale: a stout rail inside the sheer strake, its top a hand's breadth of worn wood.
    path = sheer_line(hull, z0, z1, n, inset=0.030, drop=0.022)
    prof = rect_profile(0.030, 0.030, 0.006)
    for side, tag in ((1.0, "P"), (-1.0, "S")):
        sweep(mb, f"inwale_{tag}", path * np.array([side, 1.0, 1.0]), prof, "timber_worn", caps=(True, True))
    # The rubbing strake: a half-round on the outside of the sheer strake's top edge.
    if lod <= 1:
        path = sheer_line(hull, z0 + 0.05, z1 - 0.05, n, inset=-(p["plank"] + 0.004), drop=0.030)
        a = np.linspace(-0.5 * np.pi, 0.5 * np.pi, 6 if lod == 0 else 3)
        prof_p = np.stack([0.024 * np.cos(a), 0.030 * np.sin(a)], axis=1)
        for side, tag in ((1.0, "P"), (-1.0, "S")):
            q = path * np.array([side, 1.0, 1.0])
            _t, up, b = frames_along(q, (0.0, 1.0, 0.0), True)
            # The binormal points to one side of the direction of travel; turn it outboard.
            outb = b if float(np.sum(b[:, 0])) * side > 0 else -b
            pos = q[:, None, :] + prof_p[None, :, 0, None] * outb[:, None, :] + prof_p[None, :, 1, None] * up[:, None, :]
            mb.add(f"rubrail_{tag}", pos, "trim_paint", outward=outb[:, None, :])


def build_tholes(mb, hull, lod=0):
    """Keiper, the oar-rests of a Nordic boat: three a side on the gunwale, each a block with a
    horn the oar bears against. They are what tells the eye how big a person is aboard."""
    if lod >= 2:
        return
    for k, dz in enumerate((1.05, 0.22, -0.88)):
        z = RIG["mast_z"] + dz
        zs = np.linspace(z - 0.15, z + 0.15, 4)
        for side, tag in ((1.0, "P"), (-1.0, "S")):
            path = np.stack([side * (hull.plan_e(zs) - 0.014), hull.sheer_e(zs) + 0.028, zs], axis=1)
            sweep(mb, f"thole{k}_{tag}", path, rect_profile(0.024, 0.020, 0.005), "timber_worn")
            if lod == 0:
                base = path[0]
                horn = np.stack([base + np.array([0.0, 0.012, 0.020]), base + np.array([0.0, 0.070, 0.030]),
                                 base + np.array([0.0, 0.118, 0.066])])
                tube(mb, f"thole{k}_horn_{tag}", resample(horn, 5), np.linspace(0.021, 0.011, 5), "timber_worn", sides=8,
                     caps=(False, True), up=(1.0, 0.0, 0.0))


def frame_stations(hull):
    """Engine stations of the frames: one under the mast, the rest every 0.72 m."""
    z0 = RIG["mast_z"] + 0.02
    zs = z0 + 0.72 * np.arange(-4, 5)
    return [float(z) for z in zs if hull.stern_e + 0.9 < z < hull.bow_e - 0.9]


def deck_height(z):
    """The deck's height (engine y) at station z: main deck, raised fore and after decks."""
    if z < RIG["aft_deck_z"]:
        return RIG["aft_deck_y"]
    if z > RIG["fore_deck_z"]:
        return RIG["fore_deck_y"]
    return RIG["deck_y"]


def build_frames(mb, hull, lod=0):
    """The ribs where they show, between the deck and the gunwale, and a beam under the deck at
    each one. They stand a little into the planking, as a joggled frame does."""
    if lod >= 2:
        return
    for k, z in enumerate(frame_stations(hull)):
        yd = deck_height(z) - 0.04
        q, nrm = hull.section_e(z, 41)
        keep = (q[:, 1] > yd) & (q[:, 1] < hull.sheer_e(z) - 0.055)
        if np.count_nonzero(keep) < 3:
            continue
        q, nrm = q[keep], nrm[keep]
        if lod == 1:
            q, nrm = q[::3], nrm[::3]
            if len(q) < 2:
                continue
        moulded, sided = 0.055, 0.024
        outer = q + nrm * 0.014
        innr = q - nrm * moulded
        for side, tag in ((1.0, "P"), (-1.0, "S")):
            sx = np.array([side, 1.0, 1.0])

            def P3(xy, dz, sx=sx, z=z):
                return np.stack([xy[:, 0], xy[:, 1], np.full(len(xy), z + dz)], axis=1) * sx

            inward = np.stack([-nrm[:, 0] * side, -nrm[:, 1], np.zeros(len(nrm))], axis=1)[:, None, :]
            mb.add(f"frame{k}_in_{tag}", np.stack([P3(innr, -sided), P3(innr, sided)], axis=1), "timber", outward=inward)
            mb.add(f"frame{k}_fore_{tag}", np.stack([P3(outer, sided), P3(innr, sided)], axis=1), "timber",
                   outward=np.array([0.0, 0.0, 1.0]), density=0.8)
            mb.add(f"frame{k}_aft_{tag}", np.stack([P3(outer, -sided), P3(innr, -sided)], axis=1), "timber",
                   outward=np.array([0.0, 0.0, -1.0]), density=0.8)
            top = np.array([[P3(outer[-1:], -sided)[0], P3(outer[-1:], sided)[0]],
                            [P3(innr[-1:], -sided)[0], P3(innr[-1:], sided)[0]]])
            mb.add(f"frame{k}_top_{tag}", top, "timber", outward=np.array([0.0, 1.0, 0.0]), density=0.6)


def build_decks(mb, hull, lod=0):
    """Deck boards laid fore and aft on the beams, each one its own plank, trimmed to the hull's
    side. The main deck is low amidships; a raised deck closes each end. Under the boards a dark
    bed stands in for the bilge so the gaps between them read as gaps."""
    board, gap = 0.172, 0.006
    zones = [("deck_aft", hull.stern_e + 0.30, RIG["aft_deck_z"], RIG["aft_deck_y"]),
             ("deck_main", RIG["aft_deck_z"], RIG["fore_deck_z"], RIG["deck_y"]),
             ("deck_fore", RIG["fore_deck_z"], hull.bow_e - 0.34, RIG["fore_deck_y"])]
    info = {}
    for zone, z0, z1, y in zones:
        # Which stretch of each board lies inside the hull is decided on one fine set of
        # stations, so every level of detail has the same boards.
        fine = np.linspace(z0, z1, int(max(8, (z1 - z0) / 0.10)))
        hb_fine = np.array([hull.half_breadth_e(z, y) for z in fine]) + 0.006
        info[zone] = {"y": y, "z0": z0, "z1": z1, "half_breadth_max": float(hb_fine.max())}
        kmax = int(math.ceil(hb_fine.max() / (board + gap))) + 1
        for k in range(-kmax, kmax + 1):
            xc = k * (board + gap)
            lo, hi = xc - 0.5 * board, xc + 0.5 * board
            ok = (np.clip(hi, -hb_fine, hb_fine) - np.clip(lo, -hb_fine, hb_fine)) > 0.012
            if np.count_nonzero(ok) < 2:
                continue
            idx = np.nonzero(ok)[0]
            za, zb = float(fine[idx[0]]), float(fine[idx[-1]])
            nz = (int(max(3, (zb - za) / 0.16)), int(max(2, (zb - za) / 0.6)), 2)[lod] + 1
            zs = np.linspace(za, zb, nz)
            hb = np.interp(zs, fine, hb_fine)
            # Clip the board against the hull's side at every station.
            xa, xb = np.clip(lo, -hb, hb), np.clip(hi, -hb, hb)
            name = f"{zone}_board{k + kmax:02d}"
            dy = 0.003 * (name_rand(name, 3) - 0.5)
            tilt = 0.008 * (name_rand(name, 4) - 0.5)
            pos = np.stack([np.stack([xa, y + dy + tilt * (xa - xc), zs], axis=1),
                            np.stack([xb, y + dy + tilt * (xb - xc), zs], axis=1)], axis=1)
            mb.add(name, pos, "deck", outward=np.array([0.0, 1.0, 0.0]))
        nz = (int(max(8, (z1 - z0) / 0.16)), 6, 3)[lod]
        zs = np.linspace(z0, z1, nz)
        hb = np.interp(zs, fine, hb_fine)
        bed = np.stack([np.stack([-hb, np.full(nz, y - 0.022), zs], axis=1),
                        np.stack([hb, np.full(nz, y - 0.022), zs], axis=1)], axis=1)
        mb.add(f"{zone}_bed", bed, "bilge", outward=np.array([0.0, 1.0, 0.0]), density=0.25)
    # The two steps: a board wall from the main deck up to each raised deck.
    for name, z, y_top, facing in (("step_aft", RIG["aft_deck_z"], RIG["aft_deck_y"], 1.0),
                                   ("step_fore", RIG["fore_deck_z"], RIG["fore_deck_y"], -1.0)):
        ys = np.linspace(RIG["deck_y"] - 0.02, y_top + 0.004, 5 if lod == 0 else 2)
        nx = 13 if lod == 0 else 3
        rows = []
        for yy in ys:
            hbw = hull.half_breadth_e(z, float(yy)) + 0.006
            rows.append(np.stack([np.linspace(-hbw, hbw, nx), np.full(nx, yy), np.full(nx, z)], axis=1))
        mb.add(name, np.stack(rows, axis=0), "deck_wall", outward=np.array([0.0, 0.0, facing]))
        # A coaming board along the top of the step.
        hbw = hull.half_breadth_e(z, y_top) + 0.004
        path = np.stack([np.linspace(-hbw, hbw, 5), np.full(5, y_top + 0.012), np.full(5, z + facing * 0.012)], axis=1)
        if lod == 0:
            sweep(mb, name + "_coaming", path, rect_profile(0.03, 0.02, 0.004), "timber_worn", up=(0.0, 1.0, 0.0))
    return info


def build_beams(mb, hull, lod=0):
    """The crossbeams at the gunwale: the mast beam with its partner, and one fore and one aft."""
    out = {}
    specs = [("beam_mast", RIG["mast_z"] - 0.17, 0.055, 0.075), ("beam_fore", RIG["mast_z"] + 1.46, 0.04, 0.055),
             ("beam_aft", RIG["mast_z"] - 1.44, 0.04, 0.055)]
    for name, z, hh, hw in specs:
        y = float(hull.sheer_e(z)) - 0.075 - hh
        hb = hull.half_breadth_e(z, y) + 0.008
        path = np.stack([np.linspace(-hb, hb, 7 if lod == 0 else 2), np.full(7 if lod == 0 else 2, y),
                         np.full(7 if lod == 0 else 2, z)], axis=1)
        sweep(mb, name, path, rect_profile(hw, hh, 0.008), "timber_worn", caps=(False, False))
        out[name] = {"y_top": y + hh, "z": z, "half_breadth": hb}
        if lod == 0:
            # Hanging knees: a triangular web under each end of the beam, against the hull's side.
            arm, web = 0.26, 0.02
            for side, tag in ((1.0, "P"), (-1.0, "S")):
                x0 = side * (hb - 0.004)                                   # where the beam meets the side
                xi = x0 - side * arm                                       # the knee's inboard toe, under the beam
                xl = side * hull.half_breadth_e(z, y - hh - arm)           # its lower toe, down the side
                yt, yl = y - hh, y - hh - arm
                face = np.array([[[xi, yt, z - web], [xi, yt, z + web]], [[xl, yl, z - web], [xl, yl, z + web]]])
                mb.add(f"{name}_knee_face_{tag}", face, "timber", outward=np.array([-side, -1.0, 0.0]), density=0.8)
                for zz, sg, t3 in ((z + web, 1.0, "f"), (z - web, -1.0, "a")):
                    tri = np.array([[[x0, yt, zz], [xi, yt, zz]], [[xl, yl, zz], [xl, yl, zz]]])
                    mb.add(f"{name}_knee_{t3}_{tag}", tri, "timber", outward=np.array([0.0, 0.0, sg]), density=0.8)
    return out


def build_breasthooks(mb, hull, lod=0):
    """A knee across each end at the sheer, tying the gunwales to the stem."""
    if lod >= 2:
        return
    for name, z_tip, sgn in (("breasthook_bow", hull.bow_e - 0.10, -1.0), ("breasthook_stern", hull.stern_e + 0.09, 1.0)):
        n = 7
        zs = z_tip + sgn * np.linspace(0.0, 0.50, n)
        xs = np.maximum(hull.plan_e(zs) - 0.012, 0.012)
        ys = hull.sheer_e(zs) - 0.034
        top = np.stack([np.stack([-xs, ys, zs], axis=1), np.stack([xs, ys, zs], axis=1)], axis=1)
        mb.add(name + "_top", top, "timber_worn", outward=np.array([0.0, 1.0, 0.0]))
        end = np.array([[[-xs[-1], ys[-1], zs[-1]], [xs[-1], ys[-1], zs[-1]]],
                        [[-xs[-1], ys[-1] - 0.05, zs[-1]], [xs[-1], ys[-1] - 0.05, zs[-1]]]])
        mb.add(name + "_end", end, "timber", outward=np.array([0.0, 0.0, sgn]), density=0.7)
        bot = np.stack([np.stack([-xs, ys - 0.05, zs], axis=1), np.stack([xs, ys - 0.05, zs], axis=1)], axis=1)
        mb.add(name + "_under", bot, "timber", outward=np.array([0.0, -1.0, 0.0]), density=0.4)


# ----------------------------------------------------------------------------
# Mast and standing rigging
# ----------------------------------------------------------------------------


def catenary(a, b, sag, n):
    """n points from a to b hanging `sag` metres at mid-span."""
    a, b = np.asarray(a, dtype=F64), np.asarray(b, dtype=F64)
    t = np.linspace(0.0, 1.0, n)
    pts = a[None, :] + (b - a)[None, :] * t[:, None]
    pts[:, 1] -= sag * 4.0 * t * (1.0 - t)
    return pts


def build_mast(mb, hull, lod=0):
    mz = RIG["mast_z"]
    sides = (20, 10, 6)[lod]
    y0 = RIG["deck_y"] - 0.03
    y1 = RIG["mast_truck_y"]
    n = (26, 8, 3)[lod]
    ys = np.linspace(y0, y1, n)
    t = (ys - y0) / (y1 - y0)
    r = RIG["mast_radius"] * (1.0 - 0.42 * t ** 1.4)
    path = np.stack([np.zeros(n), ys, np.full(n, mz)], axis=1)
    tube(mb, "mast", path, r, "spar", sides=sides, caps=(False, True), up=(0.0, 0.0, 1.0))
    hounds_y = y1 - 0.34
    if lod == 0:
        # The hounds: a shoulder the shrouds and stay sit on, and the truck above it.
        rt = float(np.interp(hounds_y, ys, r))
        lathe(mb, "mast_hounds", [0.0, 0.0, mz], [(rt + 0.004, hounds_y - 0.05), (rt + 0.028, hounds_y - 0.02),
                                                 (rt + 0.028, hounds_y + 0.02), (rt + 0.004, hounds_y + 0.05)], "spar", sides=sides)
        lathe(mb, "mast_truck", [0.0, 0.0, mz], [(float(r[-1]) + 0.002, y1 - 0.03), (float(r[-1]) + 0.022, y1 - 0.005),
                                                (float(r[-1]) + 0.016, y1 + 0.03), (0.004, y1 + 0.045)], "spar", sides=sides)
        # The mast partner: a block on the mast beam with the mast in its jaws, and wedges.
        zb = mz - 0.17
        yb = float(hull.sheer_e(zb)) - 0.075
        box(mb, "mast_partner_P", [0.13, yb + 0.012, mz - 0.04], [0.05, 0.035, 0.14], "timber_worn")
        box(mb, "mast_partner_S", [-0.13, yb + 0.012, mz - 0.04], [0.05, 0.035, 0.14], "timber_worn")
        lathe(mb, "mast_coat", [0.0, 0.0, mz], [(RIG["mast_radius"] + 0.030, RIG["deck_y"] - 0.004),
                                               (RIG["mast_radius"] + 0.022, RIG["deck_y"] + 0.04),
                                               (RIG["mast_radius"] + 0.004, RIG["deck_y"] + 0.07)], "leather", sides=sides)
    return {"z": mz, "deck_y": RIG["deck_y"], "truck_y": y1, "hounds_y": hounds_y, "radius_at_deck": RIG["mast_radius"],
            "radius_at_hounds": float(np.interp(hounds_y, ys, r))}


def build_standing_rigging(mb, hull, mast, anchors, lod=0):
    """Forestay to the stem head, two shrouds a side led well aft (so the yard can brace 45
    degrees inside them), and the halyard's fall to the after beam."""
    mz = RIG["mast_z"]
    sides = (8, 5, 3)[lod]
    seg = (14, 5, 2)[lod]
    fat = (1.0, 1.25, 2.2)[lod]      # a line a pixel wide shimmers: the far levels draw it thicker
    hy = mast["hounds_y"]
    rh = mast["radius_at_hounds"]
    head = np.array(anchors["stem_head_bow"])
    stay_end = head + np.array([0.0, -0.12, -0.03])
    tube(mb, "forestay", catenary([0.0, hy + 0.02, mz + rh], stay_end, 0.03, seg), 0.011 * fat, "rope_tar", sides=sides)
    out = {"forestay": [[0.0, hy + 0.02, mz + rh], stay_end.tolist()], "shrouds": []}
    for k, dz in enumerate((-1.50, -2.05)):
        z = mz + dz
        for side, tag in ((1.0, "P"), (-1.0, "S")):
            x = side * (float(hull.plan_e(z)) - 0.035)
            y = float(hull.sheer_e(z)) - 0.01
            top = [side * rh * 0.8, hy + 0.01 - 0.02 * k, mz - rh * 0.5]
            tube(mb, f"shroud{k}_{tag}", catenary(top, [x, y + 0.16, z], 0.012, seg), 0.010 * fat, "rope_tar", sides=sides)
            out["shrouds"].append([top, [x, y, z]])
            if lod == 0:
                # The lanyard: three turns between the shroud's eye and a ring on the inwale.
                tube(mb, f"lanyard{k}_{tag}", catenary([x, y + 0.165, z], [x, y - 0.03, z], 0.0, 4), 0.016, "rope_hemp", sides=6)
                lathe(mb, f"shroud_eye{k}_{tag}", [x, 0.0, z], [(0.012, y + 0.15), (0.024, y + 0.17), (0.012, y + 0.19)], "rope_tar", sides=8)
    # Halyard: up the fore side from the yard's sling, over the truck, down to the after beam.
    beam_z = mz - 1.44
    beam_y = float(hull.sheer_e(beam_z)) - 0.075
    fall = catenary([0.0, RIG["mast_truck_y"] - 0.02, mz - 0.03], [0.22, beam_y + 0.02, beam_z + 0.03], 0.05, seg)
    tube(mb, "halyard_fall", fall, 0.008 * fat, "rope_hemp", sides=sides)
    if lod <= 1:
        tie = catenary([0.0, RIG["yard_y"] + 0.05, mz + 0.14], [0.0, RIG["mast_truck_y"] - 0.02, mz + 0.035], 0.0, 3)
        tube(mb, "halyard_tie", tie, 0.008 * fat, "rope_hemp", sides=sides)
    if lod == 0:
        # A coil of the halyard's tail hung on a pin at the after beam.
        a = np.linspace(0.0, 2.0 * np.pi * 4.0, 60)
        rr = 0.085 + 0.006 * np.sin(a * 0.31)
        coil = np.stack([0.22 + rr * np.cos(a) * 0.25, beam_y - 0.10 + rr * np.sin(a), beam_z + 0.045 + 0.0016 * a], axis=1)
        tube(mb, "halyard_coil", coil, 0.008, "rope_hemp", sides=6)
    return out


# ----------------------------------------------------------------------------
# Deck gear: what shows the boat's size
# ----------------------------------------------------------------------------


def build_gear(mb, hull, lod=0):
    """Two sea chests, a cask, a bucket, a pair of oars along the side, a coil of rope and the
    anchor on the foredeck."""
    if lod >= 2:
        return {}
    dy = RIG["deck_y"]
    info = {}
    # Sea chests: the rowing seats of an open boat, trapezoid in section, iron-bound.
    for k, (x, z, yaw) in enumerate(((0.52, RIG["mast_z"] + 1.02, 0.08), (-0.50, RIG["mast_z"] - 0.98, -0.12))):
        box(mb, f"chest{k}", [x, dy + 0.17, z], [0.30, 0.165, 0.185], "chest", yaw=yaw)
        box(mb, f"chest{k}_lid", [x, dy + 0.345, z], [0.315, 0.014, 0.20], "chest", yaw=yaw)
        if lod == 0:
            cy, sy = math.cos(yaw), math.sin(yaw)
            for s in (-0.19, 0.19):
                c = [x + cy * s, dy + 0.18, z - sy * s]
                box(mb, f"chest{k}_band{0 if s < 0 else 1}", c, [0.014, 0.182, 0.190], "iron", yaw=yaw, density=0.6)
    # A water cask, on its side in chocks.
    cx, cz = -0.62, RIG["mast_z"] + 0.86
    n = 9 if lod == 0 else 4
    t = np.linspace(-1.0, 1.0, n)
    rad = 0.19 * (1.0 - 0.16 * t * t)
    path = np.stack([np.full(n, cx), np.full(n, dy + 0.215), cz + 0.26 * t], axis=1)
    tube(mb, "cask", path, rad, "cask", sides=16 if lod == 0 else 8, caps=(True, True))
    if lod == 0:
        for j, tz in enumerate((-0.80, -0.42, 0.42, 0.80)):
            rr = 0.19 * (1.0 - 0.16 * tz * tz) + 0.004
            ring = np.stack([np.full(3, cx), np.full(3, dy + 0.215), cz + 0.26 * tz + np.array([-0.014, 0.0, 0.014])], axis=1)
            tube(mb, f"cask_hoop{j}", ring, rr, "iron", sides=16, caps=(False, False), density=0.6)
        for j, zz in enumerate((cz - 0.16, cz + 0.16)):
            box(mb, f"cask_chock{j}", [cx, dy + 0.03, zz], [0.17, 0.03, 0.025], "timber")
    # A bucket.
    bx, bz = 0.62, RIG["mast_z"] - 0.35
    lathe(mb, "bucket", [bx, 0.0, bz], [(0.105, dy + 0.004), (0.125, dy + 0.24)], "cask", sides=14 if lod == 0 else 7)
    if lod == 0:
        lathe(mb, "bucket_in", [bx, 0.0, bz], [(0.113, dy + 0.24), (0.095, dy + 0.02), (0.0, dy + 0.02)], "bilge", sides=14, smooth=False)
        lathe(mb, "bucket_rim", [bx, 0.0, bz], [(0.127, dy + 0.236), (0.127, dy + 0.244), (0.111, dy + 0.244)], "cask", sides=14, smooth=False)
        a = np.linspace(0.0, np.pi, 9)
        bail = np.stack([bx + 0.12 * np.cos(a), dy + 0.235 + 0.11 * np.sin(a) * 0.35, np.full(9, bz) + 0.11 * np.sin(a) * 0.9], axis=1)
        tube(mb, "bucket_bail", bail, 0.006, "rope_hemp", sides=5)
    # A pair of oars stowed along the port side, blades forward, lashed to the beams.
    for k, (inset, y) in enumerate(((0.10, dy + 0.045), (0.19, dy + 0.078))):
        z0, z1 = RIG["aft_deck_z"] + 0.10, RIG["aft_deck_z"] + 3.45
        n = 12 if lod == 0 else 3
        t = np.linspace(0.0, 1.0, n)
        x0 = hull.half_breadth_e(z0, y) - inset
        x1 = hull.half_breadth_e(z1, y) - inset - 0.05
        path = np.stack([x0 + (x1 - x0) * t, np.full(n, y), z0 + (z1 - z0) * t], axis=1)
        tube(mb, f"oar{k}_loom", path[: n - (3 if lod == 0 else 1)], 0.024, "spar", sides=10 if lod == 0 else 5)
        # The blade: a flat board at the fore end.
        bl = path[n - (4 if lod == 0 else 2):]
        hw = np.linspace(0.026, 0.075, len(bl))
        up = np.array([0.0, 1.0, 0.0])
        topf = np.stack([bl + np.array([1.0, 0.0, 0.0]) * -hw[:, None] + up * 0.010, bl + np.array([1.0, 0.0, 0.0]) * hw[:, None] + up * 0.010], axis=1)
        botf = topf - up * 0.020
        mb.add(f"oar{k}_blade_top", topf, "spar", outward=up)
        mb.add(f"oar{k}_blade_bot", botf, "spar", outward=-up, density=0.5)
    # A coil of rope on the after deck.
    if lod == 0:
        ax, az, ay = 0.30, RIG["aft_deck_z"] - 0.62, RIG["aft_deck_y"]
        a = np.linspace(0.0, 2.0 * np.pi * 5.0, 110)
        rr = 0.20 - 0.022 * a / (2.0 * np.pi)
        lay = np.floor(a / (2.0 * np.pi * 2.5))
        coil = np.stack([ax + rr * np.cos(a), ay + 0.014 + 0.024 * lay + 0.002 * np.sin(a * 3.0), az + rr * np.sin(a)], axis=1)
        tube(mb, "deck_coil", coil, 0.012, "rope_hemp", sides=6)
    # The anchor on the foredeck: a wooden stock, an iron shank and two flukes.
    fy = RIG["fore_deck_y"]
    az = RIG["fore_deck_z"] + 0.55
    shank = np.array([[0.10, fy + 0.035, az - 0.38], [0.10, fy + 0.035, az + 0.34]])
    tube(mb, "anchor_shank", resample(shank, 4), 0.017, "iron", sides=8 if lod == 0 else 4)
    sweep(mb, "anchor_stock", np.stack([np.linspace(-0.26, 0.46, 4), np.full(4, fy + 0.05), np.full(4, az - 0.33)], axis=1),
          rect_profile(0.03, 0.028, 0.006), "timber")
    # The arms lie flat on the deck, curving back from the crown toward the stock.
    a = np.linspace(-0.62 * np.pi, 0.62 * np.pi, 11 if lod == 0 else 5)
    arms = np.stack([0.10 + 0.23 * np.sin(a), np.full(len(a), fy + 0.035), az + 0.34 - 0.21 * (1.0 - np.cos(a))], axis=1)
    tube(mb, "anchor_arms", arms, 0.016, "iron", sides=8 if lod == 0 else 4)
    info["anchor"] = [0.10, fy + 0.035, az]
    return info


def rudder_geometry(hull):
    """Where the side rudder hangs: on the STARBOARD quarter (-X), its shaft lying along the
    hull's side, so it is canted inboard at the foot. Returns the pivot (on the shaft, at the
    gunwale), the cant and the shaft's stand-off."""
    z = RIG["rudder_z"]
    yg = float(hull.sheer_e(z))
    th = hull.p["plank"]
    x_top = hull.half_breadth_e(z, yg - 0.02) + th
    x_low = hull.half_breadth_e(z, 0.12) + th
    cant = math.atan2(x_top - x_low, yg - 0.02 - 0.12)
    stand = 0.075
    return {"pivot": [-(x_top + stand), yg, z], "cant": cant, "stand": stand, "gunwale_y": yg}


def build_rudder_mount(mb, hull, lod=0):
    """The boss the rudder turns on, and the strap that holds its head to the gunwale."""
    geo = rudder_geometry(hull)
    if lod >= 2:
        return geo
    px, yg, z = geo["pivot"]
    yb = 0.26
    xh = -(hull.half_breadth_e(z, yb) + hull.p["plank"])           # the hull's side at the boss
    xs = px + (yg - yb) * math.tan(geo["cant"])                    # the shaft's axis at that height
    n = 4
    t = np.linspace(0.0, 1.0, n)
    path = np.stack([xh + 0.03 + (xs + 0.025 - xh - 0.03) * t, np.full(n, yb), np.full(n, z)], axis=1)
    tube(mb, "rudder_boss", path, 0.085 - 0.035 * t, "timber", sides=10 if lod == 0 else 5, caps=(False, True), up=(0.0, 1.0, 0.0))
    if lod == 0:
        a = np.linspace(0.0, 2.0 * np.pi, 13)
        loop = np.stack([px + 0.035 + 0.070 * np.cos(a), np.full(13, yg - 0.03), z + 0.052 * np.sin(a)], axis=1)
        tube(mb, "rudder_strap", loop, 0.010, "leather", sides=6, caps=(False, False))
    return geo


# ----------------------------------------------------------------------------
# The sail: yard, cloth, bolt rope, robands and reef points, in the hull's frame with the mast
# axis at x = z = 0 (the file's origin), so the entity's Y rotation is the yard angle.
# ----------------------------------------------------------------------------


# The sail's shape, as numbers. The files hold SAIL_FULL; each morph target is the same sail drawn
# under different ones, vertex for vertex, and the game blends between them from the wind.
SAIL_FULL = {
    "belly": RIG["sail_belly"],   # m the cloth stands forward of the yard
    "hoist": 1.0,                 # share of the hoist that is set; the rest is gathered up to the yard
    "folds": 0.0,                 # the vertical folds of cloth hanging limp
    "aback": 0.0,                 # m the cloth is pressed AFT either side of the mast, wind on its fore face
    "wave": None,                 # (amplitude m, waves across, waves down, phase rad): a ripple standing in the cloth
}
SAIL_SHAPE = dict(SAIL_FULL)

# Travelling waves are made of three standing ones a third of a cycle apart, because a morph
# weight cannot go negative: weights (1 + cos(wt - k * 120 deg)) / 3 on the three sum to a wave
# of half the pattern's amplitude running through the cloth, and their steady parts cancel.
SAIL_MORPHS = [
    ("Slack", {"belly": 0.035, "folds": 1.0}),          # no wind: she hangs from the yard
    ("Hard", {"belly": 0.60}),                          # a gale in her
    ("Aback", {"belly": 0.0, "aback": 0.30}),           # wind on the fore face: pressed round the mast
    ("Furled", {"hoist": 0.07}),                        # gathered up to the yard
    ("Luff0", {"wave": (0.16, 1.5, 0.9, 0.0)}),         # luffing: big slow waves
    ("Luff120", {"wave": (0.16, 1.5, 0.9, 2.0 * math.pi / 3.0)}),
    ("Luff240", {"wave": (0.16, 1.5, 0.9, 4.0 * math.pi / 3.0)}),
    ("Ripple0", {"wave": (0.045, 3.5, 2.2, 0.0)}),      # the shiver that never quite stops
    ("Ripple120", {"wave": (0.045, 3.5, 2.2, 2.0 * math.pi / 3.0)}),
    ("Ripple240", {"wave": (0.045, 3.5, 2.2, 4.0 * math.pi / 3.0)}),
]


def sail_surface(s, t):
    """A point on the sail's mid-surface for s in [-1, 1] across and t in [0, 1] head to foot,
    relative to the mast axis, in the shape SAIL_SHAPE names. The cloth hangs from the yard and
    bellies forward (+Z), fullest two thirds of the way down; the foot arches up between the
    clews and the leeches hollow. The head and the two clews are where the yard and the sheets
    hold her, so no shape but a shortened hoist moves them."""
    sh = SAIL_SHAPE
    s = np.asarray(s, dtype=F64)
    t = np.asarray(t, dtype=F64)
    k = sh["hoist"]
    half = 0.5 * (RIG["sail_head"] + (RIG["sail_foot"] - RIG["sail_head"]) * t * k)
    half = half * (1.0 - 0.030 * np.sin(np.pi * t) * k)                   # hollow leeches
    x = s * half
    drop = RIG["sail_hoist"] * t * k
    arch = 0.13 * (1.0 - s * s) * t ** 3 * k                               # the foot's arch
    scallop = 0.012 * (0.5 - 0.5 * np.cos(2.0 * np.pi * (s * 0.5 + 0.5) * 14.0)) * (1.0 - t) ** 8   # between robands
    y = RIG["yard_y"] - 0.085 - drop + arch - scallop
    fill = (1.0 - np.abs(s) ** 2.3) * np.sin(np.pi * np.clip(t, 0.0, 1.0) ** 0.80 * 0.5 + 0.0) * (1.0 - 0.22 * t ** 4)
    edge = 0.030 * np.sin(np.pi * t) * (np.abs(s) ** 6)                   # the leeches curl a touch
    z = yard_z() + 0.012 + sh["belly"] * fill * (0.2 + 0.8 * k) + edge
    if sh["aback"] > 0.0:
        # The mast stands behind her middle, so she lies on it there and goes aft either side.
        z = z - sh["aback"] * fill * (1.0 - np.exp(-(x / 0.20) ** 2))
    if sh["folds"] > 0.0:
        z = z + sh["folds"] * 0.045 * np.sin(6.0 * np.pi * s) * t ** 1.3 * (1.0 - np.abs(s) ** 4)
    if k < 1.0:
        z = z + (1.0 - k) * 0.06 * np.sin(np.pi * t) * (1.0 - np.abs(s) ** 8)          # the bunt of the gathered cloth
    if sh["wave"] is not None:
        amp, across, down, phase = sh["wave"]
        held = np.sin(0.5 * np.pi * t) ** 1.2 * (1.0 - np.abs(s) ** 6 * t ** 6)       # still at the yard and at the clews
        z = z + amp * held * np.sin(np.pi * (across * s + down * t) + phase)
    return np.stack([x, y, z], axis=-1)


def yard_z():
    return RIG["mast_radius"] * 0.72 + 0.052


def build_sail(mb, lod=0):
    nu, nv = ((57, 41), (25, 17), (9, 7))[lod]
    s = np.linspace(-1.0, 1.0, nu)
    t = np.linspace(0.0, 1.0, nv)
    ss, tt = np.meshgrid(s, t, indexing="ij")
    mid = sail_surface(ss, tt)
    nrm = grid_normals(mid)
    if float(np.sum(nrm[..., 2])) < 0.0:
        nrm = -nrm
    th = 0.004
    fore = mid + nrm * th
    aft = mid - nrm * th
    mb.add("sail_fore", fore, "cloth", outward=nrm)
    mb.add("sail_aft", aft, "cloth_back", outward=-nrm)
    sides = (8, 5, 3)[lod]
    fat = (1.0, 1.2, 2.0)[lod]
    # The bolt rope sewn round the edge closes the two faces.
    edge = np.concatenate([mid[:, 0], mid[-1, 1:], mid[::-1, -1][1:], mid[0, ::-1][1:]])
    edge = resample(edge, (220, 70, 24)[lod])
    edge[-1] = edge[0]
    tube(mb, "bolt_rope", edge, 0.011 * fat, "rope_hemp", sides=sides, caps=(False, False), up=(0.0, 0.0, 1.0))
    # The yard, tapered to its arms, a little bowed.
    ny = (25, 9, 3)[lod]
    xs = np.linspace(-0.5 * RIG["yard_length"], 0.5 * RIG["yard_length"], ny)
    r = 0.050 * (1.0 - 0.52 * np.abs(xs / (0.5 * RIG["yard_length"])) ** 1.7)
    yard = np.stack([xs, RIG["yard_y"] - 0.035 * (xs / (0.5 * RIG["yard_length"])) ** 2, np.full(ny, yard_z())], axis=1)
    tube(mb, "yard", yard, r, "spar", sides=(14, 8, 5)[lod], up=(0.0, 1.0, 0.0))
    info = {"yard_arm_port": yard[-1].tolist(), "yard_arm_starboard": yard[0].tolist(),
            "clew_port": mid[-1, -1].tolist(), "clew_starboard": mid[0, -1].tolist()}
    if lod <= 1:
        # Robands: a turn of line round the yard at every scallop of the head.
        for k in range(15):
            sx = -1.0 + 2.0 * k / 14.0
            x = float(sx * 0.5 * RIG["sail_head"])
            yy = RIG["yard_y"] - 0.035 * (x / (0.5 * RIG["yard_length"])) ** 2
            rr = float(np.interp(x, xs, r)) + 0.006
            a = np.linspace(0.0, 2.0 * np.pi, 9 if lod == 0 else 5)
            ring = np.stack([np.full(len(a), x) + 0.004 * np.sin(a), yy + rr * np.cos(a), yard_z() + rr * np.sin(a)], axis=1)
            tube(mb, f"roband{k:02d}", ring, 0.006 * fat, "rope_hemp", sides=5 if lod == 0 else 3, caps=(False, False), up=(1.0, 0.0, 0.0), density=0.6)
        # The parrel: a loop of rope and wooden beads holding the yard to the mast.
        a = np.linspace(0.0, 2.0 * np.pi, 17 if lod == 0 else 7)
        rp = RIG["mast_radius"] * 0.72 + 0.016
        loop = np.stack([rp * np.sin(a), np.full(len(a), RIG["yard_y"] - 0.01), rp * np.cos(a) * 1.0], axis=1)
        tube(mb, "parrel", loop, 0.013 * fat, "rope_hemp", sides=6 if lod == 0 else 3, caps=(False, False))
    if lod == 0:
        # One row of reef points on each face.
        tr = 0.72
        for k in range(13):
            sx = -0.9 + 1.8 * k / 12.0
            c = sail_surface(np.array(sx), np.array(tr))
            n1 = unit(np.cross(sail_surface(np.array(sx + 0.01), np.array(tr)) - c, sail_surface(np.array(sx), np.array(tr + 0.01)) - c))
            if n1[2] < 0:
                n1 = -n1
            for face, sg in (("f", 1.0), ("a", -1.0)):
                a0 = c + n1 * sg * 0.006
                pts = np.stack([a0, a0 + n1 * sg * 0.018 + np.array([0.0, -0.05, 0.0]), a0 + n1 * sg * 0.012 + np.array([0.004 * sg, -0.20, 0.0])])
                tube(mb, f"reef{k:02d}{face}", resample(pts, 5), 0.005, "rope_hemp", sides=4, caps=(False, True), density=0.5)
    # The sail's area and centre of area, yard squared, in the file's own frame.
    a = mid[1:, 1:] - mid[:-1, :-1]
    b = mid[:-1, 1:] - mid[1:, :-1]
    cell = 0.5 * np.linalg.norm(np.cross(a, b), axis=-1)
    cc = 0.25 * (mid[1:, 1:] + mid[:-1, :-1] + mid[:-1, 1:] + mid[1:, :-1])
    area = float(cell.sum())
    info.update({"area": area, "centre_of_area": (np.sum(cc * cell[..., None], axis=(0, 1)) / area).tolist(),
                 "projected_area": float(RIG["sail_hoist"] * 0.5 * (RIG["sail_head"] + RIG["sail_foot"]))})
    return info


def sail_morph_targets(base, lod):
    """The sail's morph targets for one level of detail: name, position deltas and normal deltas
    over the vertices of `base`, in the order write_gltf writes them. Each is the whole sail
    rebuilt in another shape, so the bolt rope and the reef points go with the cloth."""
    base_v, base_n = base.arrays()[:2]
    base_n = unit(base_n)
    out = []
    for name, shape in SAIL_MORPHS:
        SAIL_SHAPE.clear()
        SAIL_SHAPE.update(SAIL_FULL)
        SAIL_SHAPE.update(shape)
        try:
            mb = MeshBuilder()
            info = build_sail(mb, lod)
        finally:
            SAIL_SHAPE.clear()
            SAIL_SHAPE.update(SAIL_FULL)
        assert len(mb.grids) == len(base.grids), name
        for g, g0 in zip(mb.grids, base.grids):
            assert g.name == g0.name and g.pos.shape == g0.pos.shape, (name, g.name)
            if g.flipped != g0.flipped:                # keep every vertex the vertex it is in the base
                g.pos = g.pos[:, ::-1].copy()
                g.nrm = -grid_normals(g.pos, g.wrap_v)
        v, n = mb.arrays()[:2]
        out.append({"name": name, "position": v - base_v, "normal": unit(n) - base_n,
                    "clew_port": info["clew_port"], "clew_starboard": info["clew_starboard"]})
    return out


def build_rudder(mb, geo, lod=0):
    """The side rudder, in its own frame: the origin is on the shaft at the gunwale, the shaft is
    the Y axis, the blade trails aft (-Z) and the tiller reaches inboard (+X, toward the
    centreline from the starboard quarter). The scene cants the entity about Z to lie along the
    hull's side and turns it about Y to steer; the tiller is pre-tilted so it lies level."""
    cant = geo["cant"]
    top = 0.36
    bottom = -(geo["gunwale_y"] + 0.72) / math.cos(cant)          # 0.72 m below the waterline, along the shaft
    blade = 1.28
    n = (12, 5, 3)[lod]
    ys = np.linspace(bottom + blade - 0.12, top, n)
    r = 0.030 + 0.007 * np.sin(np.linspace(0.0, np.pi, n))
    tube(mb, "rudder_shaft", np.stack([np.zeros(n), ys, np.zeros(n)], axis=1), r, "spar", sides=(12, 6, 4)[lod], up=(0.0, 0.0, 1.0))
    # The blade: a board, thick at the shaft and fine at its trailing edge, widest low down.
    nb = (14, 6, 3)[lod]
    yb = np.linspace(bottom, bottom + blade, nb)
    tb = (yb - bottom) / blade
    chord = 0.36 * (0.60 + 0.40 * np.sin(np.pi * np.clip(tb * 0.9 + 0.08, 0, 1))) * (1.0 - 0.30 * tb ** 3)
    lead = 0.055 * (1.0 - tb * 0.4)
    for side, tag in ((1.0, "p"), (-1.0, "s")):
        pos = np.stack([np.stack([np.full(nb, side * 0.028), yb, lead], axis=1),
                        np.stack([np.full(nb, side * 0.021), yb, lead - 0.5 * chord], axis=1),
                        np.stack([np.full(nb, side * 0.005), yb, lead - chord], axis=1)], axis=1)
        mb.add(f"rudder_blade_{tag}", pos, "spar_wet", outward=np.array([side, 0.0, 0.0]))
    lead_edge = np.stack([np.stack([np.full(nb, -0.028), yb, lead], axis=1), np.stack([np.full(nb, 0.028), yb, lead], axis=1)], axis=1)
    mb.add("rudder_blade_lead", lead_edge, "spar_wet", outward=np.array([0.0, 0.0, 1.0]), density=0.5)
    trail = np.stack([np.stack([np.full(nb, -0.005), yb, lead - chord], axis=1), np.stack([np.full(nb, 0.005), yb, lead - chord], axis=1)], axis=1)
    mb.add("rudder_blade_trail", trail, "spar_wet", outward=np.array([0.0, 0.0, -1.0]), density=0.5)
    foot = np.array([[[-0.028, bottom, lead[0]], [0.028, bottom, lead[0]]], [[-0.005, bottom, lead[0] - chord[0]], [0.005, bottom, lead[0] - chord[0]]]])
    mb.add("rudder_blade_foot", foot, "spar_wet", outward=np.array([0.0, -1.0, 0.0]), density=0.5)
    # The tiller: through the shaft's head, reaching inboard, worn round at the grip.
    nt = (10, 4, 2)[lod]
    length = 0.95
    tx = np.linspace(-0.06, length, nt)
    level = np.array([math.cos(cant), -math.sin(cant), 0.0])       # level once the entity is canted
    path = np.array([0.0, top - 0.10, 0.0]) + tx[:, None] * level + np.array([0.0, 1.0, 0.0]) * (0.035 * (tx / length) ** 2)[:, None]
    tube(mb, "rudder_tiller", path, 0.025 - 0.006 * (tx / length), "timber_worn", sides=(10, 6, 4)[lod], up=(0.0, 1.0, 0.0))
    return {"shaft_top_y": top, "blade_bottom_y": bottom, "tiller_length": length,
            "blade_bottom_world_y": geo["gunwale_y"] + bottom * math.cos(cant)}


def build_line(mb):
    """One metre of rope along +Z from the origin: the scene stretches a copy between two points
    for each sheet and brace."""
    n = 9
    path = np.stack([np.zeros(n), np.zeros(n), np.linspace(0.0, 1.0, n)], axis=1)
    tube(mb, "line", path, 0.009, "rope_hemp", sides=6, caps=(True, True), up=(0.0, 1.0, 0.0))


# ----------------------------------------------------------------------------
# The atlas: every grid gets a rectangle, at one texel density
# ----------------------------------------------------------------------------


class Atlas:
    def __init__(self, size, pad=ATLAS_PAD):
        self.size = size
        self.pad = pad
        self.charts = {}
        self.ppm = 0.0

    def _layout(self, items, ppm):
        size, pad = self.size, self.pad
        rects = []
        for name, w_m, h_m, dens in items:
            su = sv = ppm * dens
            w = max(int(math.ceil(w_m * su)), 2)
            h = max(int(math.ceil(h_m * sv)), 2)
            if w + 2 * pad > size:
                su *= (size - 2 * pad) / w
                w = size - 2 * pad
            if h + 2 * pad > size:
                return None
            rects.append((name, w + 2 * pad, h + 2 * pad, su, sv))
        # First-fit, tallest first: each chart goes on the first shelf with room for it, so the
        # short charts fill the ends of the shelves the long ones opened.
        order = sorted(range(len(rects)), key=lambda k: (-rects[k][2], -rects[k][1], rects[k][0]))
        shelves = []            # [y, height, x of the free end]
        top = 0
        placed = {}
        for k in order:
            name, w, h, su, sv = rects[k]
            for shelf in shelves:
                if shelf[2] + w <= size and h <= shelf[1]:
                    placed[name] = (shelf[2], shelf[0], w, h, su, sv)
                    shelf[2] += w
                    break
            else:
                if top + h > size:
                    return None
                shelves.append([top, h, w])
                placed[name] = (0, top, w, h, su, sv)
                top += h
        return placed

    def pack(self, grids, log=print):
        """Place every chart of these grids at the largest texel density that fits."""
        items = {}
        for g in grids:
            w, h = float(g.uv[..., 0].max()), float(g.uv[..., 1].max())
            old = items.get(g.chart)
            items[g.chart] = (g.chart, max(w, old[1]) if old else w, max(h, old[2]) if old else h, g.density)
        items = [items[k] for k in sorted(items)]
        lo, hi = 4.0, 4096.0
        best = None
        for _ in range(40):
            mid = 0.5 * (lo + hi)
            got = self._layout(items, mid)
            if got is None:
                hi = mid
            else:
                lo, best = mid, got
        assert best is not None, "the atlas cannot hold the charts at any density"
        self.ppm = lo
        self.charts = {name: {"rect": best[name], "w_m": w, "h_m": h} for name, w, h, _ in items}
        used = sum(r[2] * r[3] for r in best.values()) / float(self.size * self.size)
        log(f"[atlas] {self.size}^2: {len(items)} charts at {self.ppm:.0f} px/m ({1000.0 / self.ppm:.2f} mm a texel), {100.0 * used:.0f}% used")

    def assign(self, grids):
        """Atlas UVs (glTF convention: v down from the image's top) for grids whose charts are
        already placed. A coarser level of detail lands on its chart's same rectangle."""
        for g in grids:
            c = self.charts.get(g.chart)
            assert c is not None, f"grid {g.name} has no chart in the atlas (a LOD may only reuse LOD 0's charts)"
            x, y, w, h, su, sv = c["rect"]
            own_w, own_h = float(g.uv[..., 0].max()), float(g.uv[..., 1].max())
            ku = c["w_m"] / own_w if own_w > 1e-9 else 1.0
            kv = c["h_m"] / own_h if own_h > 1e-9 else 1.0
            px = x + self.pad + g.uv[..., 0] * ku * su
            py = y + self.pad + g.uv[..., 1] * kv * sv
            g.atlas_uv = np.stack([px / self.size, py / self.size], axis=-1)


# ----------------------------------------------------------------------------
# Painting. Each chart is painted in its own metres, with the world position, the normal and the
# traced occlusion of every texel to hand, so a rivet sits where the lap is and the green sits
# where the water is.
# ----------------------------------------------------------------------------


def _hash(ix, iy, seed):
    h = (ix.astype(np.int64) * 374761393 + iy.astype(np.int64) * 668265263 + int(seed) * 2246822519) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    h = h ^ (h >> 16)
    return (h & 0xFFFFFF).astype(F32) / F32(16777216.0)


def vnoise(x, y, seed=0):
    """Value noise in [0, 1], one octave."""
    x = np.asarray(x, dtype=F32)
    y = np.asarray(y, dtype=F32)
    x0, y0 = np.floor(x), np.floor(y)
    fx, fy = x - x0, y - y0
    fx = fx * fx * (3.0 - 2.0 * fx)
    fy = fy * fy * (3.0 - 2.0 * fy)
    ix, iy = x0.astype(np.int64), y0.astype(np.int64)
    a = _hash(ix, iy, seed)
    b = _hash(ix + 1, iy, seed)
    c = _hash(ix, iy + 1, seed)
    d = _hash(ix + 1, iy + 1, seed)
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy


def fbm(x, y, octaves=4, seed=0, gain=0.5, lac=2.03):
    out = np.zeros(np.broadcast(x, y).shape, dtype=F32)
    amp, total = 1.0, 0.0
    fx, fy = np.asarray(x, dtype=F32), np.asarray(y, dtype=F32)
    for o in range(octaves):
        out += amp * vnoise(fx, fy, seed + 101 * o)
        total += amp
        amp *= gain
        fx = fx * lac + 17.3
        fy = fy * lac + 9.1
    return out / total


def mix(a, b, t):
    """Linear blend. A colour (last axis 3) blended by a scalar field broadcasts over it."""
    a, b, t = np.asarray(a, dtype=F32), np.asarray(b, dtype=F32), np.asarray(t, dtype=F32)
    colour = (a.ndim >= 1 and a.shape[-1] == 3) or (b.ndim >= 1 and b.shape[-1] == 3)
    if colour and not (t.ndim == max(a.ndim, b.ndim) and t.shape[-1] == 3):
        t = t[..., None]
    return a * (1.0 - t) + b * t


def col(r, g, b):
    return np.array([r, g, b], dtype=F32)


class Texel:
    """What a painter knows about every texel of one chart."""

    def __init__(self, g, chart, pad, ao):
        x, y, w, h, su, sv = chart["rect"]
        nu, nv = g.pos.shape[:2]
        own_w, own_h = float(g.uv[..., 0].max()), float(g.uv[..., 1].max())
        px = (np.arange(w) + 0.5 - pad) / su
        py = (np.arange(h) + 0.5 - pad) / sv
        u = np.clip(px, 0.0, own_w)
        ug = g.uv[:, nv // 2, 0]
        fi = np.interp(u, ug, np.arange(nu))
        wst = g.uv[:, -1, 1]
        wu = np.interp(u, ug, wst)
        V = np.clip(py[:, None], 0.0, wu[None, :])
        ok = wst > 1e-6
        prof = np.mean(g.uv[ok, :, 1] / wst[ok, None], axis=0) if np.any(ok) else np.linspace(0.0, 1.0, nv)
        fj = np.interp(V / np.maximum(wu[None, :], 1e-9), prof, np.arange(nv))
        i0 = np.clip(np.floor(fi).astype(int), 0, nu - 2)
        ti = (fi - i0)[None, :, None]
        j0 = np.clip(np.floor(fj).astype(int), 0, nv - 2)
        tj = (fj - j0)[..., None]
        ii = np.broadcast_to(i0[None, :], j0.shape)

        def bil(a):
            return ((a[ii, j0] * (1 - ti) + a[ii + 1, j0] * ti) * (1 - tj) + (a[ii, j0 + 1] * (1 - ti) + a[ii + 1, j0 + 1] * ti) * tj)

        self.g = g
        self.name = g.name
        self.kind = g.kind
        self.seed = name_seed(g.chart) % 100000
        self.tint = F32(g.tint)
        self.shape = (h, w)
        self.U = np.broadcast_to(u[None, :].astype(F32), (h, w))
        self.V = V.astype(F32)
        self.W = np.broadcast_to(wu[None, :].astype(F32), (h, w))            # the chart's width here
        self.L = F32(own_w)                                                   # its length
        self.P = bil(g.pos).astype(F32)
        self.N = unit(bil(g.nrm)).astype(F32)
        if ao is None:
            self.ao = np.ones((h, w), dtype=F32)
        else:
            # Read the trace at the texel the CLAMPED chart coordinate lands on: the part of a
            # tapering chart's rectangle that no triangle covers was never traced.
            ix = np.clip(np.floor(self.U * su + pad).astype(int), pad, w - pad - 1)
            iy = np.clip(np.floor(V * sv + pad).astype(int), pad, h - pad - 1)
            self.ao = ao[y + iy, x + ix].astype(F32)
        self.su, self.sv = su, sv
        self.rect = (x, y, w, h)
        # Distance from the plank's lower and upper edges, whichever way the chart runs.
        low_first = float(np.mean(g.pos[:, 0, 1])) <= float(np.mean(g.pos[:, -1, 1]))
        self.from_low = self.V if low_first else (self.W - self.V)
        self.from_high = (self.W - self.V) if low_first else self.V
        # Offsets that give every chart its own stretch of the same noise.
        self.ox = F32(name_rand(g.chart, 1) * 311.0)
        self.oy = F32(name_rand(g.chart, 2) * 173.0)


def band(freq, c, across=True, texels=3.0):
    """A spatial frequency (cycles a metre) held to what this chart's texels can carry: a detail
    finer than `texels` texels is dropped, not left to alias into a weave."""
    return min(freq, (c.sv if across else c.su) / texels)


def fade(freq, c, across=True, texels=3.0):
    """How much of a feature at `freq` cycles a metre survives on this chart: 1 when it is well
    resolved, 0 when it would alias."""
    limit = (c.sv if across else c.su) / texels
    return float(np.clip(2.0 - freq / max(limit, 1e-6), 0.0, 1.0))


def wood_grain(c, along=1.0, across=1.0):
    """Streaks along the plank, cathedral figure across it, and the pores: three fields 0..1.
    Each is laid at the finest pitch the chart's texels can carry and no finer."""
    u, v = c.U + c.ox, c.V + c.oy
    streak = fbm(u * 1.3 * along, v * band(48.0 * across, c, texels=8.0), 3, c.seed)
    streak = np.clip(0.5 + (streak - 0.5) * 2.0, 0.0, 1.0)
    warp = fbm(u * 0.55 * along, v * 4.0 * across, 3, c.seed + 7)
    ring_f = band(26.0 * across, c, texels=5.0)
    rings = 0.5 + 0.5 * np.sin(2.0 * np.pi * (v * ring_f + 2.6 * warp + 0.35 * streak))
    rings = 0.5 + (rings - 0.5) * fade(ring_f, c, texels=3.5)
    pores = vnoise(u * 9.0 * along, v * band(220.0 * across, c, texels=2.5), c.seed + 13)
    return streak, rings, pores


def rivet_field(c, pitch, offset, radius):
    """Distance (m) to the nearest rivet of a row running `offset` metres above the plank's lower
    edge, and the texel's distance below that rivet (for the rust that weeps from it)."""
    du = np.mod(c.U + 0.5 * pitch, pitch) - 0.5 * pitch
    dv = c.from_low - offset
    r = np.sqrt(du * du + dv * dv)
    return r, du, -dv


WATERLINE_BAND = 0.20


def weathering(c, colour, rough, exposure=1.0):
    """What the sea and the sun do to anything on the outside of the boat: slime below the
    waterline, a pale band of dried salt just above it, grey where rain runs and the sun bleaches."""
    y = c.P[..., 1]
    wob = (fbm(c.P[..., 2] * 2.2 + 31.0, c.P[..., 0] * 2.2 + c.P[..., 1] * 0.7, 3, 4711) - 0.5) * 0.09
    under = smoothstep(0.035, -0.06, y + wob).astype(F32)
    slime = mix(col(0.050, 0.060, 0.034), col(0.085, 0.075, 0.045), fbm(c.U * 9.0, c.V * 30.0, 3, c.seed + 31))
    colour = mix(colour, slime, 0.82 * under)
    rough = mix(rough, 0.42, 0.7 * under)
    band = (smoothstep(-0.02, 0.05, y + wob) * smoothstep(WATERLINE_BAND + 0.10, 0.03, y + wob)).astype(F32)
    salt = band * (0.35 + 0.65 * fbm(c.U * 5.0 + 3.0, c.V * 18.0, 3, c.seed + 37))
    colour = mix(colour, col(0.50, 0.47, 0.42), 0.34 * salt * exposure)
    rough = mix(rough, 0.86, 0.5 * salt)
    up = np.clip(c.N[..., 1], 0.0, 1.0)
    bleach = (0.25 + 0.75 * up) * fbm(c.U * 1.7 + 11.0, c.V * 9.0, 3, c.seed + 41) * (1.0 - under) * exposure
    grey = np.mean(colour, axis=-1, keepdims=True) * col(1.02, 1.0, 0.96)
    colour = mix(colour, grey * 1.20 + 0.02, 0.20 * bleach)
    return colour, rough


def paint_plank_tar(c):
    streak, rings, pores = wood_grain(c)
    wood = mix(col(0.115, 0.080, 0.052), col(0.270, 0.200, 0.135), 0.55 * streak + 0.30 * rings + 0.15 * pores)
    # Tar: thick and dark in the laps and low down, rubbed thin where the plank stands proud.
    worn = fbm(c.U * 0.9 + 5.0, c.V * 7.0, 4, c.seed + 3)
    cover = np.clip(0.97 - 0.50 * smoothstep(0.50, 0.82, worn) - 0.22 * smoothstep(0.015, 0.0, c.from_low), 0.0, 1.0)
    cover = np.clip(cover + 0.5 * (1.0 - c.ao), 0.0, 1.0).astype(F32)
    tar = mix(col(0.016, 0.011, 0.008), col(0.046, 0.030, 0.019), streak) * (0.80 + 0.40 * c.tint)
    colour = mix(wood, tar, cover)
    rough = mix(0.82, 0.46 + 0.20 * streak, cover)
    height = -0.0007 * (1.0 - cover) * rings - 0.00035 * pores + 0.0003 * cover
    metal = np.zeros(c.shape, dtype=F32)
    colour, height = scarfs(c, colour, height)
    colour, rough, metal, height = rivets(c, colour, rough, metal, height, row=0.5 * HULL["lap"] + 0.004)
    colour, rough = weathering(c, colour, rough)
    return colour, rough, metal, height


def scarfs(c, colour, height):
    """The joints where the planks of one strake meet: a fine slanting line, two to a strake."""
    for k, frac in enumerate((0.31, 0.68)):
        at = (frac + 0.10 * (name_rand(c.g.chart, 20 + k) - 0.5)) * c.L
        d = np.abs((c.U - at) - 0.9 * (c.from_low - 0.5 * c.W))
        line = smoothstep(0.0045, 0.0015, d).astype(F32) * fade(1.0 / 0.006, c, across=False, texels=1.0)
        colour = mix(colour, colour * 0.30, 0.85 * line)
        height = height - 0.0012 * line
    return colour, height


def rivets(c, colour, rough, metal, height, row, pitch=0.150, radius=0.0085, rust=1.0):
    r, du, below = rivet_field(c, pitch, row, radius)
    head = smoothstep(radius, radius * 0.72, r).astype(F32)
    iron = mix(col(0.070, 0.052, 0.044), col(0.190, 0.085, 0.040), vnoise(c.U * 60.0, c.V * 60.0, c.seed + 5))
    weep = (smoothstep(0.012, 0.0, np.abs(du)) * smoothstep(0.0, 0.004, below) * smoothstep(0.075, 0.0, below)).astype(F32)
    weep = weep * (0.4 + 0.6 * vnoise(c.U * 3.0, np.zeros_like(c.U), c.seed + 9)) * rust
    colour = mix(colour, col(0.200, 0.095, 0.045), 0.45 * weep)
    colour = mix(colour, iron, head)
    rough = mix(rough, 0.62, head)
    metal = mix(metal, 0.85, head)
    dome = np.sqrt(np.clip(1.0 - (r / radius) ** 2, 0.0, 1.0)).astype(F32)
    height = height + 0.0028 * dome
    return colour, rough, metal, height


def paint_plank_paint(c):
    """The sheer strake: paint over the tar, chalked by the sun and chipped at the edges."""
    streak, rings, pores = wood_grain(c)
    wood = mix(col(0.20, 0.14, 0.09), col(0.36, 0.27, 0.18), 0.6 * streak + 0.4 * rings)
    paint = mix(col(0.028, 0.135, 0.150), col(0.055, 0.215, 0.215), fbm(c.U * 0.8, c.V * 5.0, 3, c.seed + 2))
    paint = mix(paint, col(0.20, 0.33, 0.32), 0.22 * fbm(c.U * 2.5 + 9.0, c.V * 14.0, 3, c.seed + 4))     # chalking
    edge = smoothstep(0.020, 0.0, np.minimum(c.from_low, c.from_high))
    chip = fbm(c.U * 11.0, c.V * band(60.0, c, texels=8.0), 4, c.seed + 6) + 0.22 * edge + 0.10 * smoothstep(0.5, 0.9, fbm(c.U * 0.7, c.V * 3.0, 2, c.seed + 8))
    gone = smoothstep(0.64, 0.71, chip).astype(F32)
    colour = mix(paint, wood * 0.9, gone)
    rough = mix(0.52 + 0.14 * streak, 0.82, gone)
    height = 0.00035 * (1.0 - gone) - 0.0005 * gone * rings + 0.00012 * vnoise(c.U * 4.0, c.V * 260.0, c.seed + 12)
    metal = np.zeros(c.shape, dtype=F32)
    colour, height = scarfs(c, colour, height)
    colour, rough, metal, height = rivets(c, colour, rough, metal, height, row=0.5 * HULL["lap"] + 0.004, rust=1.4)
    colour, rough = weathering(c, colour, rough, exposure=0.6)
    return colour, rough, metal, height


def paint_trim(c):
    """The rubbing strake: yellow ochre, rubbed through to the wood along its crown."""
    streak, rings, _ = wood_grain(c)
    wood = mix(col(0.11, 0.075, 0.05), col(0.24, 0.17, 0.11), 0.6 * streak + 0.4 * rings)
    paint = mix(col(0.420, 0.270, 0.060), col(0.560, 0.390, 0.105), fbm(c.U * 1.1, c.V * band(30.0, c, texels=6.0), 3, c.seed + 2))
    crown = smoothstep(0.012, 0.0, np.abs(c.V - 0.5 * c.W))
    gone = smoothstep(0.58, 0.70, fbm(c.U * 5.0, c.V * band(60.0, c, texels=6.0), 4, c.seed + 6) + 0.26 * crown).astype(F32)
    colour = mix(paint, wood, gone)
    rough = mix(0.58, 0.80, gone)
    colour, rough = weathering(c, colour, rough, exposure=0.5)
    return colour, rough, np.zeros(c.shape, dtype=F32), 0.0003 * (1.0 - gone) - 0.0004 * gone * rings


def paint_plank_inside(c):
    """The planking seen from inside: oiled, paler than the outside, dark where the bilge was."""
    streak, rings, pores = wood_grain(c)
    wood = mix(col(0.140, 0.088, 0.048), col(0.300, 0.200, 0.112), 0.5 * streak + 0.35 * rings + 0.15 * pores)
    wood = wood * (0.86 + 0.28 * c.tint)
    damp = smoothstep(0.34, 0.20, c.P[..., 1]).astype(F32)
    colour = mix(wood, wood * col(0.42, 0.40, 0.40), 0.8 * damp)
    colour = colour * (0.55 + 0.45 * c.ao[..., None] ** 1.5)
    rough = mix(0.72, 0.50, 0.6 * damp) + 0.08 * streak
    height = -0.0006 * rings - 0.0003 * pores
    metal = np.zeros(c.shape, dtype=F32)
    # The roves: a square iron washer under every rivet, along the lap at the plank's upper edge.
    pitch, row, half = 0.150, 0.5 * HULL["lap"], 0.011
    du = np.abs(np.mod(c.U + 0.5 * pitch, pitch) - 0.5 * pitch)
    dv = np.abs(c.from_low - row)
    sq = np.maximum(du, dv)
    rove = smoothstep(half, half * 0.8, sq).astype(F32)
    colour = mix(colour, col(0.085, 0.060, 0.048), rove)
    rough = mix(rough, 0.66, rove)
    metal = mix(metal, 0.8, rove)
    height = height + 0.0016 * rove + 0.0016 * smoothstep(0.005, 0.003, np.sqrt(du * du + dv * dv))
    return colour, rough, metal, height


def paint_timber(c, base=(0.185, 0.118, 0.066), light=(0.365, 0.255, 0.150), worn=0.0, tar=0.0):
    """Oak: the frames, beams, knees and the keel. `worn` polishes what hands and feet reach."""
    streak, rings, pores = wood_grain(c, across=0.8)
    colour = mix(col(*base), col(*light), 0.5 * streak + 0.3 * rings + 0.2 * pores) * (0.88 + 0.24 * c.tint)
    checks = smoothstep(0.80, 0.86, fbm(c.U * 2.2 + 3.0, c.V * band(150.0, c, texels=6.0), 3, c.seed + 15)).astype(F32)   # drying cracks
    colour = mix(colour, colour * 0.45, 0.7 * checks)
    rough = 0.74 - 0.10 * streak + 0.1 * checks
    height = -0.0007 * rings - 0.0012 * checks - 0.0003 * pores
    if tar > 0.0:
        cover = np.clip(tar - 0.5 * smoothstep(0.45, 0.8, fbm(c.U * 1.1, c.V * 9.0, 4, c.seed + 3)), 0.0, 1.0).astype(F32)
        colour = mix(colour, mix(col(0.028, 0.020, 0.015), col(0.058, 0.040, 0.027), streak), cover)
        rough = mix(rough, 0.44 + 0.2 * streak, cover)
    if worn > 0.0:
        up = np.clip(c.N[..., 1], 0.0, 1.0)
        rub = (worn * up * (0.45 + 0.55 * fbm(c.U * 2.4, c.V * 12.0, 3, c.seed + 19))).astype(F32)
        colour = mix(colour, mix(col(0.290, 0.195, 0.110), col(0.420, 0.300, 0.180), streak), 0.75 * rub)
        rough = mix(rough, 0.44, 0.8 * rub)
    colour = colour * (0.60 + 0.40 * c.ao[..., None] ** 1.3)
    return colour, rough, np.zeros(c.shape, dtype=F32), height


def paint_timber_tar(c):
    colour, rough, metal, height = paint_timber(c, tar=0.95)
    colour, rough = weathering(c, colour, rough)
    return colour, rough, metal, height


def paint_deck(c):
    """Pine deck boards: silvered by sun and salt, pale down the middle where feet go, dark at the
    sides where water lies, a nail at each beam."""
    streak, rings, pores = wood_grain(c, across=0.7)
    colour = mix(col(0.215, 0.165, 0.110), col(0.420, 0.345, 0.245), 0.45 * streak + 0.35 * rings + 0.2 * pores)
    colour = colour * (0.84 + 0.32 * c.tint)
    # Knots.
    kx, kz = c.U * 1.9 + c.ox, c.V * 6.5 + c.oy
    cell = np.sqrt((np.mod(kx, 1.0) - 0.5 - 0.3 * (_hash(np.floor(kx).astype(np.int64), np.floor(kz).astype(np.int64), c.seed) - 0.5)) ** 2
                   + ((np.mod(kz, 1.0) - 0.5) * 0.45) ** 2)
    has = _hash(np.floor(kx).astype(np.int64), np.floor(kz).astype(np.int64), c.seed + 1) > 0.80
    knot = (smoothstep(0.075, 0.03, cell) * has).astype(F32)
    colour = mix(colour, col(0.105, 0.065, 0.038), 0.85 * knot)
    x = np.abs(c.P[..., 0])
    walk = (smoothstep(0.75, 0.15, x) * (0.5 + 0.5 * fbm(c.P[..., 0] * 3.0, c.P[..., 2] * 1.4, 3, 77))).astype(F32)
    colour = mix(colour, colour * 1.22 + 0.02, 0.6 * walk)
    wet = (smoothstep(0.30, 0.0, c.ao) + 0.6 * smoothstep(0.55, 1.0, fbm(c.P[..., 0] * 1.3 + 4.0, c.P[..., 2] * 0.9, 3, 91)) * smoothstep(0.5, 1.05, x)).astype(F32)
    wet = np.clip(wet, 0.0, 1.0)
    colour = mix(colour, colour * col(0.50, 0.50, 0.52), 0.7 * wet)
    rough = mix(0.80 - 0.10 * walk, 0.50, 0.6 * wet)
    edge = smoothstep(0.004, 0.0, np.minimum(c.V, c.W - c.V)).astype(F32)
    colour = mix(colour, colour * 0.55, 0.6 * edge)
    height = -0.0008 * rings - 0.0004 * pores - 0.0010 * knot - 0.0012 * edge
    metal = np.zeros(c.shape, dtype=F32)
    # Nails where the board crosses a beam.
    stations = np.array(frame_stations_cache, dtype=F32) if frame_stations_cache else np.zeros(1, dtype=F32)
    dz = np.min(np.abs(c.P[..., 2][..., None] - stations[None, None, :]), axis=-1)
    dn = np.minimum(np.abs(c.V - 0.035), np.abs(c.W - c.V - 0.035))
    nail = smoothstep(0.0055, 0.004, np.sqrt(dz * dz + dn * dn)).astype(F32)
    colour = mix(colour, col(0.075, 0.055, 0.045), nail)
    stain = (smoothstep(0.03, 0.0, np.sqrt(dz * dz + dn * dn)) * 0.35).astype(F32)
    colour = mix(colour, colour * col(0.72, 0.55, 0.42), stain)
    metal = mix(metal, 0.8, nail)
    height = height - 0.0006 * nail
    colour = colour * (0.62 + 0.38 * c.ao[..., None] ** 1.2)
    return colour, rough, metal, height


frame_stations_cache = []


def paint_deck_wall(c):
    colour, rough, metal, height = paint_timber(c, base=(0.21, 0.16, 0.11), light=(0.42, 0.35, 0.26))
    # Three boards laid athwartships, one above the other.
    seam = smoothstep(0.004, 0.0, np.abs(np.mod(c.V, 0.118) - 0.059)).astype(F32)
    colour = mix(colour, colour * 0.35, seam)
    return colour, rough, metal, height - 0.0015 * seam


def paint_bilge(c):
    colour = np.broadcast_to(col(0.018, 0.016, 0.014), c.shape + (3,)).copy()
    return colour, np.full(c.shape, 0.6, dtype=F32), np.zeros(c.shape, dtype=F32), np.zeros(c.shape, dtype=F32)


def paint_spar(c, wet=False):
    """Pine spars: the mast, yard, oars and rudder, oiled gold and gone grey where they weather."""
    streak, rings, pores = wood_grain(c, across=0.6)
    colour = mix(col(0.235, 0.135, 0.055), col(0.480, 0.315, 0.140), 0.55 * streak + 0.30 * rings + 0.15 * pores)
    grey = fbm(c.U * 0.7 + 2.0, c.V * 6.0, 4, c.seed + 21)
    colour = mix(colour, col(0.30, 0.27, 0.23) * (0.8 + 0.4 * streak)[..., None], 0.35 * smoothstep(0.50, 0.85, grey))
    checks = smoothstep(0.83, 0.88, fbm(c.U * 1.4 + 3.0, c.V * band(170.0, c, texels=6.0), 3, c.seed + 15)).astype(F32)
    colour = mix(colour, colour * 0.40, 0.8 * checks)
    rough = 0.60 - 0.12 * streak + 0.15 * checks
    height = -0.0005 * rings - 0.0014 * checks
    if c.name.startswith("mast"):
        # Hands, the parrel and the halyard polish and darken the mast where they bear on it.
        y = c.P[..., 1]
        hand = (smoothstep(0.75, 1.15, y) * smoothstep(2.0, 1.5, y)).astype(F32)
        colour = mix(colour, colour * col(0.62, 0.52, 0.44), 0.55 * hand)
        rough = mix(rough, 0.38, 0.6 * hand)
        chafe = (smoothstep(RIG["yard_y"] - 0.45, RIG["yard_y"] - 0.15, y) * smoothstep(RIG["yard_y"] + 0.30, RIG["yard_y"] + 0.05, y)).astype(F32)
        colour = mix(colour, col(0.50, 0.40, 0.27), 0.5 * chafe)
        rough = mix(rough, 0.42, 0.5 * chafe)
        foot = smoothstep(RIG["deck_y"] + 0.5, RIG["deck_y"], y).astype(F32)
        colour = mix(colour, colour * 0.6, 0.6 * foot)
    colour = colour * (0.62 + 0.38 * c.ao[..., None] ** 1.2)
    if wet:
        colour, rough = weathering(c, colour, rough)
    return colour, rough, np.zeros(c.shape, dtype=F32), height


def paint_rope(c, tarred=False):
    """Three-strand rope: the lay runs diagonally round it."""
    circ = np.maximum(c.W, 1e-4)
    pitch = circ * 1.15
    phase = (c.U / pitch + c.V / circ) * 3.0
    strand = 0.5 + 0.5 * np.cos(2.0 * np.pi * phase)
    yarn = vnoise(c.U * band(900.0, c, across=False, texels=2.5), phase * 9.0, c.seed + 2)
    if tarred:
        colour = mix(col(0.026, 0.020, 0.016), col(0.085, 0.062, 0.044), 0.65 * strand + 0.2 * yarn)
        rough = 0.58 + 0.2 * (1.0 - strand)
    else:
        base = mix(col(0.250, 0.195, 0.120), col(0.560, 0.470, 0.320), 0.70 * strand + 0.30 * yarn)
        grime = fbm(c.U * 2.0 + c.ox, c.V * 40.0, 3, c.seed + 6)
        colour = mix(base, base * col(0.55, 0.52, 0.48), 0.5 * smoothstep(0.4, 0.8, grime))
        rough = np.full(c.shape, 0.88, dtype=F32)
    colour = colour * (0.55 + 0.45 * c.ao[..., None])
    return colour, rough.astype(F32), np.zeros(c.shape, dtype=F32), (0.0016 * strand * (circ / 0.06)).astype(F32)


def paint_iron(c):
    n = fbm(c.U * 30.0 + c.ox, c.V * 30.0 + c.oy, 4, c.seed)
    colour = mix(col(0.060, 0.050, 0.046), col(0.110, 0.090, 0.080), n)
    rust = smoothstep(0.50, 0.72, fbm(c.U * 12.0 + 4.0, c.V * 12.0, 4, c.seed + 5)).astype(F32)
    colour = mix(colour, mix(col(0.180, 0.075, 0.035), col(0.300, 0.140, 0.060), n), 0.8 * rust)
    rough = mix(0.50, 0.85, rust)
    metal = mix(0.9, 0.25, rust)
    return colour * (0.6 + 0.4 * c.ao[..., None]), rough, metal, 0.0006 * n + 0.0006 * rust


def paint_leather(c):
    n = fbm(c.U * 60.0 + c.ox, c.V * 60.0 + c.oy, 4, c.seed)
    colour = mix(col(0.055, 0.036, 0.026), col(0.150, 0.090, 0.055), n)
    return colour * (0.6 + 0.4 * c.ao[..., None]), np.full(c.shape, 0.6, dtype=F32) + 0.15 * n, np.zeros(c.shape, dtype=F32), 0.0008 * n


def paint_chest(c):
    colour, rough, metal, height = paint_timber(c, base=(0.150, 0.088, 0.050), light=(0.300, 0.190, 0.105), worn=0.9)
    seam = smoothstep(0.003, 0.0, np.abs(np.mod(c.V + 0.03, 0.115) - 0.0575)).astype(F32)
    colour = mix(colour, colour * 0.4, 0.8 * seam)
    return colour, rough, metal, height - 0.0012 * seam


def paint_cask(c):
    """Staves round the cask or bucket, running along it."""
    colour, rough, metal, height = paint_timber(c, base=(0.200, 0.130, 0.072), light=(0.400, 0.290, 0.170))
    if "_cap" in c.name:
        # A head: three boards across it, seen end-on along the cask.
        stave = smoothstep(0.004, 0.0, np.abs(np.mod(c.P[..., 1] + 0.02, 0.105) - 0.0525)).astype(F32)
    else:
        stave = smoothstep(0.004, 0.0, np.abs(np.mod(c.V, 0.062) - 0.031)).astype(F32)
    colour = mix(colour, colour * 0.35, 0.85 * stave)
    return colour, rough, metal, height - 0.0016 * stave


def paint_cloth(c, back=False):
    """Wool sailcloth sewn from vertical cloths, two of them dyed madder red, with tabling round
    the edges, a reef band, a patch, and the stains of a working life."""
    width = np.maximum(c.L, 1e-3)
    s = c.U / width                                   # 0..1 across the sail (port to starboard or back)
    if back:
        s = 1.0 - s
    t = c.V / np.maximum(c.W, 1e-3)                   # 0 at the head, 1 at the foot
    n_cloths = 7
    f = s * n_cloths
    k = np.clip(np.floor(f), 0, n_cloths - 1)
    within = f - k
    red = ((k == 1) | (k == n_cloths - 2)).astype(F32)
    shade = _hash(k.astype(np.int64), np.zeros_like(k, dtype=np.int64), 4242)
    cream = mix(col(0.600, 0.500, 0.330), col(0.730, 0.640, 0.460), shade)
    madder = mix(col(0.340, 0.070, 0.040), col(0.450, 0.105, 0.055), shade)
    colour = mix(cream, madder, red)
    # The weave, at the scale a few texels can carry: slubs along the warp and the weft.
    warp = vnoise(c.U * band(420.0, c, across=False, texels=2.5), c.V * 26.0, c.seed + 1)
    weft = vnoise(c.U * 26.0, c.V * band(420.0, c, texels=2.5), c.seed + 2)
    colour = colour * (0.88 + 0.12 * warp)[..., None] * (0.90 + 0.10 * weft)[..., None]
    # Weather: bleached toward the head, grey and damp toward the foot, runs of stain down the cloths.
    sun = fbm(c.U * 0.8 + 3.0, c.V * 0.6, 4, c.seed + 5)
    colour = mix(colour, colour * 1.12 + 0.03, 0.5 * smoothstep(0.6, 0.0, t) * sun)
    grime = smoothstep(0.45, 1.0, t) * (0.4 + 0.6 * fbm(c.U * 1.6, c.V * 0.9 + 7.0, 4, c.seed + 6))
    colour = mix(colour, colour * col(0.62, 0.62, 0.60), 0.55 * grime)
    runs = smoothstep(0.62, 0.86, fbm(c.U * 7.0 + 2.0, c.V * 0.55, 4, c.seed + 8)) * smoothstep(0.05, 0.4, t)
    colour = mix(colour, colour * col(0.74, 0.70, 0.62), 0.5 * runs)
    mildew = smoothstep(0.80, 0.9, fbm(c.U * 16.0, c.V * 16.0, 3, c.seed + 9)) * smoothstep(0.55, 0.95, t)
    colour = mix(colour, colour * 0.6, 0.35 * mildew)
    height = 0.0005 * warp + 0.0005 * weft
    # Seams between the cloths: a doubled, flat-felled strip with two rows of stitches.
    cloth_w = width / n_cloths
    d_seam = np.minimum(within, 1.0 - within) * cloth_w
    seam = smoothstep(0.026, 0.020, d_seam).astype(F32)
    colour = mix(colour, colour * 0.86, seam)
    stitch_row = smoothstep(0.004, 0.0015, np.abs(d_seam - 0.019)).astype(F32)
    stitch = stitch_row * (np.mod(c.V, 0.014) < 0.008)
    colour = mix(colour, col(0.20, 0.16, 0.11), 0.75 * stitch)
    height = height + 0.0012 * seam + 0.0006 * stitch
    # Tabling: the doubled hem all the way round, and the reef band across.
    d_edge = np.minimum(np.minimum(c.U, width - c.U), np.minimum(c.V, c.W - c.V))
    hem = smoothstep(0.075, 0.068, d_edge).astype(F32)
    reef = smoothstep(0.045, 0.038, np.abs(c.V - 0.72 * c.W)).astype(F32)
    doubled = np.maximum(hem, reef)
    colour = mix(colour, colour * 0.84, doubled)
    height = height + 0.0011 * doubled
    hem_stitch = smoothstep(0.004, 0.0015, np.abs(d_edge - 0.062)).astype(F32) * (np.mod(c.U + c.V, 0.014) < 0.008)
    colour = mix(colour, col(0.20, 0.16, 0.11), 0.7 * hem_stitch)
    # A patch: someone mended her low on the port side.
    pu, pv = (0.71 if not back else 0.29) * width, 0.58 * c.W
    inside = (np.abs(c.U - pu) < 0.21) & (np.abs(c.V - pv) < 0.16)
    patch_edge = np.minimum(0.21 - np.abs(c.U - pu), 0.16 - np.abs(c.V - pv))
    colour = np.where(inside[..., None], mix(col(0.500, 0.440, 0.320), col(0.600, 0.530, 0.400), warp), colour)
    ps = inside & (np.abs(patch_edge - 0.012) < 0.003) & (np.mod(c.U + c.V, 0.016) < 0.009)
    colour = mix(colour, col(0.18, 0.14, 0.10), 0.8 * ps.astype(F32))
    height = height + 0.0010 * inside.astype(F32)
    # Creases: the cloth draws toward each clew.
    for cu in (0.0, 1.0):
        dx, dy = (s - cu) * width, (1.0 - t) * c.W
        ang = np.arctan2(dy, np.abs(dx) + 1e-3)
        rad = np.sqrt(dx * dx + dy * dy)
        fold = np.cos(ang * 23.0 + 4.0 * vnoise(rad * 2.0, ang * 3.0, c.seed + 11)) * smoothstep(1.7, 0.2, rad)
        height = height + 0.0022 * fold
    colour = colour * (0.72 + 0.28 * c.ao[..., None])
    rough = np.full(c.shape, 0.90, dtype=F32) - 0.05 * doubled
    return colour, rough, np.zeros(c.shape, dtype=F32), height.astype(F32)


PAINTERS = {
    "plank_tar": paint_plank_tar,
    "plank_paint": paint_plank_paint,
    "plank_inside": paint_plank_inside,
    "trim_paint": paint_trim,
    "timber": paint_timber,
    "timber_worn": lambda c: paint_timber(c, worn=1.0),
    "timber_tar": paint_timber_tar,
    "deck": paint_deck,
    "deck_wall": paint_deck_wall,
    "bilge": paint_bilge,
    "spar": paint_spar,
    "spar_wet": lambda c: paint_spar(c, wet=True),
    "rope_hemp": paint_rope,
    "rope_tar": lambda c: paint_rope(c, tarred=True),
    "iron": paint_iron,
    "leather": paint_leather,
    "chest": paint_chest,
    "cask": paint_cask,
    "cloth": paint_cloth,
    "cloth_back": lambda c: paint_cloth(c, back=True),
}


def physics_from_hull(rig):
    """The rigid body Drift.olo authors, derived from the drawn hull.

    The collider is the hull as a box: her waterline beam and length, keel to gunwale, placed
    where the hull is. Jolt takes the body's centre of mass and inertia from it, so the centre of
    mass is the middle of the hull, a few centimetres from her waterline, and that box is what
    touches bottom when she grounds.

    BuoyancySystem floats a second box, centred on the ENTITY, on eight corner probes; each
    bottom probe carries a share of that box's volume in proportion to its depth over the
    submergence ramp. A box of half-extents (X, Y, Z) carrying this mass floats with its bottom
    corners d = mass * ramp / (4 * density * X * Y * Z) under water, so the entity's origin rides
    Y - d above the sea. Rolled a little, the bottom probes right her with weight * X^2 / d per
    radian, less weight * a for the height a of the centre of mass above those probes (the
    probes swing out from under it as she rolls). So:

      * X is the hull's half-beam (what a reader would measure on the model) and Z half her
        waterline: those are hers;
      * the sail's force is NOT applied at the drawn sail's centre, in height or in station.
        SailArea is a game number, twenty times the canvas that is drawn, and the height it
        acts at is the lever that lays her over. At the drawn sail's own centre that lever is
        2.82 m where the game was tuned at 2.0 m: she heeled 1.7 times as far, and with the box
        stiffened to cancel that at small angles she still spent 1.6 times as long past ten
        degrees in the storm (six runs each: 37% of a beam reach against 23%), because what
        sets the force that knocks her down is weight x half-beam / lever and nothing else.
        The lever is GAME_FEEL["heel_lever"] half-beams, which keeps that force;
      * Y is the one free number, and it is solved so she heels as far as the game was tuned
        for: that roll stiffness, divided by the lever, is GAME_FEEL["heel_stiffness"];
      * the entity's origin is therefore not her centre of mass and not her waterline. The hull
        mesh hangs Y - d below it, so the model's waterline is the sea, and the collider is
        offset to where that puts the hull;
      * fore and aft, BoatSystem resists leeway at the centre of mass, where a real hull under way
        resists it some way forward of that (her "lead"), so what turns the boat is the distance
        between the two. The force is put GAME_FEEL["weather_helm"] times this body's yaw inertia
        abaft the centre of mass, which gives a loaded sail the same turn into the wind the game
        was tuned with, and the lead that implies is written beside it. With the force at the
        drawn sail's own station a storm gust bore her away where it used to round her up.

    The damping and rudder coefficients are whatever keeps the rest of GAME_FEEL true for this
    box. The rudder's immersion is sampled at the foot of its blade: it bites until the blade is
    out of the water."""
    h = rig["hull"]
    m, rho, ramp = h["massKg"], h["waterDensity"], GAME_FEEL["submergence_ramp"]
    weight = m * 9.81
    coe = rig["sail"]["centreOfArea"]
    # The collider, and with it the centre of mass, from the waterline.
    cx, cz = 0.5 * h["beamWaterline"], 0.5 * h["lengthWaterline"]
    cy = 0.5 * (h["draft"] + h["freeboardAmidships"])
    com = 0.5 * (h["freeboardAmidships"] - h["draft"])
    # The buoyancy box. With d = c / Y, the probes c / Y + com below the centre of mass and the
    # sail's force `lever` above it:
    #     weight * (X^2 * Y / c - com - c / Y) = heel_stiffness * lever
    # which is a quadratic in Y.
    ex = round(0.5 * h["beam"], 3)
    ez = round(0.5 * h["lengthWaterline"], 3)
    c = m * ramp / (4.0 * rho * ex * ez)
    lever = GAME_FEEL["heel_lever"] * ex
    q = com + GAME_FEEL["heel_stiffness"] * lever / weight
    ey = round((q + math.sqrt(q * q + 4.0 * ex * ex)) * c / (2.0 * ex * ex), 3)
    d = c / ey
    rise = ey - d                                              # the entity's origin above the waterline
    wet = d / (2.0 * ramp)                                     # the wet fraction at rest: 4 of 8 probes, each d / ramp deep
    roll_inertia = m / 12.0 * ((2 * cx) ** 2 + (2 * cy) ** 2)  # Jolt's box, about the fore-and-aft axis
    yaw_inertia = m / 12.0 * ((2 * cx) ** 2 + (2 * cz) ** 2)
    yaw_drag = yaw_inertia / (GAME_FEEL["yaw_time_constant"] * m)
    helm_lever = GAME_FEEL["weather_helm"] * yaw_inertia        # the sail's force abaft the centre of mass
    return {
        "mass": m,
        "colliderHalfExtents": [round(cx, 2), round(cy, 2), round(cz, 2)],
        "colliderOffsetY": round(com - rise, 3),
        "probeExtents": [ex, ey, ez],
        "submergenceRamp": ramp,
        "restingImmersion": round(d, 4),
        "restingWetFraction": round(wet, 4),
        "originAboveWaterline": round(rise, 3),
        "centreOfMassAboveWaterline": round(com, 3),
        "hullMeshOffsetY": round(-rise, 3),
        "buoyancyLinearDrag": round(GAME_FEEL["surge_damping"] / wet, 3),
        "buoyancyAngularDrag": round(GAME_FEEL["roll_decay"] * roll_inertia / (wet * m), 2),
        "immersionDepth": round(GAME_FEEL["hull_grip"] * d, 2),
        "yawDrag": round(yaw_drag, 2),
        "maxRudderTorque": round(GAME_FEEL["turn_rate"] * yaw_drag * m, -1),
        "thrustOffsetY": round(rig["rudder"]["bladeBottomY"] - rise, 2),
        "thrustOffsetZ": rig["rudder"]["pivot"][2],
        "centreOfEffortY": round(com + lever - rise, 2),
        "centreOfEffortZ": round(-helm_lever, 2),
        "heelLever": round(lever, 3),
        "drawnSailAboveCentreOfMass": round(coe[1] - com, 3),
        "knockdownForce": round(weight * ex / lever, 0),
        "lateralResistanceLead": round(coe[2] + helm_lever, 3),
        "rollStiffness": round(weight * (ex * ex / d - com - d), 0),
    }


def to_srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1.0 / 2.4) - 0.055)


def paint_atlas(grids, atlas, ao, log=print):
    """Colour (sRGB), normal and occlusion-roughness-metallic images of an atlas, as uint8 arrays
    with the image's top row first."""
    size = atlas.size
    colour = np.zeros((size, size, 3), dtype=F32)
    orm = np.zeros((size, size, 3), dtype=F32)
    normal = np.zeros((size, size, 3), dtype=F32)
    normal[..., 2] = 1.0
    covered = np.zeros((size, size), dtype=bool)
    seen = set()
    for g in grids:
        if g.chart in seen:
            continue
        seen.add(g.chart)
        chart = atlas.charts[g.chart]
        c = Texel(g, chart, atlas.pad, ao)
        painter = PAINTERS.get(g.kind)
        assert painter is not None, f"no painter for kind {g.kind}"
        rgb, rough, metal, height = painter(c)
        x, y, w, h = c.rect
        height = np.broadcast_to(np.asarray(height, dtype=F32), (h, w))
        hu = np.gradient(height, axis=1) * c.su if w > 1 else np.zeros((h, w), dtype=F32)
        hv = np.gradient(height, axis=0) * c.sv if h > 1 else np.zeros((h, w), dtype=F32)
        # Tangent space: +U is the tangent, the bitangent runs UP the image (glTF's convention),
        # which is this chart's -V.
        nrm = unit(np.stack([-hu, hv, np.ones_like(hu)], axis=-1))
        colour[y:y + h, x:x + w] = np.broadcast_to(rgb, (h, w, 3))
        orm[y:y + h, x:x + w, 0] = c.ao
        orm[y:y + h, x:x + w, 1] = np.clip(np.broadcast_to(rough, (h, w)), 0.04, 1.0)
        orm[y:y + h, x:x + w, 2] = np.clip(np.broadcast_to(metal, (h, w)), 0.0, 1.0)
        normal[y:y + h, x:x + w] = nrm
        covered[y:y + h, x:x + w] = True
    # Empty texels take the mean of what is painted, so no mip level bleeds black across a chart.
    if np.any(covered) and not np.all(covered):
        colour[~covered] = colour[covered].mean(axis=0)
        orm[~covered] = orm[covered].mean(axis=0)
    log(f"[paint] {len(seen)} charts painted")
    return ((to_srgb(colour) * 255.0 + 0.5).astype(np.uint8),
            ((normal * 0.5 + 0.5) * 255.0 + 0.5).astype(np.uint8),
            (np.clip(orm, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8))


# ----------------------------------------------------------------------------
# Levels of detail: does the coarse boat still draw the same shape?
# ----------------------------------------------------------------------------


def silhouette(mb, axis, cell, lo, shape):
    """The mesh's shadow along `axis` on a grid of `cell` metres: every triangle is sampled on a
    lattice finer than a cell and each sample marks the cell it falls in."""
    verts, _, quads, _ = mb.arrays()
    tris = np.concatenate([quads[:, [0, 1, 2]], quads[:, [0, 2, 3]]], axis=0)
    keep = [k for k in range(3) if k != axis]
    a, b, c = (verts[tris[:, k]][:, keep] for k in range(3))
    edge = np.maximum(np.linalg.norm(b - a, axis=1), np.maximum(np.linalg.norm(c - a, axis=1), np.linalg.norm(c - b, axis=1)))
    steps = np.clip(np.ceil(edge / (0.5 * cell)).astype(int), 1, 512)
    mask = np.zeros(shape, dtype=bool)
    for n in np.unique(steps):
        sel = steps == n
        i, j = np.meshgrid(np.arange(n + 1), np.arange(n + 1), indexing="ij")
        ok = (i + j) <= n
        wi, wj = (i[ok] / n)[None, :, None], (j[ok] / n)[None, :, None]
        pts = a[sel][:, None, :] + (b[sel] - a[sel])[:, None, :] * wi + (c[sel] - a[sel])[:, None, :] * wj
        ij = np.floor((pts.reshape(-1, 2) - lo) / cell).astype(int)
        ij = ij[(ij[:, 0] >= 0) & (ij[:, 0] < shape[0]) & (ij[:, 1] >= 0) & (ij[:, 1] < shape[1])]
        mask[ij[:, 0], ij[:, 1]] = True
    return mask


def lod_silhouettes(lods, log=print, label=""):
    """Overlap of each coarser level's silhouette with level 0's, seen from the side, the end and
    above, on a grid of one pixel at the distance the level takes over. A level that loses the
    shape fails the build."""
    verts0 = lods[0].arrays()[0]
    out = []
    for k in range(1, len(lods)):
        cell = LOD_DISTANCES[k - 1] * 2.0 * math.tan(0.5 * VIEW_FOV_Y) / VIEW_HEIGHT_PX
        row = {"level": k, "fromMetres": LOD_DISTANCES[k - 1], "pixelMetres": round(cell, 4)}
        for name, axis in (("side", 0), ("end", 2), ("above", 1)):
            keep = [q for q in range(3) if q != axis]
            lo = verts0[:, keep].min(axis=0) - 2.0 * cell
            shape = tuple(np.ceil((verts0[:, keep].max(axis=0) + 2.0 * cell - lo) / cell).astype(int))
            m0 = silhouette(lods[0], axis, cell, lo, shape)
            mk = silhouette(lods[k], axis, cell, lo, shape)
            iou = float(np.count_nonzero(m0 & mk)) / max(float(np.count_nonzero(m0 | mk)), 1.0)
            row[name] = round(iou, 3)
            assert iou >= LOD_MIN_SILHOUETTE, f"{label} LOD {k} loses the silhouette from {name}: {iou:.3f}"
        out.append(row)
        log(f"[lod] {label} level {k} from {LOD_DISTANCES[k - 1]:.0f} m: silhouette overlap side {row['side']}, end {row['end']}, above {row['above']}")
    return out


# ----------------------------------------------------------------------------
# Files
# ----------------------------------------------------------------------------


def write_png(path, rgb):
    """An 8-bit RGB PNG, written here so a rebuild is byte-stable."""
    h, w = rgb.shape[:2]
    raw = np.concatenate([np.zeros((h, 1), dtype=np.uint8), rgb.reshape(h, w * 3)], axis=1).tobytes()

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n")
        fh.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        fh.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        fh.write(chunk(b"IEND", b""))


def write_gltf(path, name, mb, material, images, targets=()):
    """One mesh, one material, identity transform. `images` is (colour, normal, orm) file names;
    `targets` are morph targets from sail_morph_targets()."""
    verts, normals, quads, _ = mb.arrays()
    uv = np.concatenate([g.atlas_uv.reshape(-1, 2) for g in mb.grids])
    tris = np.concatenate([quads[:, [0, 1, 2]], quads[:, [0, 2, 3]]], axis=1).reshape(-1, 3)
    # Drop the triangles a pointed plank end or a cap's centre collapses to nothing.
    a, b, c = verts[tris[:, 0]], verts[tris[:, 1]], verts[tris[:, 2]]
    tris = tris[np.linalg.norm(np.cross(b - a, c - a), axis=1) > 1e-12]
    pos = verts.astype(F32)
    nrm = unit(normals).astype(F32)
    uv = uv.astype(F32)
    idx = tris.astype(np.uint32)
    blobs = [pos.tobytes(), nrm.tobytes(), uv.tobytes(), idx.tobytes()]
    offsets = np.concatenate([[0], np.cumsum([len(b_) for b_ in blobs])])
    bin_name = os.path.splitext(os.path.basename(path))[0] + ".bin"
    doc = {
        "asset": {"version": "2.0", "generator": "OloEngine build_drift_boat.py"},
        "scene": 0,
        "scenes": [{"name": name, "nodes": [0]}],
        "nodes": [{"name": name, "mesh": 0}],
        "meshes": [{"name": name, "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0}]}],
        "materials": [{
            "name": material,
            "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicRoughnessTexture": {"index": 2},
                                     "metallicFactor": 1.0, "roughnessFactor": 1.0},
            "normalTexture": {"index": 1},
            "occlusionTexture": {"index": 2},
            "doubleSided": False,
        }],
        "textures": [{"sampler": 0, "source": 0}, {"sampler": 0, "source": 1}, {"sampler": 0, "source": 2}],
        "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 33071, "wrapT": 33071}],
        "images": [{"uri": u, "mimeType": "image/png"} for u in images],
        "buffers": [{"uri": bin_name, "byteLength": int(offsets[-1])}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": int(offsets[0]), "byteLength": len(blobs[0]), "target": 34962},
            {"buffer": 0, "byteOffset": int(offsets[1]), "byteLength": len(blobs[1]), "target": 34962},
            {"buffer": 0, "byteOffset": int(offsets[2]), "byteLength": len(blobs[2]), "target": 34962},
            {"buffer": 0, "byteOffset": int(offsets[3]), "byteLength": len(blobs[3]), "target": 34963},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(pos), "type": "VEC3",
             "min": [float(v) for v in pos.min(axis=0)], "max": [float(v) for v in pos.max(axis=0)]},
            {"bufferView": 1, "componentType": 5126, "count": len(nrm), "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": len(uv), "type": "VEC2"},
            {"bufferView": 3, "componentType": 5125, "count": int(idx.size), "type": "SCALAR"},
        ],
    }
    if targets:
        end = int(offsets[-1])
        prim = doc["meshes"][0]["primitives"][0]
        prim["targets"] = []
        for target in targets:
            entry = {}
            for semantic, key in (("POSITION", "position"), ("NORMAL", "normal")):
                data = target[key].astype(F32)
                assert data.shape == pos.shape, target["name"]
                blob = data.tobytes()
                doc["bufferViews"].append({"buffer": 0, "byteOffset": end, "byteLength": len(blob), "target": 34962})
                accessor = {"bufferView": len(doc["bufferViews"]) - 1, "componentType": 5126, "count": len(data), "type": "VEC3"}
                if semantic == "POSITION":
                    accessor["min"] = [float(v) for v in data.min(axis=0)]
                    accessor["max"] = [float(v) for v in data.max(axis=0)]
                doc["accessors"].append(accessor)
                entry[semantic] = len(doc["accessors"]) - 1
                blobs.append(blob)
                end += len(blob)
            prim["targets"].append(entry)
        doc["meshes"][0]["weights"] = [0.0] * len(targets)
        doc["meshes"][0]["extras"] = {"targetNames": [t["name"] for t in targets]}
        doc["buffers"][0]["byteLength"] = end
    with open(os.path.join(os.path.dirname(path), bin_name), "wb") as fh:
        for b_ in blobs:
            fh.write(b_)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(doc, fh, indent=1)
        fh.write("\n")
    out = {"file": os.path.basename(path), "triangles": int(len(idx)), "vertices": int(len(pos)),
           "bounds_min": [round(float(v), 4) for v in pos.min(axis=0)], "bounds_max": [round(float(v), 4) for v in pos.max(axis=0)]}
    if targets:
        out["morphTargets"] = len(targets)
    return out


# ----------------------------------------------------------------------------
# Blender: the occlusion trace and the previews
# ----------------------------------------------------------------------------


def to_blender(v):
    v = np.asarray(v, dtype=F64)
    return np.stack([v[..., 0], -v[..., 2], v[..., 1]], axis=-1)


def bl_object(name, mb):
    """One mesh object from a MeshBuilder, with custom normals and the atlas UVs."""
    import bpy

    verts, normals, quads, owner = mb.arrays()
    me = bpy.data.meshes.new(name)
    me.vertices.add(len(verts))
    me.vertices.foreach_set("co", to_blender(verts).astype(F32).ravel())
    me.loops.add(quads.size)
    me.loops.foreach_set("vertex_index", quads.astype(np.int32).ravel())
    me.polygons.add(len(quads))
    me.polygons.foreach_set("loop_start", (np.arange(len(quads)) * 4).astype(np.int32))
    me.polygons.foreach_set("loop_total", np.full(len(quads), 4, dtype=np.int32))
    me.polygons.foreach_set("use_smooth", np.ones(len(quads), dtype=bool))
    me.update(calc_edges=True)
    if all(g.atlas_uv is not None for g in mb.grids):
        uv = np.concatenate([g.atlas_uv.reshape(-1, 2) for g in mb.grids])
        layer = me.uv_layers.new(name="atlas")
        loop_uv = uv[quads.ravel()]
        layer.data.foreach_set("uv", np.stack([loop_uv[:, 0], 1.0 - loop_uv[:, 1]], axis=1).astype(F32).ravel())
    me.normals_split_custom_set_from_vertices(to_blender(normals).astype(F32))
    ob = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(ob)
    bpy.context.view_layer.update()      # or the bake finds no object to select
    return ob, owner


def trace_occlusion(ob, size, pad, log=print):
    """Ambient occlusion of the object into its atlas (Cycles, CPU). Returns (size, size) floats,
    top row first."""
    import bpy

    scene = bpy.context.scene
    img = bpy.data.images.new("OcclusionTrace", size, size, alpha=False, float_buffer=True)
    img.colorspace_settings.name = "Non-Color"
    mat = bpy.data.materials.new("OcclusionTrace")
    if mat.node_tree is None:
        mat.use_nodes = True
    nt = mat.node_tree
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = img
    for node in nt.nodes:
        node.select = False
    tex.select = True
    nt.nodes.active = tex
    ob.data.materials.clear()
    ob.data.materials.append(mat)
    if scene.world is None:
        scene.world = bpy.data.worlds.new("World")
    scene.world.light_settings.distance = 1.6
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = AO_SAMPLES
    scene.cycles.use_denoising = False
    scene.cycles.seed = 880
    for o in bpy.context.view_layer.objects:
        o.select_set(o is ob)
    bpy.context.view_layer.objects.active = ob
    t0 = time.time()
    bpy.ops.object.bake(type="AO", margin=pad, use_clear=True)
    arr = np.empty(size * size * 4, dtype=F32)
    img.pixels.foreach_get(arr)
    ao = arr.reshape(size, size, 4)[::-1, :, 0].copy()
    bpy.data.images.remove(img)
    ob.data.materials.clear()
    bpy.data.materials.remove(mat)
    log(f"[occlusion] {size}^2 at {AO_SAMPLES} samples in {time.time() - t0:.0f} s")
    return ao


def bl_material(name, colour_png):
    import bpy

    mat = bpy.data.materials.new(name)
    if mat.node_tree is None:
        mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes.get("Principled BSDF")
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = bpy.data.images.load(colour_png)
    nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    bsdf.inputs["Roughness"].default_value = 0.7
    return mat


def render_previews(out_dir, log=print):
    """Workbench renders of whatever is in the scene from fixed cameras: a shape and texture
    check, not the look (the engine is the judge of that)."""
    import bpy
    from mathutils import Vector

    os.makedirs(out_dir, exist_ok=True)
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_WORKBENCH"
    sh = scene.display.shading
    sh.light = "STUDIO"
    sh.color_type = "TEXTURE"
    sh.show_cavity = True
    sh.cavity_type = "BOTH"
    sh.show_shadows = True
    sh.show_specular_highlight = True
    scene.display.render_aa = "16"
    scene.render.resolution_x = 1600
    scene.render.resolution_y = 1000
    scene.view_settings.view_transform = "Standard"
    if scene.world is None:
        scene.world = bpy.data.worlds.new("PreviewWorld")
    scene.world.color = (0.55, 0.62, 0.70)
    cam_data = bpy.data.cameras.new("PreviewCam")
    cam = bpy.data.objects.new("PreviewCam", cam_data)
    scene.collection.objects.link(cam)
    scene.camera = cam

    def shoot(name, eye, target, lens=50.0):
        cam_data.lens = lens
        e = Vector(tuple(to_blender(np.array(eye))))
        t = Vector(tuple(to_blender(np.array(target))))
        cam.location = e
        cam.rotation_euler = (t - e).to_track_quat("-Z", "Y").to_euler()
        scene.render.filepath = os.path.join(out_dir, f"{name}.png")
        bpy.ops.render.render(write_still=True)

    shoot("side", (14.0, 2.6, 0.0), (0.0, 2.2, 0.0), 40)
    shoot("bow34", (6.5, 3.0, 9.5), (0.0, 1.8, 0.0), 40)
    shoot("stern34", (5.5, 3.6, -9.5), (0.0, 1.8, 0.0), 40)
    shoot("follow", (0.0, 5.2, -11.5), (0.0, 1.6, 0.0), 30)
    shoot("top", (0.01, 13.0, 0.0), (0.0, 0.0, 0.0), 50)
    shoot("inside", (1.9, 2.5, -3.6), (-0.1, 0.5, 0.3), 26)
    shoot("deck_close", (0.9, 1.7, -1.9), (0.0, 0.4, 0.1), 30)
    shoot("bow_close", (2.4, 1.6, 4.9), (0.0, 0.9, 2.9), 50)
    shoot("quarter_close", (-3.2, 1.5, -4.6), (-0.6, 0.6, -2.3), 45)
    bpy.data.objects.remove(cam)
    log(f"[preview] wrote renders to {out_dir}")


# ----------------------------------------------------------------------------
# Entry point
# ----------------------------------------------------------------------------


def build_hull_part(hull, lod):
    mb = MeshBuilder()
    build_strakes(mb, hull, lod)
    info = {"backbone": build_backbone(mb, hull, lod)}
    build_gunwales(mb, hull, lod)
    build_tholes(mb, hull, lod)
    build_frames(mb, hull, lod)
    info["decks"] = build_decks(mb, hull, lod)
    info["beams"] = build_beams(mb, hull, lod)
    build_breasthooks(mb, hull, lod)
    info["mast"] = build_mast(mb, hull, lod)
    info["rigging"] = build_standing_rigging(mb, hull, info["mast"], info["backbone"], lod)
    info["gear"] = build_gear(mb, hull, lod)
    info["rudder_mount"] = build_rudder_mount(mb, hull, lod)
    return mb, info


def main():
    import bpy

    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    out_dir = os.path.abspath(argv[0]) if argv else os.path.dirname(os.path.abspath(__file__))
    preview_dir = os.path.abspath(argv[1]) if len(argv) > 1 else ""
    os.makedirs(out_dir, exist_ok=True)
    t0 = time.time()

    def log(msg):
        print(f"[{time.time() - t0:7.1f}s] {msg}", flush=True)

    bpy.ops.wm.read_factory_settings(use_empty=True)
    hull = HullForm()
    hydro = hull.float_at_mass()
    log("[hull] " + json.dumps({k: round(v, 4) for k, v in hydro.items()}))
    frame_stations_cache[:] = frame_stations(hull)

    files = {}
    preview_objects = []
    # --- The hull: every level of detail on LOD 0's atlas ---------------------
    hull_lods = [build_hull_part(hull, lod) for lod in range(3)]
    mb0, info = hull_lods[0]
    sail_lods = []
    for lod in range(3):
        mb = MeshBuilder()
        sail_lods.append((mb, build_sail(mb, lod)))
    mb_rudder = MeshBuilder()
    rudder_info = build_rudder(mb_rudder, info["rudder_mount"], 0)
    mb_rudder1 = MeshBuilder()
    build_rudder(mb_rudder1, info["rudder_mount"], 1)
    mb_line = MeshBuilder()
    build_line(mb_line)

    # Every coarser level is judged here, before anything is written: a level that loses the
    # shape stops the build with the files of the last good one still on disk and agreeing with
    # each other.
    silhouettes = {
        "hull": lod_silhouettes([mb for mb, _ in hull_lods], log, "hull"),
        "sail": lod_silhouettes([mb for mb, _ in sail_lods], log, "sail"),
        "rudder": lod_silhouettes([mb_rudder, mb_rudder1], log, "rudder"),
    }

    atlas = Atlas(ATLAS_SIZE)
    atlas.pack(mb0.grids, log)
    for mb, _ in hull_lods:
        atlas.assign(mb.grids)
    ob, _ = bl_object("DriftBoat", mb0)
    ao = trace_occlusion(ob, ATLAS_SIZE, ATLAS_PAD, log)
    colour, normal, orm = paint_atlas(mb0.grids, atlas, ao, log)
    hull_images = ("DriftBoatHullColor.png", "DriftBoatHullNormal.png", "DriftBoatHullORM.png")
    for fn, img in zip(hull_images, (colour, normal, orm)):
        write_png(os.path.join(out_dir, fn), img)
    files["hull"] = []
    for lod, (mb, _) in enumerate(hull_lods):
        fn = "DriftBoat.gltf" if lod == 0 else f"DriftBoat_LOD{lod}.gltf"
        files["hull"].append(write_gltf(os.path.join(out_dir, fn), "DriftBoat", mb, "DriftBoatHull", hull_images))
        log(f"[export] {fn}: {files['hull'][-1]['triangles']} triangles")
    ob.data.materials.append(bl_material("DriftBoat", os.path.join(out_dir, hull_images[0])))
    preview_objects.append(ob)

    # --- The rudder and the line share the hull's material and atlas? No: they move on their
    # own entities, so each is its own file; they are small and take a corner of the sail's atlas.
    sail_atlas = Atlas(SAIL_ATLAS_SIZE)
    sail_atlas.pack(sail_lods[0][0].grids + mb_rudder.grids + mb_line.grids, log)
    for mb, _ in sail_lods:
        sail_atlas.assign(mb.grids)
    sail_atlas.assign(mb_rudder.grids)
    sail_atlas.assign(mb_rudder1.grids)
    sail_atlas.assign(mb_line.grids)
    # One object for the trace: the sail where it flies, the rudder and the line set well apart
    # so none of them shadows another.
    trace = MeshBuilder()
    trace.grids = list(sail_lods[0][0].grids)
    for src, offset in ((mb_rudder, np.array([30.0, 0.0, 0.0])), (mb_line, np.array([-30.0, 0.0, 0.0]))):
        for g in src.grids:
            moved = Grid(g.name, g.pos + offset, g.kind, g.density, g.wrap_v, g.tint, g.chart)
            moved.nrm, moved.uv, moved.atlas_uv = g.nrm, g.uv, g.atlas_uv
            trace.grids.append(moved)
    ob_sail, _ = bl_object("DriftBoatSailTrace", trace)
    ao_s = trace_occlusion(ob_sail, SAIL_ATLAS_SIZE, ATLAS_PAD, log)
    bpy.data.objects.remove(ob_sail)
    colour, normal, orm = paint_atlas(sail_lods[0][0].grids + mb_rudder.grids + mb_line.grids, sail_atlas, ao_s, log)
    sail_images = ("DriftBoatRigColor.png", "DriftBoatRigNormal.png", "DriftBoatRigORM.png")
    for fn, img in zip(sail_images, (colour, normal, orm)):
        write_png(os.path.join(out_dir, fn), img)
    files["sail"] = []
    for lod, (mb, _) in enumerate(sail_lods):
        fn = "DriftBoatSail.gltf" if lod == 0 else f"DriftBoatSail_LOD{lod}.gltf"
        targets = sail_morph_targets(mb, lod)
        if lod == 0:
            furled = next(t for t in targets if t["name"] == "Furled")
        files["sail"].append(write_gltf(os.path.join(out_dir, fn), "DriftBoatSail", mb, "DriftBoatRig", sail_images, targets))
        log(f"[export] {fn}: {files['sail'][-1]['triangles']} triangles")
    files["rudder"] = [write_gltf(os.path.join(out_dir, "DriftBoatRudder.gltf"), "DriftBoatRudder", mb_rudder, "DriftBoatRig", sail_images),
                       write_gltf(os.path.join(out_dir, "DriftBoatRudder_LOD1.gltf"), "DriftBoatRudder", mb_rudder1, "DriftBoatRig", sail_images)]
    files["line"] = [write_gltf(os.path.join(out_dir, "DriftBoatLine.gltf"), "DriftBoatLine", mb_line, "DriftBoatRig", sail_images)]

    if preview_dir:
        mz = RIG["mast_z"]
        shifted = MeshBuilder()
        for g in sail_lods[0][0].grids:
            moved = Grid(g.name, g.pos + np.array([0.0, 0.0, mz]), g.kind, g.density, g.wrap_v, g.tint, g.chart)
            moved.nrm, moved.uv, moved.atlas_uv = g.nrm, g.uv, g.atlas_uv
            shifted.grids.append(moved)
        piv = np.array(info["rudder_mount"]["pivot"])
        cc, sc = math.cos(info["rudder_mount"]["cant"]), math.sin(info["rudder_mount"]["cant"])
        cant_m = np.array([[cc, -sc, 0.0], [sc, cc, 0.0], [0.0, 0.0, 1.0]])
        for g in mb_rudder.grids:
            moved = Grid(g.name, g.pos @ cant_m.T + piv, g.kind, g.density, g.wrap_v, g.tint, g.chart)
            moved.nrm, moved.uv, moved.atlas_uv = g.nrm, g.uv, g.atlas_uv
            shifted.grids.append(moved)
        ob2, _ = bl_object("DriftBoatSailPreview", shifted)
        ob2.data.materials.append(bl_material("DriftBoatSail", os.path.join(out_dir, sail_images[0])))
        render_previews(preview_dir, log)

    # --- The numbers the scene is built from -----------------------------------
    sail = sail_lods[0][1]
    mz = RIG["mast_z"]

    def lead(z, side=1.0):
        return [round(side * (float(hull.plan_e(z)) - 0.03), 3), round(float(hull.sheer_e(z)) + 0.01, 3), round(z, 3)]

    rig = {
        "units": "metres; +Y up, bow +Z, port +X; y = 0 is the waterline at the stated mass, z = 0 the centre of buoyancy",
        "hull": {
            "massKg": HULL["mass"],
            "waterDensity": HULL["water_density"],
            "displacedVolume": round(hydro["volume"], 4),
            "lengthOverall": round(files["hull"][0]["bounds_max"][2] - files["hull"][0]["bounds_min"][2], 3),
            "lengthHull": HULL["length"],
            "lengthWaterline": round(hydro["lwl"], 3),
            "waterlineAft": round(hydro["z_wl_aft"] + hull.z_shift, 3),
            "waterlineFore": round(hydro["z_wl_fore"] + hull.z_shift, 3),
            "beam": HULL["beam"],
            "beamWaterline": round(hydro["beam_wl"], 3),
            "draftToRabbet": round(hydro["waterline_above_rabbet"], 3),
            "draft": round(hydro["waterline_above_rabbet"] + HULL["keel_depth"], 3),
            "freeboardAmidships": round(HULL["depth"] - hydro["waterline_above_rabbet"], 3),
            "centreOfBuoyancyY": round(hydro["vcb"] + hull.y_shift, 3),
            "waterplaneArea": round(hydro["waterplane_area"], 3),
            "waterplaneInertia": round(hydro["waterplane_inertia"], 3),
            "sternZ": round(hull.stern_e, 3),
            "bowZ": round(hull.bow_e, 3),
            "stemHeadBow": [round(v, 3) for v in info["backbone"]["stem_head_bow"]],
            "stemHeadStern": [round(v, 3) for v in info["backbone"]["stem_head_stern"]],
        },
        "deck": {"mainY": RIG["deck_y"], "aftY": RIG["aft_deck_y"], "foreY": RIG["fore_deck_y"]},
        "mast": {"x": 0.0, "z": mz, "truckY": RIG["mast_truck_y"], "houndsY": round(info["mast"]["hounds_y"], 3)},
        "sail": {
            "origin": [0.0, 0.0, mz],
            "area": round(sail["area"], 3),
            "projectedArea": round(sail["projected_area"], 3),
            "centreOfArea": [round(sail["centre_of_area"][0], 3), round(sail["centre_of_area"][1], 3), round(sail["centre_of_area"][2] + mz, 3)],
            "yardY": RIG["yard_y"],
            "yardLength": RIG["yard_length"],
            "yardArmPort": [round(v, 3) for v in sail["yard_arm_port"]],
            "yardArmStarboard": [round(v, 3) for v in sail["yard_arm_starboard"]],
            "clewPort": [round(v, 3) for v in sail["clew_port"]],
            "clewStarboard": [round(v, 3) for v in sail["clew_starboard"]],
            # The shapes the cloth blends between, in the order the files hold them (the engine
            # names an imported target by its index, MorphTarget_0 on), and where the clews
            # are with the sail gathered right up: the sheets follow them there.
            "morphTargets": [name for name, _ in SAIL_MORPHS],
            "clewPortFurled": [round(v, 3) for v in furled["clew_port"]],
            "clewStarboardFurled": [round(v, 3) for v in furled["clew_starboard"]],
        },
        "rudder": {
            "pivot": [round(v, 3) for v in info["rudder_mount"]["pivot"]],
            "cantDeg": round(math.degrees(info["rudder_mount"]["cant"]), 2),
            "maxAngleDeg": RIG["rudder_max_deg"],
            "bladeBottomY": round(rudder_info["blade_bottom_world_y"], 3),
        },
        # Where the running rigging leads on the hull. Its other ends are the sail's clews and
        # yard arms above (in the sail file's own frame, which the yard angle turns).
        "lines": {
            "sheetLeadPort": lead(mz - 1.85),
            "sheetLeadStarboard": lead(mz - 1.85, -1.0),
            "braceLeadPort": lead(mz - 2.45),
            "braceLeadStarboard": lead(mz - 2.45, -1.0),
        },
        "files": files,
        # A level's silhouette, as its overlap with level 0's at the distance it takes over.
        "levelsOfDetail": {
            "distances": list(LOD_DISTANCES),
            **silhouettes,
        },
        "textures": {"hull": {"size": ATLAS_SIZE, "texelsPerMetre": round(atlas.ppm, 1)},
                     "sail": {"size": SAIL_ATLAS_SIZE, "texelsPerMetre": round(sail_atlas.ppm, 1)}},
    }
    rig["physics"] = physics_from_hull(rig)
    with open(os.path.join(out_dir, "DriftBoat.rig.json"), "w", encoding="utf-8", newline="\n") as fh:
        json.dump(rig, fh, indent=2)
        fh.write("\n")
    log("[rig] " + json.dumps({"sailArea": rig["sail"]["area"], "lengthOverall": rig["hull"]["lengthOverall"],
                               "freeboard": rig["hull"]["freeboardAmidships"], "physics": rig["physics"]}))
    log("done")


if __name__ == "__main__":
    main()
