#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/GPUScene/GPUSceneTypes.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <glm/glm.hpp>

namespace OloEngine
{
    enum class SurfaceHistoryFlag : u32
    {
        None = 0,
        Reactive = 1u << 0u,
        Disoccluded = 1u << 1u,
        HasHitDistance = 1u << 2u,
    };

    [[nodiscard]] constexpr auto operator|(SurfaceHistoryFlag lhs, SurfaceHistoryFlag rhs) -> SurfaceHistoryFlag
    {
        return static_cast<SurfaceHistoryFlag>(std::to_underlying(lhs) | std::to_underlying(rhs));
    }

    [[nodiscard]] constexpr bool HasSurfaceHistoryFlag(SurfaceHistoryFlag flags, SurfaceHistoryFlag flag)
    {
        return (std::to_underlying(flags) & std::to_underlying(flag)) != 0u;
    }

    // Backend-neutral decoded form of the compact per-pixel surface record.
    // The three identities use GPU Scene's canonical (slot, generation)
    // currency. Primitive names the GPUSceneGeometry/submesh record; a future
    // ray hit may additionally supply a triangle-local primitive index.
    struct SurfaceHistoryRecord
    {
        f32 LinearDepth = 0.0f;
        glm::vec3 GeometricNormal{ 0.0f, 0.0f, 1.0f };
        glm::vec3 ShadingNormal{ 0.0f, 0.0f, 1.0f };
        f32 Roughness = 1.0f;
        u32 MaterialClass = 0u;
        // Fraction of this pixel the subject occupies, in [0,1]. An opaque
        // surface leaves it at 1 and is unaffected by every coverage test
        // below; the channel exists for the subjects that do NOT fill their
        // pixel — hair strands, foliage leaves, anything alpha-tested or
        // stochastically composited. See TemporalReactivity for why a
        // coverage CHANGE is a distinct cause from surface motion.
        f32 Coverage = 1.0f;
        // Position along the material's continuous profile axis, whatever
        // that axis is for the material class: subsurface profile blend for
        // skin, leaf translucency profile for foliage. Distinct from
        // MaterialClass and the Material handle, which are IDENTITIES and
        // change discretely; this one slides, and a slide is what the
        // discrete tests cannot see.
        //
        // NEVER PUT A PROFILE INDEX HERE. EvaluateTemporalReactivity takes
        // |current - previous| and ramps it, which is meaningful only for a
        // quantity whose DIFFERENCE is a magnitude. Encode a profile id and a
        // 1 -> 2 switch reads as a small slide while 1 -> 6 reads as a large
        // one, which is noise dressed as a signal. A profile IDENTITY change
        // is discrete and already belongs to MaterialMismatch, which hard-
        // rejects rather than grading.
        f32 MaterialProfile = 0.0f;
        glm::vec2 Motion{ 0.0f };
        GPUSceneHandle Instance{};
        GPUSceneHandle Primitive{};
        GPUSceneHandle Material{};
        SurfaceHistoryFlag Flags = SurfaceHistoryFlag::None;
        f32 HitDistance = 0.0f;
        u32 PrimitiveLocalIndex = GPUSceneHandle::InvalidIndex;
    };

    enum class SurfaceHistoryRejection : u32
    {
        None = 0,
        NoHistory = 1u << 0u,
        OffScreen = 1u << 1u,
        NonFinite = 1u << 2u,
        DepthMismatch = 1u << 3u,
        GeometricNormalMismatch = 1u << 4u,
        ShadingNormalMismatch = 1u << 5u,
        InstanceMismatch = 1u << 6u,
        PrimitiveMismatch = 1u << 7u,
        MaterialMismatch = 1u << 8u,
        RoughnessMismatch = 1u << 9u,
        MotionMismatch = 1u << 10u,
        Reactive = 1u << 11u,
        Disoccluded = 1u << 12u,
        HitDistanceMismatch = 1u << 13u,
        IdentityUnavailable = 1u << 14u,
        CoverageMismatch = 1u << 15u,
    };

    struct SurfaceHistoryValidity
    {
        SurfaceHistoryRejection Rejections = SurfaceHistoryRejection::None;
        glm::vec2 ReprojectedUV{ 0.0f };

        [[nodiscard]] constexpr bool Accepted() const
        {
            return Rejections == SurfaceHistoryRejection::None;
        }

        [[nodiscard]] constexpr bool Has(SurfaceHistoryRejection reason) const
        {
            return (std::to_underlying(Rejections) & std::to_underlying(reason)) != 0u;
        }
    };

    struct SurfaceHistoryValiditySettings
    {
        bool HistoryAvailable = true;
        bool TestGeometricNormal = true;
        bool TestShadingNormal = true;
        bool TestInstance = true;
        bool TestPrimitive = true;
        bool TestMaterial = true;
        bool TestRoughness = true;
        bool TestMotion = true;
        bool TestHitDistance = true;
        // Coverage is tested by default and costs an all-opaque caller
        // nothing: the record defaults to 1.0 at both ends, so the difference
        // is zero unless someone actually fills the channel.
        bool TestCoverage = true;
        f32 RelativeDepthThreshold = 0.05f;
        f32 GeometricNormalCosineThreshold = 0.85f;
        f32 ShadingNormalCosineThreshold = 0.75f;
        f32 RoughnessThreshold = 0.15f;
        f32 MotionThresholdPixels = 64.0f;
        f32 RelativeHitDistanceThreshold = 0.1f;
        // A coverage swing this large is a disocclusion-grade event: the
        // strand or leaf that was here is substantially gone.
        //
        // THIS IS NOT REDUNDANT WITH TemporalReactivity, although the
        // thresholds look like it — 0.5 here sits ABOVE the 0.35 at which
        // the reactive coverage term already saturates, so this test can
        // never be what decides the blend WEIGHT. The two have different
        // consequences, which is the whole point:
        //
        //   * a saturated reactive term drives confidence to 0, so no
        //     history is blended in this frame — but the accumulator keeps
        //     its HistoryLength and resumes at its converged rate;
        //   * a rejection makes the history invalid, so
        //     AccumulateTemporalMoments resets HistoryLength to 1 and
        //     reseeds the moments from the current frame.
        //
        // So a coverage change in [0.35, 0.5] says "ignore the history this
        // frame", and one past 0.5 says "the accumulator is describing a
        // surface that is gone; start over". Collapsing them would make a
        // brief thinning throw away a long convergence.
        // SurfaceHistoryCoverage.RejectionAndSaturatedReactivityDifferInTheirConsequence
        // pins the distinction.
        f32 CoverageRejectThreshold = 0.5f;
        glm::vec2 PixelSize{ 1.0f };
    };

    namespace SurfaceHistoryDetail
    {
        inline void AddRejection(SurfaceHistoryValidity& validity, SurfaceHistoryRejection reason)
        {
            validity.Rejections = static_cast<SurfaceHistoryRejection>(
                std::to_underlying(validity.Rejections) | std::to_underlying(reason));
        }

        [[nodiscard]] inline bool IsFinite(const glm::vec2& value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y);
        }

        [[nodiscard]] inline bool IsFinite(const glm::vec3& value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        [[nodiscard]] inline f32 NormalCosine(const glm::vec3& lhs, const glm::vec3& rhs)
        {
            const f32 lhsLengthSquared = glm::dot(lhs, lhs);
            const f32 rhsLengthSquared = glm::dot(rhs, rhs);
            if (!std::isfinite(lhsLengthSquared) || !std::isfinite(rhsLengthSquared) ||
                lhsLengthSquared <= 1.0e-12f || rhsLengthSquared <= 1.0e-12f)
            {
                return -1.0f;
            }
            return glm::dot(lhs, rhs) / std::sqrt(lhsLengthSquared * rhsLengthSquared);
        }

        [[nodiscard]] inline bool RelativeDifferenceExceeds(f32 lhs, f32 rhs, f32 threshold)
        {
            const f32 denominator = std::max(std::max(std::abs(lhs), std::abs(rhs)), 1.0e-4f);
            return std::abs(lhs - rhs) / denominator > threshold;
        }
    } // namespace SurfaceHistoryDetail

    [[nodiscard]] inline SurfaceHistoryValidity EvaluateSurfaceHistory(
        const SurfaceHistoryRecord& current,
        const SurfaceHistoryRecord& previous,
        const glm::vec2& reprojectedUV,
        const SurfaceHistoryValiditySettings& settings)
    {
        SurfaceHistoryValidity result{ .ReprojectedUV = reprojectedUV };
        const auto reject = [&result](SurfaceHistoryRejection reason)
        { SurfaceHistoryDetail::AddRejection(result, reason); };

        if (!settings.HistoryAvailable)
            reject(SurfaceHistoryRejection::NoHistory);
        if (!SurfaceHistoryDetail::IsFinite(reprojectedUV) || reprojectedUV.x < 0.0f || reprojectedUV.x > 1.0f ||
            reprojectedUV.y < 0.0f || reprojectedUV.y > 1.0f)
        {
            reject(SurfaceHistoryRejection::OffScreen);
        }

        const bool currentHasHitDistance = HasSurfaceHistoryFlag(current.Flags, SurfaceHistoryFlag::HasHitDistance);
        const bool previousHasHitDistance = HasSurfaceHistoryFlag(previous.Flags, SurfaceHistoryFlag::HasHitDistance);
        const bool finite =
            std::isfinite(current.LinearDepth) && std::isfinite(previous.LinearDepth) &&
            (!settings.TestGeometricNormal ||
             (SurfaceHistoryDetail::IsFinite(current.GeometricNormal) &&
              SurfaceHistoryDetail::IsFinite(previous.GeometricNormal))) &&
            (!settings.TestShadingNormal ||
             (SurfaceHistoryDetail::IsFinite(current.ShadingNormal) &&
              SurfaceHistoryDetail::IsFinite(previous.ShadingNormal))) &&
            (!settings.TestRoughness ||
             (std::isfinite(current.Roughness) && std::isfinite(previous.Roughness))) &&
            (!settings.TestMotion || SurfaceHistoryDetail::IsFinite(current.Motion)) &&
            (!settings.TestCoverage ||
             (std::isfinite(current.Coverage) && std::isfinite(previous.Coverage))) &&
            (!settings.TestHitDistance ||
             ((!currentHasHitDistance || std::isfinite(current.HitDistance)) &&
              (!previousHasHitDistance || std::isfinite(previous.HitDistance))));
        if (!finite)
            reject(SurfaceHistoryRejection::NonFinite);

        if (SurfaceHistoryDetail::RelativeDifferenceExceeds(
                current.LinearDepth, previous.LinearDepth, settings.RelativeDepthThreshold))
        {
            reject(SurfaceHistoryRejection::DepthMismatch);
        }
        if (settings.TestGeometricNormal &&
            SurfaceHistoryDetail::NormalCosine(current.GeometricNormal, previous.GeometricNormal) <
                settings.GeometricNormalCosineThreshold)
        {
            reject(SurfaceHistoryRejection::GeometricNormalMismatch);
        }
        if (settings.TestShadingNormal &&
            SurfaceHistoryDetail::NormalCosine(current.ShadingNormal, previous.ShadingNormal) <
                settings.ShadingNormalCosineThreshold)
        {
            reject(SurfaceHistoryRejection::ShadingNormalMismatch);
        }

        const bool testedIdentityUnavailable =
            (settings.TestInstance && (!current.Instance.IsValid() || !previous.Instance.IsValid())) ||
            (settings.TestPrimitive && (!current.Primitive.IsValid() || !previous.Primitive.IsValid())) ||
            (settings.TestMaterial && (!current.Material.IsValid() || !previous.Material.IsValid()));
        if (testedIdentityUnavailable)
            reject(SurfaceHistoryRejection::IdentityUnavailable);
        if (settings.TestInstance && current.Instance != previous.Instance)
            reject(SurfaceHistoryRejection::InstanceMismatch);
        if (settings.TestPrimitive &&
            (current.Primitive != previous.Primitive || current.PrimitiveLocalIndex != previous.PrimitiveLocalIndex))
        {
            reject(SurfaceHistoryRejection::PrimitiveMismatch);
        }
        if (settings.TestMaterial &&
            (current.Material != previous.Material || current.MaterialClass != previous.MaterialClass))
        {
            reject(SurfaceHistoryRejection::MaterialMismatch);
        }
        if (settings.TestRoughness && std::abs(current.Roughness - previous.Roughness) > settings.RoughnessThreshold)
            reject(SurfaceHistoryRejection::RoughnessMismatch);
        if (settings.TestCoverage &&
            std::abs(current.Coverage - previous.Coverage) > settings.CoverageRejectThreshold)
        {
            reject(SurfaceHistoryRejection::CoverageMismatch);
        }

        const glm::vec2 motionPixels = current.Motion / glm::max(settings.PixelSize, glm::vec2(1.0e-8f));
        if (settings.TestMotion && glm::dot(motionPixels, motionPixels) >
                                       settings.MotionThresholdPixels * settings.MotionThresholdPixels)
        {
            reject(SurfaceHistoryRejection::MotionMismatch);
        }
        if (HasSurfaceHistoryFlag(current.Flags, SurfaceHistoryFlag::Reactive))
            reject(SurfaceHistoryRejection::Reactive);
        if (HasSurfaceHistoryFlag(current.Flags, SurfaceHistoryFlag::Disoccluded))
            reject(SurfaceHistoryRejection::Disoccluded);

        if (settings.TestHitDistance && currentHasHitDistance != previousHasHitDistance)
            reject(SurfaceHistoryRejection::HitDistanceMismatch);
        else if (settings.TestHitDistance && currentHasHitDistance &&
                 SurfaceHistoryDetail::RelativeDifferenceExceeds(
                     current.HitDistance, previous.HitDistance, settings.RelativeHitDistanceThreshold))
        {
            reject(SurfaceHistoryRejection::HitDistanceMismatch);
        }

        return result;
    }

    // =========================================================================
    // Reactive data: the three causes, kept apart (issue #1256)
    // =========================================================================
    //
    // A pixel's history stops describing it for three materially different
    // reasons, and a temporal resolve must be able to tell them apart:
    //
    //   * SURFACE MOTION — the shading point moved across the screen. The
    //     reprojection already followed it; what is left is the resampling
    //     error, which grows with the distance travelled.
    //   * COVERAGE CHANGE — the shading point is still there and still the
    //     same material, but the fraction of the pixel it OCCUPIES moved. A
    //     strand thinned at a LOD step, a leaf rotated edge-on, an alpha test
    //     flipped. Nothing else in EvaluateSurfaceHistory can see this: the
    //     instance, primitive, material and depth are all unchanged.
    //   * MATERIAL / PROFILE CHANGE — the surface is being re-parameterised
    //     under a stationary camera. A subsurface profile blending, a leaf
    //     translucency profile sliding with the season.
    //
    // WHY THREE SCALARS RATHER THAN ONE. They are tuned against different
    // subjects and cannot share a knob. Hair needs a coverage response that
    // would make skin swim; skin needs a profile response hair never
    // exercises. Collapsing them to one number means every retune of one
    // subject silently retunes the other two — which is the regression the
    // issue's "a history heuristic tuned on one regresses another" is about.
    // Keeping them apart also makes an ATTRIBUTION possible: a test, an AOV
    // or a debug view can ask WHICH cause dropped the history, and "the
    // coverage term fired" is a finding where "confidence was 0.4" is not.
    //
    // WHY A DEAD BAND ON COVERAGE, AND WHY IT IS THE LOAD-BEARING PART.
    // A stochastic coverage estimator — which is what the strand compositor
    // is (see Groom/GroomCoverage.h) — moves its per-pixel coverage every
    // frame BY CONSTRUCTION. That movement is zero-mean noise, and averaging
    // it away is the entire reason the temporal resolve exists. So a coverage
    // term that reacted to the raw frame-to-frame delta would drop history
    // precisely where history is doing its job, and hair would sparkle
    // *worse* with the feature on than off — while every still capture looked
    // fine. The dead band is what separates the two: below it the resolve
    // keeps accumulating and converges; above it the mean has genuinely
    // shifted and the history is stale. This is the same reasoning, for the
    // same reason, as the sub-pixel dead zone in OloTemporalMotionFeedback —
    // a jittered pass moves ~1 px every frame by construction too.
    struct TemporalReactivity
    {
        f32 SurfaceMotion = 0.0f;
        f32 CoverageChange = 0.0f;
        f32 MaterialChange = 0.0f;

        // Confidence in the history: the fraction that survives all three.
        //
        // The three causes are INDEPENDENT reasons the history is wrong, so
        // the fractions that survive each of them MULTIPLY. max() would let
        // the largest cause mask the other two, so a pixel that is 0.5
        // reactive for all three reads identically to one that is 0.5
        // reactive for one — and a sum would saturate past 1 and need a
        // clamp that quietly turns three small causes into a total rejection.
        [[nodiscard]] constexpr f32 Confidence() const
        {
            return (1.0f - SurfaceMotion) * (1.0f - CoverageChange) * (1.0f - MaterialChange);
        }

        // The complement, for callers that want "how reactive is this pixel"
        // directly — an FSR2-style reactive mask, or a debug AOV.
        [[nodiscard]] constexpr f32 Combined() const
        {
            return 1.0f - Confidence();
        }
    };

    struct TemporalReactivitySettings
    {
        // The dead zone / saturation pair PostProcess_TAA.glsl already passes
        // to OloTemporalMotionFeedback, so the separated model agrees with
        // the tuning TAA shipped with rather than introducing a second.
        f32 MotionDeadZonePixels = 1.0f;
        f32 MotionSaturationPixels = 5.0f;
        // The rest of that agreement. OloTemporalMotionFeedback takes
        // min(feedback, 0.5) — motion may drive feedback DOWN to a half, and
        // no further — because a resolve that keeps no history at all during
        // a pan is showing raw jittered frames, which aliases worse than the
        // ghosting the rejection was avoiding. Ramping motion reactivity to
        // a full 1.0 would be exactly that, so it is capped here on the same
        // reasoning and at the same value.
        //
        // The cap applies to the RAMP only. A non-finite motion still yields
        // full reactivity: that is a broken signal rather than a fast one,
        // and the safe answer to a broken signal is to keep no history.
        f32 MotionMaxReactivity = 0.5f;
        // Per-frame coverage noise a converging stochastic estimator shows.
        // Below this, a coverage delta is the estimator working rather than
        // the subject changing — see the header comment above.
        //
        // SIZE THIS TO THE WORST CONSECUTIVE DELTA, NOT THE AMPLITUDE. Two
        // frames of a zero-mean +/-A jitter can differ by 2A, so the band
        // must clear 2A or the largest jumps still get through — and those
        // are the frames that move the output most, so a band of A measures
        // WORSE than no term at all. The default clears the +/-0.06 the
        // strand compositor's stochastic mode shows; a noisier estimator
        // needs it raised, and TemporalReconstructionSequenceTest's
        // StochasticCoverageNoiseMustNotDriveTheReactiveTerm is where that
        // arithmetic is written down.
        f32 CoverageNoiseDeadBand = 0.12f;
        f32 CoverageSaturation = 0.35f;
        f32 MaterialProfileDeadBand = 0.02f;
        f32 MaterialProfileSaturation = 0.25f;
        glm::vec2 PixelSize{ 1.0f };
    };

    namespace SurfaceHistoryDetail
    {
        // Linear ramp from 0 at `deadBand` to 1 at `saturation`, clamped.
        // A non-finite input reads as fully reactive: the one safe answer
        // when the signal itself is broken is to keep no history.
        [[nodiscard]] inline f32 ReactiveRamp(f32 magnitude, f32 deadBand, f32 saturation)
        {
            if (!std::isfinite(magnitude))
                return 1.0f;
            const f32 span = std::max(saturation - deadBand, 1.0e-4f);
            return std::clamp((magnitude - deadBand) / span, 0.0f, 1.0f);
        }
    } // namespace SurfaceHistoryDetail

    [[nodiscard]] inline TemporalReactivity EvaluateTemporalReactivity(
        const SurfaceHistoryRecord& current,
        const SurfaceHistoryRecord& previous,
        const TemporalReactivitySettings& settings)
    {
        const glm::vec2 motionPixels = current.Motion / glm::max(settings.PixelSize, glm::vec2(1.0e-8f));

        return {
            // Tested before the length rather than after, matching the GLSL
            // twin's shape: a non-finite motion must read as fully reactive,
            // and fabricating a NaN to push through the ramp says the same
            // thing in a way the reader has to work out.
            .SurfaceMotion =
                SurfaceHistoryDetail::IsFinite(motionPixels)
                    ? std::min(SurfaceHistoryDetail::ReactiveRamp(std::sqrt(glm::dot(motionPixels, motionPixels)),
                                                                  settings.MotionDeadZonePixels,
                                                                  settings.MotionSaturationPixels),
                               std::clamp(settings.MotionMaxReactivity, 0.0f, 1.0f))
                    : 1.0f,
            .CoverageChange = SurfaceHistoryDetail::ReactiveRamp(
                std::abs(current.Coverage - previous.Coverage), settings.CoverageNoiseDeadBand,
                settings.CoverageSaturation),
            .MaterialChange = SurfaceHistoryDetail::ReactiveRamp(
                std::abs(current.MaterialProfile - previous.MaterialProfile),
                settings.MaterialProfileDeadBand, settings.MaterialProfileSaturation),
        };
    }

    enum class TemporalSignalKind : u8
    {
        RGBRadiance,
        ScalarVisibility,
    };

    struct TemporalMoments
    {
        glm::vec4 First{ 0.0f };
        glm::vec4 Second{ 0.0f };
        f32 HistoryLength = 0.0f;
    };

    [[nodiscard]] inline TemporalMoments AccumulateTemporalMoments(
        const glm::vec4& current,
        const TemporalMoments& previous,
        bool historyAccepted,
        f32 maximumHistoryLength = 255.0f)
    {
        if (!historyAccepted)
        {
            return {
                .First = current,
                .Second = current * current,
                .HistoryLength = 1.0f,
            };
        }
        const f32 previousLength = std::max(previous.HistoryLength, 0.0f);
        const f32 length = std::min(previousLength + 1.0f, std::max(maximumHistoryLength, 1.0f));
        const f32 historyWeight = (length - 1.0f) / length;
        const f32 currentWeight = 1.0f / length;
        return {
            .First = previous.First * historyWeight + current * currentWeight,
            .Second = previous.Second * historyWeight + current * current * currentWeight,
            .HistoryLength = length,
        };
    }

    [[nodiscard]] inline glm::vec4 TemporalVariance(const TemporalMoments& moments)
    {
        return glm::max(moments.Second - moments.First * moments.First, glm::vec4(0.0f));
    }
} // namespace OloEngine
