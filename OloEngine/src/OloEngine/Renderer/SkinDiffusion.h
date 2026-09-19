#pragma once

// =============================================================================
// SkinDiffusion.h — the scattering maths #1241 adds on top of #1231's split.
//
// EVERY PHYSICAL DECISION IN THIS FEATURE IS MADE HERE, ON THE CPU. The shader
// that does the blurring is a weighted sum with a bilateral guard and knows
// nothing about diffusion profiles, transport albedo or millimetres: it is
// handed a table of (offset, per-channel weight) and a screen-space radius. That
// split is deliberate — a diffusion profile evaluated in GLSL is a thing no test
// can look at, and the one number this feature is most likely to get wrong is a
// UNIT, which is exactly what a CPU test can pin.
//
// THE UNIT CHAIN, STATED ONCE.
//
//   ScatterRadiusMM   authored, MILLIMETRES (SkinProfile.h)
//   -> d              Burley scaling, MILLIMETRES
//   -> maxRadiusMM    the 99.5% support of the profile, MILLIMETRES
//   -> world units    maxRadiusMM * kSkinWorldUnitsPerMillimetre
//   -> pixels         world * (0.5 * viewportHeight * P[1][1]) / viewDepth
//
// `ThicknessScale` is NOT in that chain. It converts a MATERIAL's authored
// thickness (metres, per glTF KHR_materials_volume) into the millimetre space
// the radii already live in; it is the transmission knob, not a second opinion
// about how long a millimetre is. Folding it into the radius would make a
// profile that exaggerates transmission quietly shrink its own blur, and its
// lower bound is 0 (SkinProfile.h), so the reciprocal that would need is not
// even defined. The engine's metre convention is the whole of the conversion,
// and SkinDiffusionTest pins it.
//
// See docs/guides/skin-diffusion.md for the reference comparison that chose
// Burley over the alternatives, and for the measured error of the separable
// approximation this kernel uses.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/SkinProfile.h"

#include <glm/glm.hpp>

#include <array>
#include <string_view>

namespace OloEngine
{

    // -------------------------------------------------------------------------
    // Units
    // -------------------------------------------------------------------------

    // One world unit is one metre — the convention the whole engine and glTF
    // share. Named rather than spelled `0.001f` at the two call sites because a
    // bare thousand in a scattering expression is indistinguishable from a
    // tuning constant, and this one is not tunable.
    inline constexpr f32 kSkinMillimetresPerWorldUnit = 1000.0f;
    inline constexpr f32 kSkinWorldUnitsPerMillimetre = 1.0f / kSkinMillimetresPerWorldUnit;

    // The fraction of the profile's energy the kernel's outermost tap reaches.
    // Not 1: Burley's profile has infinite support, so a "full" radius does not
    // exist and asking for one gives a kernel that spends most of its taps on
    // nothing. 99.5% is where the remaining tail is below the quantisation of an
    // RGBA16F target for any plausible radiance.
    //
    // IT IS 99.5% OF THE FIT, NOT OF THE TRANSPORT, AND THAT GAP WAS MEASURED
    // AND DELIBERATELY LEFT (#1361). Above a diffuse albedo of about 0.7 the
    // Burley fit runs narrow, so the radius it calls 99.5% holds less of the
    // real transport: about 97% at an authored 0.85 — the default ScatterColor's
    // red channel — and 93% at 0.95.
    //
    // RAISING IT WAS THE OBVIOUS FIX AND IT IS THE WRONG ONE. Covering 99.5% of
    // the TRANSPORT at 0.85 needs 27.05 mm of support where the fit asks for
    // 15.02 mm: a 1.80x widening. The tap count does not widen with it, so every
    // tap offset scales by 1.80 and the kernel simply gets coarser. The centre
    // tap — the part of the profile the pass does not resolve at all — goes from
    // standing for 0.23 mm of the surface to 0.42 mm, and its share of the
    // profile rises by half again, from 0.143 to 0.217: another seven percent of
    // the profile stops being blurred at all.
    //
    // WHAT IT BUYS IS NOTHING YOU CAN SEE, and that is the part worth writing
    // down. Judged on the image the tail actually shows up in — a bright small
    // feature against dark skin, not a terminator — the widened support moves
    // the halo by 0.004 OF THE FEATURE'S OWN ENERGY at 0.85, against an error of
    // 0.056 that it was meant to fix. The support radius is not where the
    // missing energy is: the outer taps draw their weight from the same narrow
    // fit, so moving them out moves a near-zero weight out with them. Only a
    // wider PROFILE would put energy there, and the fit is the profile.
    //
    // AND THE FIT IS NOT THE LIMITING ERROR EITHER. The pass is SEPARABLE, and
    // at a bright small feature that is the dominant approximation by some way.
    // Replace the fit and the tap budget with the WALK's own profile, sampled
    // 600 entries to a side across the walk's full support, and the result sits
    // 0.079 from transport at 0.85 — where this kernel, narrow fit and truncated
    // support and 17 taps and all, sits 0.056. One error term measured ALONE
    // exceeds the total of every term together, which is what "dominant" means
    // here. Making the profile more accurate moves the image AWAY from transport
    // at the one albedo that ships. That is a coincidence of two errors with
    // opposite signs rather than a design, so it is not something to preserve;
    // it is the reason neither lever is worth pulling.
    //
    // SkinDiffusionReference in NonlocalTransportReferenceTest.cpp holds all
    // three measurements, against a Monte Carlo searchlight walk.
    inline constexpr f32 kSkinDiffusionSupportFraction = 0.995f;

    // -------------------------------------------------------------------------
    // Quality tiers
    // -------------------------------------------------------------------------

    // Tap count per axis, including the centre tap. The pass is separable, so a
    // tier costs 2N texture fetches per skin pixel, not N^2.
    //
    // Append, never renumber: the tier is a saved renderer setting.
    enum class SkinDiffusionQuality : u8
    {
        Low = 0,    ///< 9 taps/axis. Visible banding on a wide radius; cheapest.
        Medium = 1, ///< 17 taps/axis. The default, and what the reference comparison was run at.
        High = 2,   ///< 25 taps/axis.

        Count
    };

    inline constexpr i32 kSkinDiffusionQualityCount = static_cast<i32>(SkinDiffusionQuality::Count);

    [[nodiscard]] constexpr std::string_view ToString(SkinDiffusionQuality quality)
    {
        switch (quality)
        {
            case SkinDiffusionQuality::Low:
                return "Low";
            case SkinDiffusionQuality::Medium:
                return "Medium";
            case SkinDiffusionQuality::High:
                return "High";
            case SkinDiffusionQuality::Count:
                break;
        }
        return "Medium";
    }

    [[nodiscard]] inline constexpr bool IsValidSkinDiffusionQuality(i32 value) noexcept
    {
        return value >= 0 && value < kSkinDiffusionQualityCount;
    }

    // The largest tier's tap count. The GPU-side table is sized to this and a
    // lower tier simply fills fewer entries, so changing tier never resizes a
    // uniform block — which would mean a shader recompile mid-session.
    inline constexpr u32 kMaxSkinDiffusionTaps = 25;

    [[nodiscard]] inline constexpr u32 GetSkinDiffusionTapCount(SkinDiffusionQuality quality) noexcept
    {
        switch (quality)
        {
            case SkinDiffusionQuality::Low:
                return 9;
            case SkinDiffusionQuality::Medium:
                return 17;
            case SkinDiffusionQuality::High:
                return kMaxSkinDiffusionTaps;
            case SkinDiffusionQuality::Count:
                break;
        }
        return 17;
    }

    // Every tier is odd — the centre tap is the pixel itself, and an even count
    // would have no pixel at offset 0, which turns an unblurred region into a
    // half-texel shift rather than an identity.
    static_assert(GetSkinDiffusionTapCount(SkinDiffusionQuality::Low) % 2 == 1);
    static_assert(GetSkinDiffusionTapCount(SkinDiffusionQuality::Medium) % 2 == 1);
    static_assert(GetSkinDiffusionTapCount(SkinDiffusionQuality::High) % 2 == 1);
    static_assert(GetSkinDiffusionTapCount(SkinDiffusionQuality::High) <= kMaxSkinDiffusionTaps);

    // -------------------------------------------------------------------------
    // Burley normalized diffusion
    // -------------------------------------------------------------------------

    // Christensen-Burley's fit of the profile's scaling to the transport albedo,
    // "searchlight configuration" (Christensen & Burley 2015, eq. 6). `albedo`
    // is one channel of SkinProfileParameters::ScatterColor, unitless [0,1].
    //
    // Unitless. It divides the authored mean free path to give `d`, so a low
    // albedo (blue, which is absorbed) yields a large s and a short d.
    [[nodiscard]] f32 SkinBurleyShapeFromAlbedo(f32 albedo) noexcept;

    // The per-channel scaling `d`, MILLIMETRES, from an authored profile.
    // Guaranteed finite and strictly positive: the profile's own bounds keep
    // ScatterRadiusMM at or above kMinSkinScatterRadiusMM and the shape fit is
    // bounded below on [0,1], so no caller has to test the result.
    //
    // The parameters must already be sanitized — every path into this function
    // goes through SkinProfileParameters::Sanitize, and the one assertion this
    // header makes is that it did.
    [[nodiscard]] glm::vec3 SkinBurleyScalingMM(const SkinProfileParameters& parameters) noexcept;

    // R(r): the radial diffusion profile, per unit AREA, integrating to exactly
    // 1 over the plane. `r` and `d` in millimetres; the result is 1/mm^2.
    //
    // r == 0 is a true singularity of the analytic form (the 1/r), and is
    // returned as 0 rather than as an infinity: the kernel below never samples a
    // point, it integrates an annulus, and the annulus at the origin has zero
    // area. A caller that wants the centre weight asks for the CDF.
    [[nodiscard]] f32 SkinBurleyProfile(f32 r, f32 d) noexcept;

    // The fraction of the profile's energy inside radius `r`. Analytic:
    //   CDF(r) = 1 - 0.25 e^{-r/d} - 0.75 e^{-r/3d}
    // Unitless [0,1]; `r` and `d` in millimetres.
    [[nodiscard]] f32 SkinBurleyCdf(f32 r, f32 d) noexcept;

    // The radius, MILLIMETRES, containing `fraction` of the energy — the inverse
    // of SkinBurleyCdf, by bisection (the CDF has no closed-form inverse). A
    // fraction at or above 1 is clamped below it: the true answer is infinite.
    [[nodiscard]] f32 SkinBurleyRadiusForFraction(f32 fraction, f32 d) noexcept;

    // S(a): the fraction of the profile's energy lying in the VERTICAL STRIP
    // |x| <= a. Unitless [0,1]; `a` and `d` in millimetres.
    //
    // This — not the radial CDF — is what a separable blur's 1D tap weights are
    // made of, and it is the whole reason this file is longer than a Gaussian.
    // A two-pass separable blur reproduces the true 2D response of a STRAIGHT
    // EDGE only if its 1D kernel is the profile's LINE spread function, the
    // projection of the 2D profile onto one axis; S is that function's integral,
    // so the energy of a tap's strip is a difference of two S values.
    //
    // It is computed as a STRIP rather than as a pointwise line spread function
    // because the pointwise one is singular: R(r) has a 1/r pole and its line
    // integral a logarithmic one, so sampling near x = 0 measures the quadrature
    // step rather than the profile. In polar form the pole cancels against the
    // Jacobian and what is left is a bounded 1D integral:
    //
    //   S(a) = CDF(a) + (1 / 2*pi*d) * integral_a^inf asin(a/r) (e^-r/d + e^-r/3d) dr
    //
    // the disc of radius a, which lies wholly inside the strip, plus the angular
    // fraction of every annulus beyond it that still does.
    //
    // SkinDiffusionTest checks this against a direct 2D integration of the
    // profile, which is the only way to be sure the polar rearrangement is the
    // same number.
    [[nodiscard]] f32 SkinBurleyStripFraction(f32 a, f32 d) noexcept;

    // The radius, MILLIMETRES, that the kernel's outermost tap must reach for
    // this profile: the widest channel's kSkinDiffusionSupportFraction support.
    // This is the one number that becomes a screen-space pixel radius, so it is
    // also the one the projection maths is pinned against.
    [[nodiscard]] f32 SkinDiffusionSupportRadiusMM(const SkinProfileParameters& parameters) noexcept;

    // -------------------------------------------------------------------------
    // The kernel the GPU is handed
    // -------------------------------------------------------------------------

    // A separable, per-channel-weighted tap table.
    //
    // WHY ONE TAP SET AND THREE WEIGHTS RATHER THAN THREE TAP SETS. Red scatters
    // roughly three times as far as blue, so the physically obvious thing is a
    // different set of offsets per channel — and it costs three times the
    // fetches. Sharing one set of offsets sized to the WIDEST channel and giving
    // each channel its own weight at each offset reproduces the same per-channel
    // profile from a single fetch: blue's weights simply fall to zero long
    // before the outer taps. The colour response — red bleeding out of a shadow
    // terminator while blue stays put — is carried entirely by the weights.
    //
    // OFFSETS ARE NORMALISED, [-1, 1], in units of SupportRadiusMM. That is what
    // lets the screen-space radius be a per-PIXEL quantity (it depends on depth)
    // while the table stays per-PROFILE and is uploaded once per frame.
    struct SkinDiffusionKernel
    {
        // The radius the offsets are expressed in, MILLIMETRES.
        f32 SupportRadiusMM = 0.0f;
        u32 TapCount = 0;
        // x = normalised offset in [-1, 1]; yzw = per-channel weight, each
        // channel summing to exactly 1 across TapCount entries so the kernel is
        // energy-preserving by construction rather than by a normalising divide
        // in the shader.
        std::array<glm::vec4, kMaxSkinDiffusionTaps> Taps{};

        // A kernel that leaves its input alone: one tap at offset 0 with weight
        // 1. What a non-skin slot, a profile authored against an older transport
        // version, or a degenerate radius produces — an identity is a correct
        // answer, an empty table is a divide by zero.
        [[nodiscard]] static SkinDiffusionKernel Identity() noexcept;

        [[nodiscard]] bool IsIdentity() const noexcept;
    };

    // Build the kernel for an authored profile.
    //
    // The parameters must be sanitized (every caller routes through
    // SkinProfileTable, which only hands out sanitized parameters). A profile
    // whose EvaluationModel is not ScreenSpaceDiffusion returns Identity(): the
    // version branch lives here as well as in the shader, so a version 0 profile
    // cannot be blurred by a renderer setting.
    [[nodiscard]] SkinDiffusionKernel BuildSkinDiffusionKernel(const SkinProfileParameters& parameters,
                                                               SkinDiffusionQuality quality);

    // -------------------------------------------------------------------------
    // World units -> pixels
    // -------------------------------------------------------------------------

    // The screen-space radius, in PIXELS, that `radiusMM` subtends at
    // `viewDepth` metres in front of the camera.
    //
    //   projectionScaleY = P[1][1] of the projection matrix (2 * near / height
    //                      for a frustum; cot(fovY/2) for a symmetric one)
    //   viewportHeight   = the height in pixels of the target being blurred,
    //                      which under dynamic resolution is NOT the swapchain's
    //
    // Both of those are arguments rather than globals because that is what makes
    // the third acceptance criterion testable: a change of resolution or of FOV
    // must move this number, and a test can say by how much.
    //
    // THIS FUNCTION IS THE SPECIFICATION, NOT THE RUNTIME PATH. The radius is a
    // PER-PIXEL quantity — it depends on that pixel's depth — so the expression
    // lives in SkinDiffusion.glsl, where the depth is. This is the same
    // expression on the CPU, and SkinDiffusionTest pins it as ratios under a
    // change of depth, resolution, field of view and authored radius. A shader
    // expression no test can reach is exactly how a unit slip survives a review,
    // and a mirror a test DOES reach is the cheapest defence against one; the
    // pair is the same arrangement the GPU-mirror structs in
    // cpp-coding-quality.md §13 use, for the same reason.
    //
    // Returns 0 for a non-positive depth (a pixel behind the eye) rather than a
    // negative or infinite radius.
    [[nodiscard]] f32 SkinDiffusionRadiusPixels(f32 radiusMM, f32 viewDepth, f32 projectionScaleY,
                                                f32 viewportHeight) noexcept;

    // The hard ceiling on that radius, in pixels of the blurred target.
    //
    // A head one centimetre from the near plane subtends a radius of hundreds of
    // pixels, at which point the separable pass is both ruinous and wrong (the
    // surface is no longer locally flat over the kernel's footprint). Clamping
    // is the "bounded artifacts" half of the fourth acceptance criterion: the
    // failure mode becomes "the blur stops widening", which reads as a slightly
    // firm close-up, instead of "the frame time triples and the face smears".
    inline constexpr f32 kMaxSkinDiffusionRadiusPixels = 64.0f;

    // Below this the kernel cannot express anything a bilinear fetch does not
    // already do, and the pass is skipped for the pixel. Half a texel.
    inline constexpr f32 kMinSkinDiffusionRadiusPixels = 0.5f;

} // namespace OloEngine
