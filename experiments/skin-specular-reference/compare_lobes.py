#!/usr/bin/env python3
"""Reference comparison for issue #1243: how many specular lobes does skin need,
and what does filtering the pore detail cost?

This is the experiment behind the issue's scope note ("Begin with a measured
lobe/detail comparison"). It answers three questions, in order, and every one of
them is answered against the SAME ground truth, which is the point: the lobe
question and the pore-filtering question are the same question asked at two
scales, and answering them with two unrelated references is how an engine ends
up with a second lobe that is really just unfiltered variance.

  1. GROUND TRUTH. An explicit micro-geometry integration. A procedural patch of
     skin -- furrows, pores and fine micro-roughness -- is tessellated far below
     the pixel, and the aggregate specular response of a PIXEL FOOTPRINT is
     computed by summing the microfacet BRDF over the texels inside it, with each
     texel's own normal. Nothing analytic is assumed about the aggregate: it is a
     sum over a surface, so a lobe mixture is a RESULT here and never an input.

  2. HOW MANY LOBES. The aggregate is fitted with (a) one GGX lobe at the mean
     normal, (b) one GGX lobe whose roughness is widened by the footprint's own
     normal variance (Toksvig), and (c) a convex mixture of two GGX lobes. Each
     is scored by relative RMS over a view/light sweep that includes grazing,
     and by directional albedo, because a fit that matches the shape and loses
     the energy is not a fit.

  3. WHAT SPARKLES. The camera is pushed toward the patch so the footprint
     shrinks continuously, and the frame-to-frame variation of each model's
     response is measured. This is the number the third acceptance criterion is
     about, and it is not visible in question 2: a model can fit every static
     footprint well and still flicker, because fitting is per-footprint and
     sparkle is ACROSS footprints.

Run:  python compare_lobes.py --quick    (about 10 s, and the one to run)
      python compare_lobes.py            (about 20 min: a 512x512 patch, a 6x24
                                          direction sweep and a 30-point sigma
                                          refit at every footprint size)

--quick is the default recommendation because it reaches every conclusion the
full run does, at coarser resolution: the numbers move but none of the orderings
do. The full run is what the guide's tables record.

Results are recorded in docs/guides/skin-layered-specular.md. Nothing in the
engine imports this file; it is the evidence, not the implementation.

LIMITS, STATED UP FRONT. The aggregate in (1) sums the microfacet BRDF per
texel and does not ray-trace inter-texel shadowing or interreflection between
furrow walls. That is the same approximation every normal-map filtering result
in the literature makes (Toksvig 2005, LEAN 2010, Kaplanyan 2016), and it errs
in a known direction: it OVERSTATES the response of texels facing away from the
light at grazing angles, so the grazing residuals below are an upper bound on
the error, not a measurement of it.
"""

from __future__ import annotations

import argparse
import math

import numpy as np

# Dielectric F0 for skin's surface (lipid film, n ~ 1.45). The engine's
# DEFAULT_DIELECTRIC_F0 is 0.04 (n = 1.5); using the engine's number keeps this
# comparable to what the shader actually evaluates.
F0 = 0.04

# The patch is a square of skin, in MILLIMETRES. 4 mm across is a few furrow
# cells: big enough that a footprint at the coarsest level averages real
# mesostructure and not one pore.
PATCH_MM = 4.0


# ---------------------------------------------------------------------------
# The micro-geometry
# ---------------------------------------------------------------------------


def _value_noise(rng: np.random.Generator, n: int, cells: int) -> np.ndarray:
    """Smooth periodic value noise on an n x n grid with `cells` cells per side.

    Deliberately not Perlin: what matters here is a band-limited height field
    with a controllable feature size, and a cosine-interpolated value lattice is
    the shortest thing that gives one without importing anything.
    """
    lattice = rng.random((cells, cells))
    # Wrap so the patch tiles -- a non-tiling patch puts a discontinuity in the
    # normals at the edge, which would show up as a fake high-variance footprint.
    lattice = np.pad(lattice, ((0, 1), (0, 1)), mode="wrap")

    t = np.linspace(0.0, cells, n, endpoint=False)
    i = np.floor(t).astype(np.int64)
    f = t - i
    # Smoothstep, so the derivative is continuous and the normals are not faceted.
    f = f * f * (3.0 - 2.0 * f)

    a = lattice[np.ix_(i, i)]
    b = lattice[np.ix_(i + 1, i)]
    c = lattice[np.ix_(i, i + 1)]
    d = lattice[np.ix_(i + 1, i + 1)]

    fx = f[:, None]
    fy = f[None, :]
    return (a * (1 - fx) * (1 - fy) + b * fx * (1 - fy) + c * (1 - fx) * fy + d * fx * fy)


def _rms_slope(field: np.ndarray, texel_mm: float) -> float:
    dzdx = (np.roll(field, -1, axis=0) - np.roll(field, 1, axis=0)) / (2.0 * texel_mm)
    dzdy = (np.roll(field, -1, axis=1) - np.roll(field, 1, axis=1)) / (2.0 * texel_mm)
    return float(np.sqrt(np.mean(dzdx**2 + dzdy**2)))


# TARGET RMS SLOPES, which is what the bands are calibrated to rather than to an
# amplitude in millimetres, and the difference matters. A depth is only half a
# shape: 30 um of relief over a 700 um furrow and 30 um over a 100 um pore are
# the same measurement and a factor of seven apart in everything this experiment
# asks about. Skin-replica profilometry reports Sq around 10-30 um at
# correlation lengths of 100-300 um on a cheek, which is an RMS slope in the low
# tenths; pore walls are the steep minority band and sit several times that.
# Calibrating to slope also makes the result independent of `n`, which an
# amplitude does not: halving the texel size would otherwise halve the measured
# variance and quietly change the answer.
FURROW_RMS_SLOPE = 0.22
PORE_RMS_SLOPE = 0.55


def build_skin_patch(n: int, seed: int = 1243) -> tuple[np.ndarray, float]:
    """A height field for `n` x `n` texels of skin, in millimetres.

    Three bands, because skin has three and they filter differently:

      FURROWS    ~0.7 mm cells, the visible criss-cross mesostructure, as narrow
                 CREASES rather than smooth swells -- a furrow is a fold, and a
                 sinusoid of the same depth has a third of the slope and
                 therefore a third of the variance to filter.
      PORES      ~0.15 mm pits. Vanishes into variance within a metre or two of
                 camera distance -- this is the band the filtering question is
                 about, and the steep one.
      MICRO      below the texel. NOT in the height field: it is carried as the
                 per-texel base roughness instead, because putting it in the
                 field would just alias at whatever resolution we picked and
                 make the answer a function of `n`.

    Returns (height in mm, texel size in mm).
    """
    rng = np.random.default_rng(seed)
    texel_mm = PATCH_MM / n

    furrow_cells = max(2, int(round(PATCH_MM / 0.7)))
    pore_cells = max(4, int(round(PATCH_MM / 0.15)))

    # Ridged noise: |2v - 1| folds the lattice into creases, and the power
    # narrows them. A furrow has a sharp bottom and flat skin either side, which
    # a value-noise swell does not.
    v = _value_noise(rng, n, furrow_cells)
    furrows = -np.power(1.0 - np.abs(2.0 * v - 1.0), 3.0)

    p = _value_noise(rng, n, pore_cells)
    # Pores are PITS, not bumps, and they are a MINORITY of the area: threshold
    # so roughly a third of the patch is inside a pore and the rest is flat.
    pores = -np.power(np.clip((p - 0.45) / 0.55, 0.0, 1.0), 2.0)

    # Calibrate each band to its target RMS slope independently, then sum. Summed
    # bands do not have the sum of the slopes, but they are near-orthogonal in
    # frequency here, so the total lands close to the quadrature sum -- which is
    # reported, not assumed.
    furrows = furrows * (FURROW_RMS_SLOPE / max(_rms_slope(furrows, texel_mm), 1e-9))
    pores = pores * (PORE_RMS_SLOPE / max(_rms_slope(pores, texel_mm), 1e-9))

    return furrows + pores, texel_mm


# THE SEBUM BAND -- the piece that decides whether a second lobe is a real lobe.
#
# Every texel above shares one micro-roughness, and a patch built that way can
# only ever produce ONE lobe plus variance: the aggregate of many copies of the
# same lobe at scattered normals is a wider copy of that lobe, which is the whole
# of what Toksvig says. Under that surface a second lobe has nothing to do, and
# an experiment that stopped there would "measure" the answer it had assumed.
#
# Real skin is not like that. The lipid film is PATCHY -- an oily T-zone plateau
# next to a dry cheek, at a scale of a fraction of a millimetre -- so a single
# footprint routinely straddles a smooth region and a rough one, and its
# aggregate is a mixture of two DIFFERENT lobe widths rather than a spread of one.
# That is the physical claim "layered specular" makes, and this band is what
# puts it in the ground truth so it can be tested instead of assumed.
#
# The ratio is the authored contrast between the two extremes. Weyrich et al.
# (2006) fit spatially varying specular roughness across scanned faces and report
# a spread of roughly this order between the oiliest and driest regions of one
# face; the number here is the order of magnitude, not their measurement.
SEBUM_ROUGHNESS_RATIO = 3.0
SEBUM_CELL_MM = 0.5


def build_sebum_roughness(n: int, alpha_base: float, seed: int = 61243) -> np.ndarray:
    """Per-texel micro-roughness alpha: a patchy lipid film over the patch.

    Geometric about `alpha_base`, so the band changes the DISTRIBUTION of
    roughness without moving its geometric mean -- a band that also shifted the
    mean would confound "is there a second lobe" with "is the roughness right".
    """
    rng = np.random.default_rng(seed)
    cells = max(2, int(round(PATCH_MM / SEBUM_CELL_MM)))
    m = _value_noise(rng, n, cells)
    # Sharpen toward two plateaus: sebum coverage reads as oily PATCHES with
    # transitions, not as a smooth gradient across the whole face.
    m = np.clip((m - 0.5) * 2.5 + 0.5, 0.0, 1.0)
    m = m * m * (3.0 - 2.0 * m)
    return alpha_base * np.power(SEBUM_ROUGHNESS_RATIO, m - 0.5)


def normals_from_height(height: np.ndarray, texel_mm: float) -> np.ndarray:
    """Unit normals of the height field, shape (n, n, 3), tangent space +Z up."""
    # Central differences with wrap, matching the tiling the patch was built for.
    dzdx = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) / (2.0 * texel_mm)
    dzdy = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) / (2.0 * texel_mm)
    n = np.stack([-dzdx, -dzdy, np.ones_like(height)], axis=-1)
    return n / np.linalg.norm(n, axis=-1, keepdims=True)


# ---------------------------------------------------------------------------
# The microfacet BRDF -- the same algebra include/PBRCommon.glsl evaluates
# ---------------------------------------------------------------------------


def ggx_d(n_dot_h: np.ndarray, alpha: np.ndarray | float) -> np.ndarray:
    a2 = np.asarray(alpha) ** 2
    d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0
    return a2 / np.maximum(math.pi * d * d, 1e-12)


def smith_vis(n_dot_v: np.ndarray, n_dot_l: np.ndarray, alpha: np.ndarray | float) -> np.ndarray:
    """Height-correlated Smith visibility: G2 / (4 NdotV NdotL), Cook-Torrance
    denominator already folded in -- exactly visibilitySmithGGXCorrelated."""
    a2 = np.asarray(alpha) ** 2
    gv = n_dot_l * np.sqrt(n_dot_v * n_dot_v * (1.0 - a2) + a2)
    gl = n_dot_v * np.sqrt(n_dot_l * n_dot_l * (1.0 - a2) + a2)
    return 0.5 / np.maximum(gv + gl, 1e-9)


def fresnel(v_dot_h: np.ndarray) -> np.ndarray:
    return F0 + (1.0 - F0) * np.power(np.clip(1.0 - v_dot_h, 0.0, 1.0), 5.0)


def specular_brdf(normals: np.ndarray, v: np.ndarray, l: np.ndarray,
                  alpha: np.ndarray | float) -> np.ndarray:
    """Specular BRDF times NdotL, for normals of shape (..., 3) and one (v, l).

    NdotL is folded in because that is the quantity a pixel integrates, and
    because dropping it is how a grazing comparison quietly becomes a
    comparison of two things that never reach the screen.
    """
    h = v + l
    h = h / np.maximum(np.linalg.norm(h), 1e-12)

    n_dot_v = np.clip(normals @ v, 1e-4, 1.0)
    n_dot_l = np.clip(normals @ l, 0.0, 1.0)
    n_dot_h = np.clip(normals @ h, 0.0, 1.0)
    v_dot_h = np.clip(float(np.dot(v, h)), 0.0, 1.0)

    return ggx_d(n_dot_h, alpha) * smith_vis(n_dot_v, n_dot_l, alpha) * fresnel(np.asarray(v_dot_h)) * n_dot_l


# ---------------------------------------------------------------------------
# Footprints
# ---------------------------------------------------------------------------


def footprint_mean_normals(normals: np.ndarray, k: int) -> np.ndarray:
    """Average the unit normals over every non-overlapping k x k block.

    Returns the UNNORMALIZED mean, shape (n/k, n/k, 3). Unnormalized on purpose:
    its LENGTH is the whole of the filtering signal. |N| == 1 means every texel
    in the footprint agreed; |N| < 1 is exactly the variance that a point sample
    throws away and then renders as sparkle.
    """
    n = normals.shape[0]
    m = n // k
    blocks = normals[: m * k, : m * k].reshape(m, k, m, k, 3)
    return blocks.mean(axis=(1, 3))


# WHY THERE IS A SECOND ESTIMATOR, AND WHY IT IS THE ONE THAT SHIPS.
#
# Toksvig reads the variance off the LENGTH of the averaged normal. That signal
# only exists if the averaged normal is allowed to be short -- and in this engine
# it never is. include/PBRCommon.glsl's decodeTangentNormal reconstructs
# z = sqrt(1 - x^2 - y^2) from the sampled xy instead of sampling the blue
# channel, which it must do because two-channel BC5 normal maps have no blue
# channel (#440). Reconstruction renormalizes: whatever the mip chain averaged,
# what comes out is unit length. |N| is 1.0 at every mip, so the Toksvig factor
# is 1.0 at every mip, and the filter is silently a no-op.
#
# That is worth measuring rather than asserting, so Toksvig stays in the table
# as the idealised estimator -- what variance filtering buys if you could read
# the variance perfectly. The KAPLANYAN arm is the one the engine can actually
# compute: it estimates the same variance from the SCREEN-SPACE derivatives of
# the final shading normal, which cost two ALU instructions, exist on every path
# that has a normal, and respond to geometric curvature and to LOD as well as to
# the normal map. The gap between the two rows is the price of the estimator.
#
# Tokuyoshi & Kaplanyan, "Improved Geometric Specular Antialiasing" (2019):
#     variance = SIGMA2 * (|dN/dx|^2 + |dN/dy|^2)
#     kernel   = min(2 * variance, KAPPA)
#     alpha'^2 = alpha^2 + kernel
# The variances ADD, which is the whole reason this composes with a roughness
# map instead of fighting it.
SCREEN_SPACE_SIGMA2 = 0.5
SCREEN_SPACE_KAPPA = 0.18


def kaplanyan_alpha(mean_unit_grid: np.ndarray, alpha: np.ndarray,
                    sigma2: float = SCREEN_SPACE_SIGMA2,
                    kappa: float = SCREEN_SPACE_KAPPA) -> np.ndarray:
    """Alpha widened by the screen-space variance of the shading normal.

    `mean_unit_grid` is (m, m, 3): one unit normal per PIXEL, laid out as the
    pixels are on screen, so a difference between neighbours IS dFdx/dFdy.
    """
    dndx = np.diff(mean_unit_grid, axis=0, append=mean_unit_grid[-1:, :, :])
    dndy = np.diff(mean_unit_grid, axis=1, append=mean_unit_grid[:, -1:, :])
    variance = sigma2 * ((dndx * dndx).sum(-1) + (dndy * dndy).sum(-1))
    kernel = np.minimum(2.0 * variance, kappa)
    a = np.asarray(alpha, dtype=float).reshape(mean_unit_grid.shape[:2])
    return np.clip(np.sqrt(a * a + kernel), 1e-4, 1.0)


def toksvig_alpha(mean_normal_len: np.ndarray, alpha_base: float) -> np.ndarray:
    """Toksvig's widened alpha from the mean normal's length.

    Toksvig (2005) derives, for a Blinn-Phong exponent s, an effective
    s' = s * |N| / (|N| + s * (1 - |N|)). Written in GGX alpha with the usual
    s = 2/alpha^2 - 2 correspondence, and solved back for alpha'. The identity
    is what the engine's SkinNormalVarianceAlpha mirrors, and the C++ test pins
    the two against each other.
    """
    ln = np.clip(mean_normal_len, 1e-4, 1.0)
    a = np.maximum(np.asarray(alpha_base, dtype=float), 1e-4)
    s = np.maximum(2.0 / (a * a) - 2.0, 1e-4)
    s_prime = np.maximum(s * ln / (ln + s * (1.0 - ln)), 1e-4)
    return np.sqrt(2.0 / (s_prime + 2.0))


# ---------------------------------------------------------------------------
# The direction sweep
# ---------------------------------------------------------------------------


def direction_sweep(quick: bool) -> tuple[np.ndarray, np.ndarray]:
    """View and light directions. Elevations chosen to include grazing on both
    sides, because that is where a single lobe is worst and where the issue's
    acceptance criterion looks."""
    v_elev = np.array([70.0, 45.0, 20.0, 8.0])           # degrees above the surface
    if quick:
        l_elev = np.array([70.0, 40.0, 15.0])
        l_azim = np.linspace(0.0, 360.0, 9, endpoint=False)
    else:
        l_elev = np.array([80.0, 60.0, 40.0, 25.0, 12.0, 5.0])
        l_azim = np.linspace(0.0, 360.0, 24, endpoint=False)

    def to_dir(elev_deg, azim_deg):
        e = np.radians(elev_deg)
        a = np.radians(azim_deg)
        return np.array([math.cos(e) * math.cos(a), math.cos(e) * math.sin(a), math.sin(e)])

    views = np.array([to_dir(e, 0.0) for e in v_elev])
    lights = np.array([to_dir(e, a) for e in l_elev for a in l_azim])
    return views, lights


# ---------------------------------------------------------------------------
# 1 + 2: ground truth and the fits
# ---------------------------------------------------------------------------


def aggregate_and_models(normals: np.ndarray, k: int, alpha_map: np.ndarray,
                         views: np.ndarray, lights: np.ndarray):
    """For every footprint at size k, the ground-truth aggregate response and the
    inputs every candidate model needs.

    Returns (truth, mean_unit, mean_len) where truth has shape
    (n_footprints, n_views, n_lights).
    """
    n = normals.shape[0]
    m = n // k
    blocks = normals[: m * k, : m * k].reshape(m, k, m, k, 3)
    # (n_footprints, k*k, 3) -- every texel of every footprint, flat.
    texels = blocks.transpose(0, 2, 1, 3, 4).reshape(m * m, k * k, 3)

    # The per-texel roughness, blocked exactly like the normals so texel i of
    # footprint f is the same texel in both.
    a_blocks = alpha_map[: m * k, : m * k].reshape(m, k, m, k)
    a_texels = a_blocks.transpose(0, 2, 1, 3).reshape(m * m, k * k)

    mean_vec = texels.mean(axis=1)
    mean_len = np.linalg.norm(mean_vec, axis=-1)
    mean_unit = mean_vec / np.maximum(mean_len, 1e-12)[:, None]

    # The footprint's own mean roughness -- what a mip-filtered roughness map
    # hands a shader, and therefore the alpha every single-lobe model below gets.
    #
    # AVERAGED IN ROUGHNESS AND THEN SQUARED, NOT AVERAGED IN ALPHA, and the
    # first version of this script got that backwards. The texture stores
    # PERCEPTUAL ROUGHNESS -- OLO_MAT_METALLIC_ROUGHNESS samples it straight out
    # of the map's green channel and every closure squares it later -- so what
    # the hardware's mip chain averages linearly is the roughness. Averaging
    # `a_texels` (which are alphas) computes mean(r^2); the hardware computes
    # mean(r)^2. By Jensen those differ in a KNOWN direction, mean(r^2) >=
    # mean(r)^2, so the old code modelled a mip chain that was systematically
    # ROUGHER than the real one -- i.e. it flattered the `mip` row of the table,
    # which is the row this experiment uses to argue that a mip is not a filter.
    #
    # Reported by CodeRabbit on PR #1316. The comment above it already said "a
    # roughness map is averaged linearly by the hardware", which is exactly the
    # thing the line below was not doing.
    mean_roughness = np.sqrt(a_texels).mean(axis=1)
    mean_alpha = mean_roughness * mean_roughness

    truth = np.empty((m * m, len(views), len(lights)))
    for vi, v in enumerate(views):
        for li, l in enumerate(lights):
            # The aggregate IS the mean over the footprint's texels. This is the
            # only place ground truth is produced, and it assumes nothing.
            truth[:, vi, li] = specular_brdf(texels, v, l, a_texels).mean(axis=1)
    return truth, mean_unit, mean_len, mean_alpha


def model_response(mean_unit: np.ndarray, alpha: np.ndarray, views: np.ndarray,
                   lights: np.ndarray) -> np.ndarray:
    """One GGX lobe at the footprint's mean normal with a per-footprint alpha."""
    out = np.empty((mean_unit.shape[0], len(views), len(lights)))
    a = np.asarray(alpha)
    if a.ndim == 0:
        a = np.full(mean_unit.shape[0], float(a))
    for vi, v in enumerate(views):
        for li, l in enumerate(lights):
            out[:, vi, li] = specular_brdf(mean_unit, v, l, a)
    return out


# WHY THE SCORE IS AN ENSEMBLE MEAN AND NOT A PER-FOOTPRINT RESIDUAL.
#
# The first version of this experiment scored every model by its per-footprint
# RMS against the truth and reported 45-70% for all three, including the ones
# that are obviously right. The number was real and the metric was wrong.
#
# With a narrow base lobe and a normal spread wider than it, one footprint's
# true aggregate is not a smooth lobe: it is spiky, because only a handful of
# its texels ever align with the half-vector. A per-footprint residual therefore
# charges a model for FAILING TO REPRODUCE THAT SPIKINESS -- which is the exact
# thing the filtering is for. Scored that way, a perfect filter looks like a bad
# fit and an unfiltered point sample looks respectable.
#
# So the two quantities are separated, and both are reported:
#
#   ENSEMBLE  how well the model reproduces the mean response over many
#             footprints, direction by direction. This is "does the surface look
#             right", and it is the number question 2 is about.
#   SPREAD    the per-footprint standard deviation of the truth around that
#             mean. This is the sub-pixel noise -- the sparkle BUDGET. It is a
#             property of the surface and the footprint size, not of any model,
#             so it is reported once per row rather than per model.
#
# Question 3 then measures how much of that budget each model actually spends.


def ensemble_rms(model: np.ndarray, truth: np.ndarray) -> float:
    """Relative RMS between the two direction-wise means over footprints."""
    m = model.mean(axis=0)
    t = truth.mean(axis=0)
    denom = math.sqrt(float(np.mean(t**2)))
    if denom <= 0.0:
        return 0.0
    return math.sqrt(float(np.mean((m - t) ** 2))) / denom


def footprint_spread(truth: np.ndarray) -> float:
    """Per-footprint standard deviation of the truth, relative to its mean --
    the sub-pixel noise a filter has to absorb."""
    t = truth.mean(axis=1).mean(axis=1) if truth.ndim == 3 else truth
    mean = float(np.mean(t))
    if mean <= 0.0:
        return 0.0
    return float(np.std(t) / mean)


def fit_two_lobe(mean_unit, mean_len, truth, mean_alpha, views, lights, quick):
    """Fit a CONVEX mixture (1-w) GGX(a1) + w GGX(a2) to the aggregate.

    Convex, not additive, and the constraint is the point rather than a
    convenience: a convex mixture of two lobes has a directional albedo that is a
    convex combination of the two lobes' albedos, so it can never exceed either
    of them and therefore never manufactures energy. An additive second lobe
    can, and a second specular lobe that quietly brightens every head is the
    failure the first acceptance criterion names.

    BOTH lobe widths are free, as multipliers on the Toksvig alpha, and the
    narrow one is allowed to go BELOW it. Fixing the narrow lobe at the Toksvig
    alpha would have rigged the question: if variance filtering over-widens --
    and Toksvig is known to, it is a single-lobe fit to a distribution that is
    not one -- then the only repair is a NARROWER core, and a fitter denied that
    reports "a second lobe does not help" when what it means is "you did not let
    me move the first one".
    """
    a_ref = toksvig_alpha(mean_len, mean_alpha)

    weights = np.linspace(0.0, 0.8, 9 if quick else 17)
    narrow_scales = np.linspace(0.3, 1.0, 4 if quick else 8)
    broad_scales = np.linspace(1.3, 8.0, 6 if quick else 14)

    best = (math.inf, 0.0, 1.0, 1.0)
    for sn in narrow_scales:
        narrow = model_response(mean_unit, np.clip(a_ref * sn, 1e-3, 1.0), views, lights)
        for sb in broad_scales:
            broad = model_response(mean_unit, np.clip(a_ref * sn * sb, 1e-3, 1.0), views, lights)
            for w in weights:
                err = ensemble_rms((1.0 - w) * narrow + w * broad, truth)
                if err < best[0]:
                    best = (err, float(w), float(sn), float(sb))
    return best


def fit_single_lobe(mean_unit, truth, views, lights, quick):
    """Best single GGX lobe, alpha free and CONSTANT over the patch -- the model
    the engine shades with today."""
    alphas = np.linspace(0.05, 0.9, 18 if quick else 44)
    best = (math.inf, 0.0)
    for a in alphas:
        err = ensemble_rms(model_response(mean_unit, a, views, lights), truth)
        if err < best[0]:
            best = (err, float(a))
    return best


def directional_albedo(response: np.ndarray, lights: np.ndarray) -> np.ndarray:
    """Hemispherical sum of a response, per footprint per view. Not a calibrated
    integral -- the sweep is not a quadrature -- but the SAME sum for every
    model, which is what makes the ratios below meaningful."""
    del lights
    return response.sum(axis=2)


# ---------------------------------------------------------------------------
# 3: sparkle
# ---------------------------------------------------------------------------


def sparkle_sweep(normals, alpha_map, quick):
    """Push the camera in and measure how much each model's response JUMPS
    between consecutive frames.

    The camera distance sets the footprint size in texels. A pixel both shrinks
    and drifts across the surface as the head approaches, so both happen here,
    and the drift is the important half: a sequence of nested crops would let a
    model look stable for the wrong reason.

    Four arms, and the two at the ends are the ones that make the number
    readable:

      point    ONE texel's normal at the window centre, shaded at that texel's
               own roughness. No filtering of any kind -- what a naive sampler
               does, and the arm that sparkles.
      mip      the averaged normal, renormalized, at the averaged roughness.
               A hardware mip chain and nothing else. Discards the variance.
      toksvig  the same, with the roughness widened by |N|. The shipping model.
      truth    the full footprint aggregate. THE FLOOR: whatever this moves by
               is real change in the surface under the pixel, not sparkle, and
               no model should be asked to go below it.
    """
    n = normals.shape[0]

    # One fixed near-grazing view and one light -- sparkle is worst where the
    # lobe is narrow, and averaging over directions would hide it.
    e = math.radians(25.0)
    v = np.array([math.cos(e), 0.0, math.sin(e)])
    e2 = math.radians(35.0)
    l = np.array([math.cos(e2) * math.cos(math.radians(150.0)),
                  math.cos(e2) * math.sin(math.radians(150.0)), math.sin(e2)])

    frames = 60 if quick else 220
    # Footprint edge in texels, shrinking geometrically: a camera approaching at
    # constant speed makes the footprint shrink like 1/distance.
    k_start, k_end = 48.0, 3.0
    ks = k_start * (k_end / k_start) ** (np.arange(frames) / (frames - 1))

    series = {"point": [], "mip": [], "toksvig": [], "kaplan": [], "truth": []}

    for fi, kf in enumerate(ks):
        k = max(2, int(round(kf)))
        # Drift by a smooth, non-integer-multiple amount so consecutive frames
        # see a genuinely different texel set rather than a nested crop.
        off = int(round(fi * 2.7)) % max(1, n - k)
        win = normals[off:off + k, off:off + k].reshape(-1, 3)
        awin = alpha_map[off:off + k, off:off + k].reshape(-1)

        # TRUTH: the aggregate over the footprint.
        series["truth"].append(float(specular_brdf(win, v, l, awin).mean()))

        # POINT: one texel, no filtering at all.
        c = (k // 2) * k + (k // 2)
        series["point"].append(float(specular_brdf(win[c:c + 1], v, l, awin[c:c + 1])[0]))

        mean_vec = win.mean(axis=0)
        mean_len = float(np.linalg.norm(mean_vec))
        mean_unit = (mean_vec / max(mean_len, 1e-12))[None, :]
        # Roughness averaged, then squared -- the same correction as in
        # aggregate_and_models, and for the same reason: the map stores
        # perceptual roughness and the hardware mips THAT.
        mean_alpha = float(np.sqrt(awin).mean()) ** 2

        series["mip"].append(float(specular_brdf(mean_unit, v, l, mean_alpha)[0]))

        a_t = float(toksvig_alpha(np.array(mean_len), mean_alpha))
        series["toksvig"].append(float(specular_brdf(mean_unit, v, l, a_t)[0]))

        # KAPLANYAN: the derivative needs the NEIGHBOURING pixels, so shade the
        # two footprints one pixel to the right and one down and difference the
        # mean normals. That IS dFdx/dFdy -- the shader gets the same two numbers
        # from the hardware for free, but they have to be built by hand here.
        def _mean_unit_at(ox, oy):
            w2 = normals[ox:ox + k, oy:oy + k].reshape(-1, 3).mean(axis=0)
            return w2 / max(float(np.linalg.norm(w2)), 1e-12)

        nx = _mean_unit_at(min(off + k, n - k), off)
        ny = _mean_unit_at(off, min(off + k, n - k))
        dndx = nx - mean_unit[0]
        dndy = ny - mean_unit[0]
        var = SCREEN_SPACE_SIGMA2 * (float(dndx @ dndx) + float(dndy @ dndy))
        a_k = min(math.sqrt(mean_alpha * mean_alpha +
                            min(2.0 * var, SCREEN_SPACE_KAPPA)), 1.0)
        series["kaplan"].append(float(specular_brdf(mean_unit, v, l, a_k)[0]))

    out = {}
    for name, vals in series.items():
        a = np.array(vals)
        mean = float(np.mean(a))
        out[name] = 0.0 if mean <= 0.0 else float(np.mean(np.abs(np.diff(a))) / mean)
    return out


# ---------------------------------------------------------------------------


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--quick", action="store_true", help="coarser patch and sweep")
    args = ap.parse_args()

    n = 256 if args.quick else 512
    # The per-texel base roughness: the micro-roughness band, the part that is
    # below the height field by construction. 0.35 is a plausible authored skin
    # roughness and the number the engine's sample head uses.
    alpha_base = 0.35**2

    height, texel_mm = build_skin_patch(n)
    normals = normals_from_height(height, texel_mm)
    alpha_map = build_sebum_roughness(n, alpha_base)
    views, lights = direction_sweep(args.quick)

    print(f"skin specular reference -- patch {n}x{n} over {PATCH_MM} mm "
          f"({texel_mm * 1000.0:.1f} um/texel), base alpha {alpha_base:.4f}")
    print(f"sweep: {len(views)} views x {len(lights)} lights\n")

    sizes = [4, 8, 16, 32] if args.quick else [2, 4, 8, 16, 32, 64]

    # THE SINGLE LOBE IS FITTED ONCE, AT THE FINEST FOOTPRINT, AND THEN SCORED
    # EVERYWHERE. That is the honest statement of what the engine does today: a
    # material carries ONE authored roughness, an artist tunes it at the distance
    # they are looking at -- which is a close-up, for a head -- and every other
    # distance gets whatever that number happens to give. Refitting it per
    # footprint would model an engine that reauthors the material as the camera
    # dollies, which is not a thing, and would flatter the baseline into looking
    # like a solution to a problem it does not address.
    truth0, mean_unit0, _, _ = aggregate_and_models(normals, sizes[0], alpha_map, views, lights)
    _, single_a = fit_single_lobe(mean_unit0, truth0, views, lights, args.quick)
    del truth0, mean_unit0

    print("QUESTION 2 -- how many lobes, per footprint size")
    print(f"  single-lobe alpha authored at the {sizes[0]}x{sizes[0]} footprint: {single_a:.4f}"
          f"  (base {alpha_base:.4f})")
    print(f"{'footprint':>12} {'um':>7} {'spread':>7} | {'fixed':>7} {'mip':>7} "
          f"{'toksvig':>7} {'kaplan':>7} {'kap*':>7} {'sig2*':>6} {'2 lobe':>7} | {'w':>5} {'narrow':>6} {'broad':>6} "
          f"{'E kap':>6} {'E 2lb':>6}")

    for k in sizes:
        truth, mean_unit, mean_len, mean_alpha = aggregate_and_models(
            normals, k, alpha_map, views, lights)

        single_err = ensemble_rms(model_response(mean_unit, single_a, views, lights), truth)
        spread = footprint_spread(truth)

        # The MIP model: the footprint's own mean roughness, unwidened. This is
        # what an engine with a roughness map and no variance filtering shades --
        # strictly better informed than the fixed authored alpha, and still blind
        # to the normal variance.
        mip_err = ensemble_rms(model_response(mean_unit, mean_alpha, views, lights), truth)

        # The shipping estimator: variance from the screen-space derivatives of
        # the shading normal, which needs the footprints laid out as PIXELS.
        m = int(round(math.sqrt(mean_unit.shape[0])))
        grid = mean_unit.reshape(m, m, 3)
        a_k = kaplanyan_alpha(grid, mean_alpha).reshape(-1)
        kap = model_response(mean_unit, a_k, views, lights)
        kap_err = ensemble_rms(kap, truth)
        kap_e = float(np.mean(directional_albedo(kap, lights) /
                              np.maximum(directional_albedo(truth, lights), 1e-12)))

        # THE STRENGTH KNOB, FITTED. sigma^2 is the estimator's one free
        # parameter -- how much of a pixel the derivative is taken to span --
        # and the row below is why the engine exposes it per profile instead of
        # hard-coding the paper's 0.5. A screen-space estimator measures the
        # variation BETWEEN pixels, not WITHIN one, and those are the same
        # quantity only when the normal map is resolved at about one texel per
        # pixel. Magnify past that (a close-up head, the left-hand rows) and
        # between-pixel variation is real detail the filter should keep; shrink
        # past it (the right-hand rows) and one pixel hides variance no
        # neighbour difference can see. One constant cannot serve both.
        best_sig = (math.inf, SCREEN_SPACE_SIGMA2)
        for sig in np.linspace(0.05, 1.5, 12 if args.quick else 30):
            e = ensemble_rms(model_response(
                mean_unit, kaplanyan_alpha(grid, mean_alpha, sigma2=float(sig)).reshape(-1),
                views, lights), truth)
            if e < best_sig[0]:
                best_sig = (e, float(sig))

        a_t = toksvig_alpha(mean_len, mean_alpha)
        tok = model_response(mean_unit, a_t, views, lights)
        tok_err = ensemble_rms(tok, truth)
        tok_e = float(np.mean(directional_albedo(tok, lights) /
                              np.maximum(directional_albedo(truth, lights), 1e-12)))

        two_err, w, sn, sb = fit_two_lobe(mean_unit, mean_len, truth, mean_alpha,
                                          views, lights, args.quick)

        # Energy: the two-lobe mixture's hemispherical sum against the truth's.
        # A convex mixture cannot exceed the larger of its lobes, so this number
        # is a check that the CONSTRUCTION held, not a fitted quantity.
        narrow = model_response(mean_unit, np.clip(a_t * sn, 1e-3, 1.0), views, lights)
        broad = model_response(mean_unit, np.clip(a_t * sn * sb, 1e-3, 1.0), views, lights)
        mixed = (1.0 - w) * narrow + w * broad
        e_ratio = float(np.mean(directional_albedo(mixed, lights) /
                                np.maximum(directional_albedo(truth, lights), 1e-12)))

        print(f"{k:>7}x{k:<4} {k * texel_mm * 1000.0:>6.0f}u "
              f"{spread:>6.1%} | {single_err:>6.1%} {mip_err:>6.1%} {tok_err:>6.1%} "
              f"{kap_err:>6.1%} {best_sig[0]:>6.1%} {best_sig[1]:>6.2f} "
              f"{two_err:>6.1%} | {w:>5.2f} {sn:>6.2f} {sb:>6.2f} "
              f"{kap_e:>6.3f} {e_ratio:>6.3f}")
    print("\n  'spread' is the per-footprint standard deviation of the TRUTH about its own")
    print("  mean -- the sub-pixel noise, a property of the surface and not of any model.")
    print("  It is the sparkle budget question 3 measures against.")
    print("  'fixed' is the authored alpha above -- one number for every distance.")
    print("  'mip'   is the footprint's own mean roughness: a roughness map, no filtering.")
    print("  'toksvig' widens the mip alpha from |N| -- the IDEAL estimator, which this")
    print("          engine cannot compute: decodeTangentNormal renormalizes (see the note).")
    print("  'kaplan' widens it from the screen-space normal derivatives -- what SHIPS.")
    print("  '2 lobe' is a convex mixture whose two widths multiply the toksvig ALPHA by")
    print("          'narrow' and 'narrow x broad'. Those are ALPHA ratios: the engine's")
    print("          LobeRoughnessScale is on ROUGHNESS, which is their square root.")
    print("  'E kap'/'E 2lb' are hemispherical energy against truth; 1.000 is neutral.")

    print("\nQUESTION 3 -- sparkle: mean |frame-to-frame change| / mean response")
    sp = sparkle_sweep(normals, alpha_map, args.quick)
    for name in ("point", "mip", "toksvig", "kaplan", "truth"):
        rel = sp[name] / sp["truth"] if sp["truth"] > 0 else 0.0
        tag = "  <- the floor" if name == "truth" else f"  ({rel:.1f}x the floor)"
        print(f"{name:>12}: {sp[name]:>8.1%}{tag}")
    print("\n  (camera pushed in over a shrinking, DRIFTING footprint at a grazing view.")
    print("   'truth' is the full footprint aggregate: real change under the pixel, and")
    print("   the floor no model should be asked to beat.)")


if __name__ == "__main__":
    main()
