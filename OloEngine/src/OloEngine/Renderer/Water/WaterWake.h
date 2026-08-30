#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Water/WaterDisturbanceField.h"

#include <glm/glm.hpp>

#include <cmath>

namespace OloEngine::WaterWake
{
    // =========================================================================
    // THE WAKE-SHAPE CONTRACT (issue #968)
    //
    // #967 made the boat disturb FOAM. This makes it disturb the SURFACE: a
    // bounded height contribution plus a hull-footprint suppression, evaluable
    // at an arbitrary world XZ by BOTH the water shader and CPU physics.
    //
    // Two consumers mirror this file and neither re-derives it:
    //
    //   * include/WaterWakeCommon.glsl    - the GLSL twin, included by the
    //                                       water vertex / tess-eval stages;
    //   * WaterProbe::SampleSurfaceY      - the CPU side buoyancy floats on.
    //
    // ---- 1. Why an ANALYTIC field and not the #967 raster ------------------
    //
    // The disturbance field is a GPU-written, decaying RG16F texture. Physics
    // cannot read it without a readback, and a readback is a stall and a frame
    // of lag. So the wake SHAPE is a separate, analytic function of a small
    // record that the CPU builds each tick and both sides evaluate. Parity is
    // then achievable by construction rather than by synchronising two
    // different representations, and it is what
    // WaterWakeParityTest.CpuAndGpuAgreeOnWakeHeightAtSampledPoints can assert
    // at all.
    //
    // The two are still one wake visually: BoatWakeSystem derives the foam
    // splats and these hull records from the SAME pose history and the SAME
    // arm-offset law, so the foam sits on the ridge rather than beside it.
    //
    // ---- 2. Why the record is FLAT vec4s -----------------------------------
    //
    // The record travels to the GPU inside WaterUBO (binding 23) as a plain
    // `vec4[kHullVec4Count]` array. A flat vec4 array is the one std140 layout
    // with no padding rules to get wrong - its stride is exactly 16 bytes on
    // every implementation - and it costs ZERO new binding slots, which
    // matters: the engine has exactly one UBO slot left below
    // ShaderBindingLayout::UBO_BINDING_LIMIT and this is not what to spend it
    // on. Water is single-instance by design, so the block is uploaded once
    // per water draw regardless of its size.
    //
    // ---- 3. One validation boundary ----------------------------------------
    //
    // Every geometric decision - arm placement, amplitudes, radii, the speed
    // gate, the bounding circle - is made ONCE, on the CPU, in
    // WaterWakeSystem::SubmitHull. What reaches both evaluators is already
    // sanitised, so the mirrored math below carries `max(x, 1e-4)` guards and
    // nothing else. Same reasoning as WaterDisturbance::SplatWeight and
    // docs/agent-rules/persistent-world-space-fields.md section 7: a CPU/GPU
    // pair that SANITISES differently agrees on every value anyone tests and
    // disagrees on exactly the ones nobody does.
    //
    // ---- 4. The Kelvin half-angle is a RATIO, not a spread rate ------------
    //
    // A real ship wake's arms open at a half-angle of asin(1/3) = 19.47 deg
    // INDEPENDENT of speed - that is the classic Kelvin result, and it is what
    // makes a wake read as a wake at every throttle setting. #967's foam arms
    // used a constant lateral spread of 1.6 m/s, which gives 15 deg at 6 m/s
    // and 5 deg at 18 m/s: correct-looking at exactly one speed, and the
    // failure is a wake that visibly NARROWS as the boat accelerates, which
    // reads as a perspective effect rather than as a bug.
    //
    // The fix is that the lateral offset must be proportional to the distance
    // travelled, not to the time elapsed:
    //
    //     offset(age) = halfBeam + kKelvinTanHalfAngle * |speed| * age
    //
    // so tan(halfAngle) == kKelvinTanHalfAngle at every speed. #968 moved the
    // foam arms onto this same law so the foam and the height ridge coincide.
    //
    // ---- 5. Bounded, and band-limited ---------------------------------------
    //
    // The total height contribution is clamped to kMaxWakeHeightMetres. That is
    // not defensive tidiness: this sum composes with the FFT/Gerstner
    // displacement, and an unbounded term added to an already-choppy sea is how
    // water ends up above the deck it was added to keep water off.
    //
    // `vertexSpacing` fades a feature out as it approaches the water mesh's
    // vertex spacing, because a ridge narrower than the mesh can represent does
    // not get smaller on screen - it aliases (docs/agent-rules/
    // water-shading-nyquist.md: sub-pixel detail must be DROPPED, not
    // filtered). Physics passes 0, meaning "no mesh, no limit", which is right
    // rather than a parity hole: buoyancy samples at the hull, where the water
    // is at its finest tessellation and the factor is 1 anyway, and a floating
    // body should track the true surface rather than the renderable
    // approximation of it. The parity test drives BOTH sides at several
    // non-zero spacings precisely so the factor is covered rather than
    // sidestepped.
    // =========================================================================

    /// Hulls whose wake can be evaluated at once. Four rather than one so a
    /// second boat, an AI opponent or a tug is not a special case; each costs
    /// one bounding-circle rejection per sample when it is far away.
    inline constexpr u32 kMaxHulls = 4;

    /// Pose samples per hull along the historical trail. The arms are the
    /// polyline through these, so this is the resolution of the curve an
    /// S-turn leaves behind.
    inline constexpr u32 kMaxArmSamples = 8;

    /// vec4s of header per hull: CentreForward, Shape, Bound, Control.
    inline constexpr u32 kHullHeaderVec4 = 4;

    /// vec4s per arm sample: the starboard point and the port point.
    inline constexpr u32 kArmSampleVec4 = 2;

    inline constexpr u32 kVec4PerHull = kHullHeaderVec4 + kArmSampleVec4 * kMaxArmSamples; // 20
    inline constexpr u32 kHullVec4Count = kMaxHulls * kVec4PerHull;                        // 80

    // Offsets within one hull's vec4 block. Mirrored by the WATER_WAKE_*
    // defines in WaterWakeCommon.glsl.
    inline constexpr u32 kOffsetCentreForward = 0; ///< xy = hull centre world XZ, zw = unit forward XZ
    inline constexpr u32 kOffsetShape = 1;         ///< x = halfBeam, y = halfLength, z = bow amplitude (m), w = stern trough amplitude (m, positive magnitude)
    inline constexpr u32 kOffsetBound = 2;         ///< xy = bounding-circle centre world XZ, z = radius (m), w = unused
    inline constexpr u32 kOffsetControl = 3;       ///< x = arm sample count, y = per-hull flatten enable [0,1], z = bow reach (m), w = stern reach (m)
    inline constexpr u32 kOffsetArms = 4;          ///< [kOffsetArms + 2i] = starboard (xy pos, z amplitude, w radius), [+1] = port (xy pos)

    /// tan(19.47 deg) = 1 / (2 * sqrt(2)) - the Kelvin wake's half-angle. See section 4.
    inline constexpr f32 kKelvinTanHalfAngle = 0.35355339f;

    /// Normalised footprint radius at which the hull-flatten mask reaches zero.
    /// 1.0 is the hull's own oriented bounding rectangle, so this leaves a 35%
    /// skirt for the sea to come back up in rather than stepping at the hull
    /// edge - a step there is a visible crease along the waterline.
    inline constexpr f32 kFootprintFadeEnd = 1.35f;

    /// Falloff exponent of an arm ridge across its capsule. Higher than the
    /// foam splats' 2.0: a height ridge with a soft shoulder reads as a swell
    /// rather than as a crest, and the sharper core is what survives the
    /// band-limit fade at distance.
    inline constexpr f32 kArmSoftness = 2.5f;

    /// Hard ceiling on |wake height| in metres, after every hull is summed.
    inline constexpr f32 kMaxWakeHeightMetres = 1.5f;

    /// What one evaluation of the wake yields.
    struct Sample
    {
        /// Metres to ADD to the water surface height. Signed: the stern trough
        /// is negative.
        f32 m_Height = 0.0f;
        /// How much of the base ocean displacement to REMOVE, in [0, 1]. 1 is
        /// "flat calm here", which is what stops a crest rising through a deck.
        f32 m_Flatten = 0.0f;
    };

    // -------------------------------------------------------------------------
    // Mirrored math - every function below has a twin of the same name (camel-
    // cased, `waterWake` prefixed) in WaterWakeCommon.glsl.
    // -------------------------------------------------------------------------

    /// Smooth, compactly supported bump: 1 at u == 0, 0 at |u| >= 1, with zero
    /// slope at both ends.
    ///
    /// `(1 - u^2)^2` rather than a Gaussian or a cosine because it is exactly
    /// zero outside its support. A bump that is merely SMALL outside its
    /// support still has to be evaluated everywhere, and - worse - leaves a
    /// millimetre-scale offset across the whole ocean that reads as the water
    /// plane having moved rather than as a wake.
    [[nodiscard("the bump weight is the only effect")]]
    inline f32 Bump(f32 u) noexcept
    {
        const f32 t = 1.0f - u * u;
        return (t <= 0.0f) ? 0.0f : (t * t);
    }

    /// Amplitude scale that drops a feature of size `featureMetres` as it
    /// approaches the water mesh's `vertexSpacing`. `vertexSpacing <= 0` means
    /// "no mesh" (the CPU/physics path) and disables the limit. See section 5.
    [[nodiscard("the band-limit weight is the only effect")]]
    inline f32 BandLimit(f32 featureMetres, f32 vertexSpacing) noexcept
    {
        if (!(vertexSpacing > 0.0f))
            return 1.0f;
        return glm::clamp(2.0f * featureMetres / glm::max(vertexSpacing, 1.0e-4f) - 1.0f, 0.0f, 1.0f);
    }

    /// Lateral offset of the wake arm at `ageSeconds` behind a hull that was
    /// making `speed` m/s with half-beam `halfBeam`. The Kelvin law of section 4.
    [[nodiscard("the arm offset is the only effect")]]
    inline f32 ArmOffset(f32 halfBeam, f32 speed, f32 ageSeconds) noexcept
    {
        return halfBeam + kKelvinTanHalfAngle * glm::abs(speed) * glm::max(ageSeconds, 0.0f);
    }

    /// Evaluate the whole wake at absolute world XZ.
    ///
    /// `hulls` is `kHullVec4Count` vec4s laid out as documented above;
    /// `hullCount` is how many of them are live (the rest are not read).
    /// `heightScale` is the global multiplier - 0 disables the height
    /// contribution AND the flatten, so a scene that publishes the disabled
    /// state cannot leave a stale hull footprint pressed into the sea.
    ///
    /// `flattenStrength` is the scene setting, passed in rather than baked into
    /// the records on purpose: the records are built during the PHYSICS tick and
    /// the settings are published during render submission, so a record that
    /// carried the strength would carry the PREVIOUS frame's — invisible while
    /// the value is constant, and a one-frame flash of the wrong sea the first
    /// time anyone drags the slider. The per-hull `control.y` remains a plain
    /// enable, which a hull that should not suppress the sea at all (airborne,
    /// fully submerged) can drop to zero without touching the scene setting.
    [[nodiscard("the wake sample is the only effect")]]
    inline Sample Evaluate(const glm::vec4* hulls, u32 hullCount, f32 heightScale, f32 flattenStrength,
                           glm::vec2 worldXZ, f32 vertexSpacing) noexcept
    {
        Sample result{};
        if (hulls == nullptr || hullCount == 0u || !(heightScale > 0.0f))
            return result;

        const u32 live = glm::min(hullCount, kMaxHulls);
        f32 height = 0.0f;
        f32 flatten = 0.0f;

        for (u32 h = 0; h < live; ++h)
        {
            const u32 base = h * kVec4PerHull;

            // Bounding-circle rejection first. The wake is a local feature on
            // an ocean, so this is the branch that runs for almost every
            // sample - putting anything else ahead of it makes the whole sea
            // pay for the boat.
            const glm::vec4 bound = hulls[base + kOffsetBound];
            const glm::vec2 toBound = worldXZ - glm::vec2(bound.x, bound.y);
            if (glm::dot(toBound, toBound) > bound.z * bound.z)
                continue;

            const glm::vec4 centreForward = hulls[base + kOffsetCentreForward];
            const glm::vec4 shape = hulls[base + kOffsetShape];
            const glm::vec4 control = hulls[base + kOffsetControl];

            const glm::vec2 centre(centreForward.x, centreForward.y);
            const glm::vec2 forward(centreForward.z, centreForward.w);
            // Starboard is forward rotated -90 deg about +Y in a right-handed
            // frame. Getting this backwards mirrors the wake, which on a
            // symmetric hull looks entirely correct - see BoatSystem.cpp and
            // issue #897.
            const glm::vec2 starboard(-forward.y, forward.x);

            const glm::vec2 rel = worldXZ - centre;
            const f32 lateral = glm::dot(rel, starboard);
            const f32 along = glm::dot(rel, forward);

            const f32 halfBeam = glm::max(shape.x, 1.0e-4f);
            const f32 halfLength = glm::max(shape.y, 1.0e-4f);

            // --- 1. Hull footprint suppression ----------------------------
            {
                const f32 r = glm::max(glm::abs(lateral) / halfBeam, glm::abs(along) / halfLength);
                // 1 inside the hull rectangle, easing to 0 at kFootprintFadeEnd.
                const f32 t = glm::clamp((kFootprintFadeEnd - r) / glm::max(kFootprintFadeEnd - 1.0f, 1.0e-4f),
                                         0.0f, 1.0f);
                const f32 mask = t * t * (3.0f - 2.0f * t); // smoothstep
                flatten = glm::max(flatten, mask * glm::clamp(control.y, 0.0f, 1.0f) *
                                                glm::clamp(flattenStrength, 0.0f, 1.0f));
            }

            // --- 2. Bow bump ----------------------------------------------
            // Centred ON the bow (along == +halfLength) so half of it lifts the
            // water over the forward hull and half stands ahead of it, which is
            // where a displacement hull actually piles water up.
            if (shape.z > 0.0f)
            {
                const f32 reach = glm::max(control.z, 1.0e-4f);
                const f32 lon = (along - halfLength) / reach;
                const f32 lat = lateral / (halfBeam * 1.6f);
                height += shape.z * Bump(lon) * Bump(lat) * BandLimit(reach, vertexSpacing);
            }

            // --- 3. Stern trough ------------------------------------------
            // Centred one reach BEHIND the stern, not at it: a bump centred on
            // the stern is symmetric, so it would dig the same trough forward
            // under the hull and cancel the bow bump.
            if (shape.w > 0.0f)
            {
                const f32 reach = glm::max(control.w, 1.0e-4f);
                const f32 lon = (along + halfLength + reach) / reach;
                const f32 lat = lateral / (halfBeam * 1.4f);
                height -= shape.w * Bump(lon) * Bump(lat) * BandLimit(reach, vertexSpacing);
            }

            // --- 4. The diverging arms ------------------------------------
            // A polyline per side through the historical arm points, each
            // segment a capsule with the SAME falloff the foam splats use
            // (WaterDisturbance::SplatWeight) so the ridge and the foam that
            // rides it cannot drift apart.
            //
            // `max` across segments rather than a sum: consecutive capsules
            // overlap at their shared endpoint, and summing there would put a
            // bright bead at every sample - the arm would read as a string of
            // pearls, which is a look, just not this one.
            {
                const i32 samples = glm::clamp(static_cast<i32>(control.x), 0, static_cast<i32>(kMaxArmSamples));
                f32 arm = 0.0f;
                for (i32 i = 0; i + 1 < samples; ++i)
                {
                    const u32 s0 = base + kOffsetArms + kArmSampleVec4 * static_cast<u32>(i);
                    const u32 s1 = s0 + kArmSampleVec4;

                    const glm::vec4 starboard0 = hulls[s0];
                    const glm::vec4 starboard1 = hulls[s1];
                    const glm::vec4 port0 = hulls[s0 + 1];
                    const glm::vec4 port1 = hulls[s1 + 1];

                    // Keyed on the NEARER end (index i, the younger sample), so
                    // a segment never reads stronger than the one ahead of it
                    // and the ridge decays monotonically down the arm.
                    const f32 radius = glm::max(starboard0.w, 1.0e-4f);
                    const f32 amplitude = starboard0.z * BandLimit(radius, vertexSpacing);
                    if (!(amplitude > 0.0f))
                        continue;

                    const f32 weightStarboard = WaterDisturbance::SplatWeight(
                        worldXZ, glm::vec2(starboard0.x, starboard0.y), glm::vec2(starboard1.x, starboard1.y),
                        radius, kArmSoftness);
                    const f32 weightPort = WaterDisturbance::SplatWeight(
                        worldXZ, glm::vec2(port0.x, port0.y), glm::vec2(port1.x, port1.y),
                        radius, kArmSoftness);
                    arm = glm::max(arm, amplitude * glm::max(weightStarboard, weightPort));
                }
                height += arm;
            }
        }

        height *= heightScale;
        result.m_Height = glm::clamp(height, -kMaxWakeHeightMetres, kMaxWakeHeightMetres);
        result.m_Flatten = glm::clamp(flatten, 0.0f, 1.0f);
        return result;
    }
} // namespace OloEngine::WaterWake
