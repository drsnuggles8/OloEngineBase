#!/usr/bin/env python3
"""Reference comparison for issue #1241: which diffusion profile, and what does
the separable approximation cost?

This is the experiment behind the first acceptance criterion ("select and
document a diffusion model using a small reference comparison before tuning
production filters"). It answers three questions, in order:

  1. GROUND TRUTH. A Monte Carlo random walk in a semi-infinite, homogeneous,
     isotropically scattering medium gives the radial distribution of light
     re-emerging around a pencil beam. No analytic profile is assumed anywhere
     in it -- it is photons, a scattering albedo and a surface.

  2. WHICH ANALYTIC PROFILE. Burley normalized diffusion, a single Gaussian and
     the classic six-Gaussian skin sum are each fitted to that reference under
     the SAME parameterisation the engine has available (transport albedo +
     mean free path, which is what a `.oloskin` authors) and scored.

  3. WHAT SEPARABILITY COSTS. The engine's screen-space filter is separable:
     it applies a 1D kernel twice. A radially symmetric 2D profile is not
     separable, so this measures the error of doing it anyway, against the true
     2D convolution, on the cases a face actually shows -- a shadow terminator
     and a point highlight.

Run:  python compare_profiles.py            (about 40 s)
      python compare_profiles.py --quick    (about 5 s, coarser MC)

Results are recorded in docs/guides/skin-diffusion.md. Nothing in the engine
imports this file; it is the evidence, not the implementation.
"""

from __future__ import annotations

import argparse
import math

import numpy as np

# `numpy.trapezoid` is the NumPy 2.0 rename of `numpy.trapz`. Bind it once rather
# than pinning a minimum version: this script is evidence someone should be able
# to re-run years from now, and it needs nothing else that 2.0 introduced.
_trapz = getattr(np, "trapezoid", None) or np.trapz

# The engine's authored reference head, for reference:
#   ScatterColor    [0.85, 0.55, 0.45]   transport albedo, linear Rec.709
#   ScatterRadiusMM [1.55, 0.80, 0.55]   mean free path, millimetres
CHANNELS = (
    ("red", 0.85, 1.55),
    ("green", 0.55, 0.80),
    ("blue", 0.45, 0.55),
)


# ---------------------------------------------------------------------------
# 1. Ground truth
# ---------------------------------------------------------------------------


def monte_carlo_radial_profile(
    single_scatter_albedo: float,
    mean_free_path: float,
    edges: np.ndarray,
    photons: int,
    rng: np.random.Generator,
) -> tuple[np.ndarray, float]:
    """Random-walk a pencil beam into a semi-infinite medium.

    The beam enters at the origin along -z (into the medium). Each photon takes
    exponentially distributed steps of mean `mean_free_path`, scatters
    isotropically, and is killed with probability (1 - single_scatter_albedo) at
    each interaction. A photon that crosses back over z = 0 exits, and the
    radius at which it exits is binned.

    Returns (energy per bin, diffuse reflectance). The energy is a fraction of
    the incident photons, so summing the bins and the absorbed remainder gives
    one.
    """
    alive = np.ones(photons, dtype=bool)
    # Everything starts at the origin heading straight in. The first step is
    # taken before any scattering, which is what makes this a pencil beam
    # rather than a point source at depth.
    x = np.zeros(photons)
    y = np.zeros(photons)
    z = np.zeros(photons)
    dx = np.zeros(photons)
    dy = np.zeros(photons)
    dz = -np.ones(photons)

    exit_radii: list[np.ndarray] = []

    # A photon in a scattering medium with albedo < 1 has a finite expected path
    # length; 4000 interactions is far beyond the tail for the albedos here and
    # the loop reports if it ever truncates.
    max_events = 4000
    truncated = 0
    for _ in range(max_events):
        n = int(np.count_nonzero(alive))
        if n == 0:
            break

        step = rng.exponential(mean_free_path, n)
        idx = np.flatnonzero(alive)
        nx = x[idx] + dx[idx] * step
        ny = y[idx] + dy[idx] * step
        nz = z[idx] + dz[idx] * step

        # Crossed the surface: exited. Interpolate to the exact crossing point
        # rather than using the overshot position, or every exit radius is
        # biased outward by half a step.
        crossed = nz >= 0.0
        if np.any(crossed):
            cidx = idx[crossed]
            t = np.where(dz[cidx] != 0.0, -z[cidx] / dz[cidx], 0.0)
            ex = x[cidx] + dx[cidx] * t
            ey = y[cidx] + dy[cidx] * t
            exit_radii.append(np.hypot(ex, ey))
            alive[cidx] = False

        inside = ~crossed
        iidx = idx[inside]
        x[iidx] = nx[inside]
        y[iidx] = ny[inside]
        z[iidx] = nz[inside]

        # Absorption.
        if iidx.size:
            absorbed = rng.random(iidx.size) >= single_scatter_albedo
            alive[iidx[absorbed]] = False
            survivors = iidx[~absorbed]

            # Isotropic scatter.
            u = rng.random(survivors.size) * 2.0 - 1.0
            phi = rng.random(survivors.size) * 2.0 * math.pi
            s = np.sqrt(np.maximum(1.0 - u * u, 0.0))
            dx[survivors] = s * np.cos(phi)
            dy[survivors] = s * np.sin(phi)
            dz[survivors] = u
    else:
        truncated = int(np.count_nonzero(alive))

    if truncated:
        print(f"    warning: {truncated} photons hit the event cap (albedo too high?)")

    radii = np.concatenate(exit_radii) if exit_radii else np.zeros(0)
    counts, _ = np.histogram(radii, bins=edges)
    energy = counts.astype(np.float64) / float(photons)
    reflectance = float(radii.size) / float(photons)
    return energy, reflectance


def solve_single_scatter_albedo(target_reflectance: float, mfp: float, edges, photons, rng) -> float:
    """Find the single-scattering albedo whose diffuse reflectance matches the
    authored transport albedo.

    `.oloskin` authors a TRANSPORT albedo -- the fraction that comes back out --
    not a per-collision one. Bisection on a monotone function, with a small
    photon count because only the scalar reflectance is needed here.
    """
    lo, hi = 0.0, 1.0 - 1e-6
    for _ in range(18):
        mid = 0.5 * (lo + hi)
        _, refl = monte_carlo_radial_profile(mid, mfp, edges, photons, rng)
        if refl < target_reflectance:
            lo = mid
        else:
            hi = mid
    return 0.5 * (lo + hi)


# ---------------------------------------------------------------------------
# 2. Candidate analytic profiles -- all normalised to integrate to 1 over the
#    plane, so they are compared on SHAPE and the reflectance is applied after.
# ---------------------------------------------------------------------------


def burley_shape(albedo: float) -> float:
    """Christensen & Burley 2015 eq. 6, searchlight configuration. Mirrors
    SkinBurleyShapeFromAlbedo in OloEngine/src/OloEngine/Renderer/SkinDiffusion.cpp."""
    return 1.85 - albedo + 7.0 * abs(albedo - 0.8) ** 3


def burley_cdf(r: np.ndarray, d: float) -> np.ndarray:
    return 1.0 - 0.25 * np.exp(-r / d) - 0.75 * np.exp(-r / (3.0 * d))


def burley_pdf(r: np.ndarray, d: float) -> np.ndarray:
    with np.errstate(divide="ignore", invalid="ignore"):
        return np.where(r > 0.0, (np.exp(-r / d) + np.exp(-r / (3.0 * d))) / (8.0 * math.pi * d * r), 0.0)


def gaussian_cdf(r: np.ndarray, sigma: float) -> np.ndarray:
    """CDF of a normalised 2D radially symmetric Gaussian."""
    return 1.0 - np.exp(-(r**2) / (2.0 * sigma**2))


def gaussian_pdf(r: np.ndarray, sigma: float) -> np.ndarray:
    return np.exp(-(r**2) / (2.0 * sigma**2)) / (2.0 * math.pi * sigma**2)


# d'Eon & Luebke's six-Gaussian fit of the classic three-layer skin profile
# (GPU Gems 3, ch. 14), as (weight, variance in mm^2) per channel. Weights are
# renormalised to sum to one here -- the published ones carry the per-channel
# reflectance, which this comparison factors out.
DEON_SIX_GAUSSIAN = {
    "red": [(0.233, 0.0064), (0.100, 0.0484), (0.118, 0.187), (0.113, 0.567), (0.358, 1.99), (0.078, 7.41)],
    "green": [(0.455, 0.0064), (0.336, 0.0484), (0.198, 0.187), (0.007, 0.567), (0.004, 1.99), (0.000, 7.41)],
    "blue": [(0.649, 0.0064), (0.344, 0.0484), (0.000, 0.187), (0.007, 0.567), (0.000, 1.99), (0.000, 7.41)],
}


def six_gaussian_cdf(r: np.ndarray, lobes, scale: float) -> np.ndarray:
    total = sum(w for w, _ in lobes)
    out = np.zeros_like(r)
    for w, var in lobes:
        out += (w / total) * gaussian_cdf(r, math.sqrt(var) * scale)
    return out


# ---------------------------------------------------------------------------
# Scoring
# ---------------------------------------------------------------------------


def binned_energy(cdf_fn, edges: np.ndarray) -> np.ndarray:
    c = cdf_fn(edges)
    return np.diff(c)


def relative_rms(model: np.ndarray, reference: np.ndarray) -> float:
    """RMS of the per-bin relative error, over bins the reference actually
    populates. Relative rather than absolute because the interesting part of the
    profile -- the tail that makes red bleed -- is three orders of magnitude
    below the peak, and an absolute metric scores it as free."""
    mask = reference > (reference.max() * 1e-4)
    if not np.any(mask):
        return float("nan")
    rel = (model[mask] - reference[mask]) / reference[mask]
    return float(np.sqrt(np.mean(rel**2)))


def fit_scale(cdf_factory, edges: np.ndarray, reference: np.ndarray, lo: float, hi: float) -> tuple[float, float]:
    """Golden-section search for the scale minimising relative RMS."""
    phi = (math.sqrt(5.0) - 1.0) / 2.0
    a, b = lo, hi
    c, d = b - phi * (b - a), a + phi * (b - a)
    fc = relative_rms(binned_energy(cdf_factory(c), edges), reference)
    fd = relative_rms(binned_energy(cdf_factory(d), edges), reference)
    for _ in range(60):
        if fc < fd:
            b, d, fd = d, c, fc
            c = b - phi * (b - a)
            fc = relative_rms(binned_energy(cdf_factory(c), edges), reference)
        else:
            a, c, fc = c, d, fd
            d = a + phi * (b - a)
            fd = relative_rms(binned_energy(cdf_factory(d), edges), reference)
    best = 0.5 * (a + b)
    return best, relative_rms(binned_energy(cdf_factory(best), edges), reference)


# ---------------------------------------------------------------------------
# 3. Separability error
# ---------------------------------------------------------------------------


def burley_strip_fraction(a: float, d: float, samples: int = 1024, extent: float = 64.0) -> float:
    """S(a): the fraction of the profile's energy lying in the VERTICAL STRIP
    |x| <= a. Unitless [0,1]; `a` and `d` in millimetres.

    This is the integral of the profile's LINE SPREAD FUNCTION -- its projection
    onto one axis -- and it is what a 1D kernel's tap weights have to be for a
    two-pass separable blur to reproduce the true 2D response of a STRAIGHT
    EDGE. A lit face is mostly straight edges: a shadow terminator, an N.L
    falloff, the shaded side of a nose. Using the radial profile R(r) directly
    as a 1D kernel, which is the obvious thing and what most separable
    implementations do, answers a different question; the table at the end of
    this script measures the difference.

    IT IS COMPUTED AS A STRIP, NOT AS A POINTWISE LSF, BECAUSE THE POINTWISE LSF
    IS SINGULAR. R(r) has a 1/r pole at the origin and its line integral has a
    logarithmic one, so sampling LSF(x) near x = 0 gives a value that depends on
    the quadrature step rather than on the profile. In polar form the pole
    cancels against the Jacobian -- R(r) * r is smooth everywhere -- and the
    strip's energy comes out of a 1D integral with no singular point at all:

        S(a) = CDF(a) + (1 / 2*pi*d) * integral_a^inf asin(a/r) (e^-r/d + e^-r/3d) dr

    The first term is the disc of radius a, which is wholly inside the strip;
    the second is the angular fraction (2/pi) asin(a/r) of each annulus beyond
    it that still falls within |x| <= a.
    """
    if a <= 0.0 or d <= 0.0:
        return 0.0
    upper = extent * d
    if a >= upper:
        return 1.0
    # LOG-SPACED, via r = a * e^t. The wedge integrand decays like a/r, so its
    # value is spread evenly across DECADES of r, not across its linear span --
    # a linear grid from a tiny `a` puts one sample in the region that holds
    # most of the integral and overstates it several-fold. The substitution
    # makes the integrand smooth and bounded in t.
    t = np.linspace(0.0, math.log(upper / a), samples)
    r = a * np.exp(t)
    integrand = np.arcsin(np.minimum(a / r, 1.0)) * (np.exp(-r / d) + np.exp(-r / (3.0 * d))) * r
    wedge = float(_trapz(integrand, t)) / (2.0 * math.pi * d)
    return float(np.clip(float(burley_cdf(np.array([a]), d)[0]) + wedge, 0.0, 1.0))


def lsf_tap_weights(edges_mm: np.ndarray, d: float) -> np.ndarray:
    """Tap weights from the strip fractions of each tap's interval.

    S() covers BOTH sides of the axis, so an off-centre tap takes half of its
    interval's strip energy and the centre tap, whose interval straddles zero,
    takes its whole one."""
    w = np.empty(edges_mm.size - 1)
    for i in range(w.size):
        lo, hi = float(edges_mm[i]), float(edges_mm[i + 1])
        if lo < 0.0 < hi:
            w[i] = burley_strip_fraction(min(-lo, hi), d)
        else:
            a, b = sorted((abs(lo), abs(hi)))
            w[i] = 0.5 * (burley_strip_fraction(b, d) - burley_strip_fraction(a, d))
    return np.maximum(w, 0.0)


def separability_error(d: float, support_mm: float, taps: int, pixels_per_mm: float) -> dict[str, float]:
    """Compare a separable 1D application of the radial profile against the true
    2D radial convolution, on the two features a face shows.

    Both operators are built on the same grid and the same total energy, so the
    difference measured is separability alone -- not tap count, not clamping.
    """
    support_px = support_mm * pixels_per_mm
    half = int(math.ceil(support_px)) + 2
    grid = np.arange(-half, half + 1, dtype=np.float64)

    # True 2D kernel, as per-cell ENERGY (not density): the radial profile times
    # the cell's area in mm^2. Units matter here -- burley_pdf is 1/mm^2, so a
    # cell's share is pdf * (1/ppm)^2, and mixing the two is how a centre weight
    # ends up wrong by a factor of ppm^4.
    xx, yy = np.meshgrid(grid, grid)
    rr = np.hypot(xx, yy) / pixels_per_mm
    cell_area = (1.0 / pixels_per_mm) ** 2
    k2 = burley_pdf(rr, d) * cell_area
    # The 1/r singularity lands on exactly one cell. Its true share is the
    # analytic energy of the disc of half a texel -- straight from the CDF, in
    # the same (already dimensionless) units as every other cell.
    centre = half
    k2[centre, centre] = float(burley_cdf(np.array([0.5 / pixels_per_mm]), d)[0])
    k2 /= k2.sum()

    # The tap layout the engine uses: quadratically spaced, signed, symmetric.
    offs = np.linspace(-1.0, 1.0, taps)
    offs = np.sign(offs) * offs**2 * support_mm
    edges = np.empty(taps + 1)
    edges[1:-1] = 0.5 * (offs[:-1] + offs[1:])
    edges[0], edges[-1] = -support_mm, support_mm

    # Candidate A -- RADIAL: the radial profile's annulus energy used as a 1D
    # kernel. The obvious construction.
    w_radial = np.abs(burley_cdf(np.abs(edges[1:]), d) - burley_cdf(np.abs(edges[:-1]), d))
    w_radial /= w_radial.sum()

    # Candidate B -- LSF: the line-spread function's energy over the same
    # intervals.
    w_lsf = lsf_tap_weights(edges, d)
    w_lsf /= w_lsf.sum()

    def splat(weights):
        """Resample a tap table onto the pixel grid by LINEAR splatting, which
        is what a bilinear texture fetch at a fractional offset does on the
        GPU -- nearest-tap rounding would measure a choice the shader does not
        make."""
        k = np.zeros_like(grid)
        for off, weight in zip(offs, weights):
            p = off * pixels_per_mm + half
            lo_i = int(math.floor(p))
            frac = p - lo_i
            if 0 <= lo_i < k.size:
                k[lo_i] += weight * (1.0 - frac)
            if 0 <= lo_i + 1 < k.size:
                k[lo_i + 1] += weight * frac
        return k / k.sum()

    def apply2d(img):
        from numpy.fft import irfft2, rfft2

        pad = np.zeros_like(img)
        h, wid = k2.shape
        pad[:h, :wid] = k2
        pad = np.roll(pad, (-half, -half), axis=(0, 1))
        return irfft2(rfft2(img) * rfft2(pad), img.shape)

    def apply_sep(img, k1):
        out = np.apply_along_axis(lambda row: np.convolve(row, k1, mode="same"), 1, img)
        return np.apply_along_axis(lambda col: np.convolve(col, k1, mode="same"), 0, out)

    size = 8 * half
    inner = np.s_[2 * half : -2 * half, 2 * half : -2 * half]

    # A shadow terminator: the feature a lit face is mostly made of, and the one
    # that shows colour bleed.
    edge = np.zeros((size, size))
    edge[:, : size // 2] = 1.0
    truth_edge = apply2d(edge)

    # A thin bright line -- a specular-adjacent highlight on a nose or a brow.
    # Narrower than the kernel, so it is where an over-wide 1D kernel shows.
    line = np.zeros((size, size))
    line[:, size // 2] = 1.0
    truth_line = apply2d(line)
    line_peak = float(np.max(truth_line[inner]))

    results: dict[str, float] = {}
    for label, weights in (("radial", w_radial), ("lsf", w_lsf)):
        k1 = splat(weights)
        results[f"{label}_terminator"] = float(np.max(np.abs(truth_edge[inner] - apply_sep(edge, k1)[inner])))
        line_err = float(np.max(np.abs(truth_line[inner] - apply_sep(line, k1)[inner])))
        results[f"{label}_line"] = line_err / line_peak if line_peak > 0 else float("nan")

    return results


# ---------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--quick", action="store_true", help="fewer photons; indicative only")
    args = parser.parse_args()

    photons = 200_000 if args.quick else 4_000_000
    fit_photons = 20_000 if args.quick else 120_000
    rng = np.random.default_rng(20261241)

    print(f"Monte Carlo ground truth: {photons:,} photons per channel\n")

    # Log-spaced bins: the profile spans three decades and linear bins put every
    # bin in the tail or every bin in the peak.
    edges = np.concatenate(([0.0], np.geomspace(0.02, 40.0, 60)))
    centres = 0.5 * (edges[1:] + edges[:-1])

    print(f"{'channel':<8}{'A':>6}{'mfp/mm':>8}{'alpha_ss':>10}{'R_mc':>8}"
          f"{'burley':>9}{'1-gauss':>9}{'6-gauss':>9}")
    print("-" * 68)

    burley_scores = []
    for name, albedo, mfp in CHANNELS:
        alpha = solve_single_scatter_albedo(albedo, mfp, edges, fit_photons, rng)
        energy, reflectance = monte_carlo_radial_profile(alpha, mfp, edges, photons, rng)
        shape = energy / energy.sum()

        # Burley, parameterised EXACTLY as the engine does -- d = mfp / s(A),
        # no fitting. This is the honest comparison: the other two get a free
        # scale parameter fitted to the reference, Burley does not.
        d_engine = mfp / burley_shape(albedo)
        burley_rms = relative_rms(binned_energy(lambda r, _d=d_engine: burley_cdf(r, _d), edges), shape)

        _, gauss_rms = fit_scale(
            lambda s: (lambda r: gaussian_cdf(r, s)), edges, shape, 0.05 * mfp, 20.0 * mfp
        )
        _, six_rms = fit_scale(
            lambda s, _n=name: (lambda r: six_gaussian_cdf(r, DEON_SIX_GAUSSIAN[_n], s)),
            edges, shape, 0.05, 20.0,
        )

        burley_scores.append((name, d_engine, burley_rms))
        print(f"{name:<8}{albedo:>6.2f}{mfp:>8.2f}{alpha:>10.4f}{reflectance:>8.3f}"
              f"{burley_rms:>9.3f}{gauss_rms:>9.3f}{six_rms:>9.3f}")

    print("\nrelative RMS of the radial energy distribution, lower is better.")
    print("Burley is UNFITTED (d = mfp / s(A)); the Gaussians each got a fitted scale.\n")

    print("Which 1D kernel, given the blur is separable (Medium tier, 17 taps):")
    print(f"{'channel':<8}{'d/mm':>8}{'px/mm':>8}"
          f"{'term:radial':>13}{'term:lsf':>10}{'line:radial':>13}{'line:lsf':>10}")
    print("-" * 72)
    for name, d_engine, _ in burley_scores:
        # A head filling a 1080p frame at a typical portrait framing is roughly
        # 8 px/mm; 2 px/mm is the same head at conversational distance.
        for ppm in (2.0, 8.0):
            support = 12.0 * d_engine  # ~99.5% of the energy
            err = separability_error(d_engine, support, 17, ppm)
            print(f"{name:<8}{d_engine:>8.3f}{ppm:>8.1f}"
                  f"{err['radial_terminator']:>13.4f}{err['lsf_terminator']:>10.4f}"
                  f"{err['radial_line']:>13.4f}{err['lsf_line']:>10.4f}")

    print("\nterm: max absolute error against the true 2D convolution on a 0->1 step.")
    print("line: same on a one-pixel bright line, relative to the true peak.")
    print("radial = the radial profile used as a 1D kernel; lsf = its line-spread function.")

    print("\nQuality tiers, LSF kernel, red channel (the widest, so the worst case):")
    print(f"{'tier':<8}{'taps':>6}{'fetches/px':>12}{'px/mm':>8}{'term':>10}{'line':>10}")
    print("-" * 54)
    d_red = burley_scores[0][1]
    for tier, taps in (("Low", 9), ("Medium", 17), ("High", 25)):
        for ppm in (2.0, 8.0):
            err = separability_error(d_red, 12.0 * d_red, taps, ppm)
            print(f"{tier:<8}{taps:>6}{2 * taps:>12}{ppm:>8.1f}"
                  f"{err['lsf_terminator']:>10.4f}{err['lsf_line']:>10.4f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
