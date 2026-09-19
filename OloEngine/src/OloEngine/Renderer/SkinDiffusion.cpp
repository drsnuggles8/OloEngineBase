#include "OloEnginePCH.h"
#include "OloEngine/Renderer/SkinDiffusion.h"

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    namespace
    {
        // Bisection bounds for SkinBurleyRadiusForFraction. The CDF is strictly
        // increasing in r, so a bracket plus a fixed iteration count is both
        // sufficient and terminating — a Newton step would be faster and would
        // need a guard against the flat tail, which is where the answer is.
        constexpr i32 kCdfInversionIterations = 48;

        // The CDF reaches 0.995 at roughly 12d for every albedo (the 3d
        // exponential dominates the tail), so 64d is a bracket with an order of
        // magnitude to spare and no search for an upper bound.
        constexpr f32 kCdfInversionUpperBoundInD = 64.0f;
    } // namespace

    f32 SkinBurleyShapeFromAlbedo(f32 albedo) noexcept
    {
        // Christensen & Burley 2015, "Approximate Reflectance Profiles for
        // Efficient Subsurface Scattering", eq. 6 — the searchlight fit, which
        // is the one parameterised by the MEAN FREE PATH rather than by the
        // diffuse mean free path. That matches what `.oloskin` authors:
        // ScatterRadiusMM's own comment says "mean free path".
        //
        // Clamped input rather than a precondition: Sanitize keeps ScatterColor
        // in [0,1], and a clamp here is one instruction against a class of
        // caller that would otherwise have to be trusted.
        const f32 a = std::clamp(albedo, 0.0f, 1.0f);
        const f32 t = std::abs(a - 0.8f);
        return 1.85f - a + (7.0f * t * t * t);
    }

    glm::vec3 SkinBurleyScalingMM(const SkinProfileParameters& parameters) noexcept
    {
        // s is bounded below by 1.85 - 1 = 0.85 on [0,1] (the cubic term is
        // non-negative), so this division cannot blow up and the result inherits
        // ScatterRadiusMM's positive floor.
        return glm::vec3(parameters.ScatterRadiusMM.x / SkinBurleyShapeFromAlbedo(parameters.ScatterColor.x),
                         parameters.ScatterRadiusMM.y / SkinBurleyShapeFromAlbedo(parameters.ScatterColor.y),
                         parameters.ScatterRadiusMM.z / SkinBurleyShapeFromAlbedo(parameters.ScatterColor.z));
    }

    f32 SkinBurleyProfile(f32 r, f32 d) noexcept
    {
        if (!std::isfinite(r) || !std::isfinite(d) || d <= 0.0f || r <= 0.0f)
            return 0.0f;

        const f32 e1 = std::exp(-r / d);
        const f32 e2 = std::exp(-r / (3.0f * d));
        constexpr f32 kEightPi = 8.0f * 3.14159265358979323846f;
        return (e1 + e2) / (kEightPi * d * r);
    }

    f32 SkinBurleyCdf(f32 r, f32 d) noexcept
    {
        if (!std::isfinite(r) || !std::isfinite(d) || d <= 0.0f || r <= 0.0f)
            return 0.0f;

        const f32 cdf = 1.0f - (0.25f * std::exp(-r / d)) - (0.75f * std::exp(-r / (3.0f * d)));
        return std::clamp(cdf, 0.0f, 1.0f);
    }

    f32 SkinBurleyRadiusForFraction(f32 fraction, f32 d) noexcept
    {
        if (!std::isfinite(d) || d <= 0.0f)
            return 0.0f;
        if (!std::isfinite(fraction) || fraction <= 0.0f)
            return 0.0f;

        // The true inverse of 1.0 is infinite. Clamping to a fraction just
        // under it is a bounded answer rather than an infinity a caller would
        // have to test for; kSkinDiffusionSupportFraction is what callers
        // actually ask for and sits well below this.
        const f32 target = std::min(fraction, 0.9999f);

        f32 low = 0.0f;
        f32 high = kCdfInversionUpperBoundInD * d;
        for (i32 i = 0; i < kCdfInversionIterations; ++i)
        {
            const f32 mid = 0.5f * (low + high);
            if (SkinBurleyCdf(mid, d) < target)
                low = mid;
            else
                high = mid;
        }
        return 0.5f * (low + high);
    }

    f32 SkinBurleyStripFraction(f32 a, f32 d) noexcept
    {
        if (!std::isfinite(a) || !std::isfinite(d) || d <= 0.0f || a <= 0.0f)
            return 0.0f;

        const f32 upper = kCdfInversionUpperBoundInD * d;
        if (a >= upper)
            return 1.0f;

        // LOG-SPACED QUADRATURE, via r = a * e^t. The wedge integrand decays
        // like a/r, so its value is spread evenly across DECADES of r rather
        // than across its linear span: a linear grid starting at a tap edge only
        // a thousandth of a millimetre out puts ONE sample in the region holding
        // most of the integral and overstates it several-fold — which shows up
        // as a centre tap with far too much weight and a blur that does nothing.
        // The substitution makes the integrand bounded and smooth in t.
        constexpr i32 kStripQuadratureSamples = 512;
        const f32 tMax = std::log(upper / a);
        const f32 step = tMax / static_cast<f32>(kStripQuadratureSamples - 1);
        f32 sum = 0.0f;
        for (i32 i = 0; i < kStripQuadratureSamples; ++i)
        {
            const f32 t = step * static_cast<f32>(i);
            const f32 r = a * std::exp(t);
            const f32 quadWeight = (i == 0 || i == kStripQuadratureSamples - 1) ? 0.5f : 1.0f;
            const f32 wedge = std::asin(std::min(a / r, 1.0f));
            sum += quadWeight * wedge * (std::exp(-r / d) + std::exp(-r / (3.0f * d))) * r;
        }

        constexpr f32 kTwoPi = 2.0f * 3.14159265358979323846f;
        const f32 strip = SkinBurleyCdf(a, d) + ((sum * step) / (kTwoPi * d));
        return std::isfinite(strip) ? std::clamp(strip, 0.0f, 1.0f) : 0.0f;
    }

    f32 SkinDiffusionSupportRadiusMM(const SkinProfileParameters& parameters) noexcept
    {
        const glm::vec3 d = SkinBurleyScalingMM(parameters);
        // The WIDEST channel, because the tap offsets are shared: a support
        // sized to the average would truncate red's tail, which is precisely the
        // part of the profile that makes skin read as skin.
        const f32 r = std::max({ SkinBurleyRadiusForFraction(kSkinDiffusionSupportFraction, d.x),
                                 SkinBurleyRadiusForFraction(kSkinDiffusionSupportFraction, d.y),
                                 SkinBurleyRadiusForFraction(kSkinDiffusionSupportFraction, d.z) });
        return std::isfinite(r) ? std::max(r, 0.0f) : 0.0f;
    }

    SkinDiffusionKernel SkinDiffusionKernel::Identity() noexcept
    {
        SkinDiffusionKernel kernel{};
        kernel.SupportRadiusMM = 0.0f;
        kernel.TapCount = 1;
        kernel.Taps[0] = glm::vec4(0.0f, 1.0f, 1.0f, 1.0f);
        return kernel;
    }

    bool SkinDiffusionKernel::IsIdentity() const noexcept
    {
        return TapCount <= 1;
    }

    SkinDiffusionKernel BuildSkinDiffusionKernel(const SkinProfileParameters& parameters,
                                                 SkinDiffusionQuality quality)
    {
        // THE VERSION BRANCH, CPU SIDE. The shader has the same one (see
        // oloSkinDiffusionOutput in include/PBRCommon.glsl) and both exist for
        // the reason ADR 0024 gives: a profile authored against version 0 must
        // keep shading as it did when a renderer setting turns diffusion on.
        // Returning the identity here means such a profile also uploads a
        // harmless kernel rather than relying on the shader's branch alone.
        //
        // EVERY DIFFUSING VERSION, AND THE LIST MUST MATCH THE SHADER'S. Version
        // 2 (#1242) is "everything version 1 does, plus transmission", so it
        // diffuses too. This test was `!= ScreenSpaceDiffusion` until #1242 and
        // that is exactly the shape of bug it caused: the SHADER was taught to
        // hand a version-2 pixel's diffuse half to the pass, while THIS function
        // still answered with an identity kernel — so the pass dutifully blurred
        // by nothing and added `blur(aux) - aux == 0`. A version-2 head lost its
        // subsurface scattering and gained backlit ears in the same authoring
        // click, which reads as "the new feature broke #1241".
        //
        // It was caught by SkinTransmissionEvidenceTest's front-lit A/B, whose
        // two arms were a version-1 and a version-2 profile: with the
        // transmission term correctly zero under front lighting the two frames
        // should have been identical, and they differed by 19/255 across 25 000
        // pixels — which was the missing blur, not the term.
        //
        // Spelled as an explicit list of the versions that diffuse rather than
        // as `>= ScreenSpaceDiffusion`, so a version appended to
        // SkinEvaluationModel that does NOT diffuse cannot inherit it silently.
        //
        // THE PRICE OF THAT SAFETY IS AN EDIT PER VERSION, AND #1243 PAID IT
        // TOO. Version 3 was appended, every GLSL version list was found by
        // grepping the shaders, and this CPU-side list was missed — reproducing
        // the #1242 failure above exactly: a version-3 head lost its subsurface
        // scattering and the difference showed up as 66/255 across 55 000 pixels
        // in SkinLayeredSpecularEvidenceTest's neutral-identity A/B, which is a
        // test whose whole job is to notice that moving a profile forward one
        // version changed a pixel. Twice is a pattern: anyone appending a
        // version 4 should grep for `ThicknessTransmission` across .cpp AND
        // .glsl, not just the shaders.
        if (parameters.EvaluationModel != SkinEvaluationModel::ScreenSpaceDiffusion &&
            parameters.EvaluationModel != SkinEvaluationModel::ThicknessTransmission &&
            parameters.EvaluationModel != SkinEvaluationModel::LayeredSpecular &&
            parameters.EvaluationModel != SkinEvaluationModel::OralSurface)
            return SkinDiffusionKernel::Identity();

        const f32 supportRadiusMM = SkinDiffusionSupportRadiusMM(parameters);
        if (!(supportRadiusMM > 0.0f))
            return SkinDiffusionKernel::Identity();

        const glm::vec3 d = SkinBurleyScalingMM(parameters);
        if (!std::isfinite(d.x) || !std::isfinite(d.y) || !std::isfinite(d.z))
            return SkinDiffusionKernel::Identity();

        const u32 tapCount = GetSkinDiffusionTapCount(
            IsValidSkinDiffusionQuality(static_cast<i32>(quality)) ? quality : SkinDiffusionQuality::Medium);
        const i32 halfCount = static_cast<i32>(tapCount / 2u);

        SkinDiffusionKernel kernel{};
        kernel.SupportRadiusMM = supportRadiusMM;
        kernel.TapCount = tapCount;

        // TAP PLACEMENT IS QUADRATIC, NOT UNIFORM. The profile puts most of its
        // energy inside a small fraction of its support, so uniformly spaced
        // taps spend two thirds of the budget on a tail that contributes almost
        // nothing while under-sampling the peak — which shows up as a ring
        // around a bright highlight, not as a soft error. Squaring the
        // normalised index concentrates taps near the centre and is what
        // Jimenez's separable filter does for the same reason.
        //
        // The offsets stay SIGNED and symmetric: the blur must not shift the
        // image, and a one-sided table would.
        const auto offsetAt = [halfCount](i32 i) -> f32
        {
            const f32 t = static_cast<f32>(i) / static_cast<f32>(halfCount);
            return (t < 0.0f ? -1.0f : 1.0f) * t * t;
        };

        // WEIGHTS COME FROM THE LINE SPREAD FUNCTION, NOT FROM R(r).
        //
        // This is the one non-obvious decision in the file, and it is the one
        // the reference experiment was run to settle. The blur is SEPARABLE: a
        // 1D kernel applied along x, then along y. The obvious 1D kernel is the
        // radial profile itself, and that is what most separable subsurface
        // implementations use — but a two-pass separable blur reproduces the
        // true 2D response of a STRAIGHT EDGE only if the 1D kernel is the
        // profile's LINE spread function, its projection onto one axis. A lit
        // face is mostly straight edges: a shadow terminator, an N.L falloff,
        // the shaded side of a nose.
        //
        // Measured (experiments/skin-diffusion-reference/compare_profiles.py,
        // 17 taps, against a true 2D convolution of the profile): the
        // radial-as-1D kernel is off by up to 0.145 of a 0..1 step at a
        // terminator; the line spread function is off by 0.052. Three to twelve
        // times better across every channel and framing tested, for the same tap
        // count — it costs nothing at runtime, because both are just numbers in
        // this table.
        //
        // A tap's weight is the energy of the STRIP it stands for, a difference
        // of two SkinBurleyStripFraction values, rather than a sample of the
        // line spread function at its centre: an integral makes the weights
        // independent of where the taps happen to land, and the pointwise
        // function is singular at the origin anyway.
        const auto stripEnergy = [&](f32 loNorm, f32 hiNorm, f32 dChannel) -> f32
        {
            const f32 lo = loNorm * supportRadiusMM;
            const f32 hi = hiNorm * supportRadiusMM;
            if (!(hi > lo))
                return 0.0f;
            // S() covers BOTH sides of the axis, so an off-centre tap takes half
            // of its interval's strip energy — the other half belongs to its
            // mirror tap. The centre tap's interval straddles zero and takes its
            // whole one.
            if (lo <= 0.0f)
                return SkinBurleyStripFraction(hi, dChannel);
            return 0.5f * std::max(SkinBurleyStripFraction(hi, dChannel) -
                                       SkinBurleyStripFraction(lo, dChannel),
                                   0.0f);
        };

        glm::vec3 totals(0.0f);
        for (i32 i = -halfCount; i <= halfCount; ++i)
        {
            const f32 offset = offsetAt(i);
            const f32 prev = (i > -halfCount) ? offsetAt(i - 1) : offset;
            const f32 next = (i < halfCount) ? offsetAt(i + 1) : offset;

            // Midpoints to the neighbours, so adjacent taps partition the
            // support with no gap and no overlap. The outermost tap reaches the
            // support; the remaining tail beyond it is folded in by the
            // normalisation below rather than dropped.
            const f32 innerEdge = std::abs(0.5f * (offset + prev));
            const f32 outerEdge = std::abs(0.5f * (offset + next));
            const f32 lo = (i == 0) ? 0.0f : std::min(innerEdge, outerEdge);
            const f32 hi = (i == 0) ? std::max(innerEdge, outerEdge)
                                    : ((i == -halfCount || i == halfCount) ? 1.0f : std::max(innerEdge, outerEdge));

            const glm::vec3 weight(stripEnergy(lo, hi, d.x),
                                   stripEnergy(lo, hi, d.y),
                                   stripEnergy(lo, hi, d.z));

            kernel.Taps[static_cast<sizet>(i + halfCount)] = glm::vec4(offset, weight);
            totals += weight;
        }

        // Normalise each channel to exactly 1. Two reasons this is not left to
        // the shader: the divide would be per pixel rather than per frame, and a
        // shader that normalises by the weights it actually USED after bilateral
        // rejection renormalises the kernel per pixel, which turns a depth
        // discontinuity into a brightness change. The shader keeps the rejected
        // energy on the centre tap instead — see SkinDiffusion.glsl.
        for (u32 t = 0; t < tapCount; ++t)
        {
            glm::vec4& tap = kernel.Taps[t];
            tap.y = (totals.x > 0.0f) ? (tap.y / totals.x) : 0.0f;
            tap.z = (totals.y > 0.0f) ? (tap.z / totals.y) : 0.0f;
            tap.w = (totals.z > 0.0f) ? (tap.w / totals.z) : 0.0f;
        }

        // A channel whose energy did not reach the table at all (an authored
        // radius so small the whole profile fits inside the centre tap) would
        // now be all zeros and would blur that channel to black. Put it back on
        // the centre tap, which is what "this channel does not scatter" means.
        for (i32 c = 0; c < 3; ++c)
        {
            if (!(totals[c] > 0.0f))
                kernel.Taps[static_cast<sizet>(halfCount)][c + 1] = 1.0f;
        }

        return kernel;
    }

    f32 SkinDiffusionRadiusPixels(f32 radiusMM, f32 viewDepth, f32 projectionScaleY, f32 viewportHeight) noexcept
    {
        if (!std::isfinite(radiusMM) || !std::isfinite(viewDepth) || !std::isfinite(projectionScaleY) ||
            !std::isfinite(viewportHeight))
            return 0.0f;
        if (radiusMM <= 0.0f || viewDepth <= 0.0f || projectionScaleY <= 0.0f || viewportHeight <= 0.0f)
            return 0.0f;

        // THE ONE UNIT CONVERSION. Millimetres to world units (metres), then the
        // standard perspective projection of a length perpendicular to the view
        // direction: half the viewport in pixels, times P[1][1], over depth.
        const f32 radiusWorld = radiusMM * kSkinWorldUnitsPerMillimetre;
        const f32 pixels = radiusWorld * (0.5f * viewportHeight * projectionScaleY) / viewDepth;
        if (!std::isfinite(pixels))
            return 0.0f;
        return std::clamp(pixels, 0.0f, kMaxSkinDiffusionRadiusPixels);
    }

} // namespace OloEngine
