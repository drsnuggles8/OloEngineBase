// OLO_TEST_LAYER: L1
// =============================================================================
// VolumetricShadowMapMathTest.cpp
//
// Pins the volumetric shadow map's geometry and accumulation (issue #723).
//
// Two halves, and they are pinned differently on purpose:
//
//   * The CASCADE FIT is real engine code — VolumetricShadowMath is deliberately
//     GL-free so this test calls it directly rather than mirroring it. What is
//     asserted is the PROPERTY the shader depends on, not the formula: every
//     point of the domain lands inside the unit cube, at every sun elevation,
//     including the near-horizon case where the obvious "walk down from the
//     layer top" formulation diverges as 1/sin(elevation).
//
//   * The SAMPLING and ACCUMULATION halves live in GLSL
//     (VolumetricShadowCommon.glsl's vsmCascadeW, VolumetricShadow_Generate's
//     midpoint loop), so those are literal CPU transcriptions — the same
//     discipline as CloudDensityMathTest and VolumetricFogMathTest. A silent
//     shader-side change is meant to surface as a test edit.
//
// What gets pinned:
//   - The light frame is orthonormal and right-handed, AxisZ points the way the
//     light travels, and a degenerate direction degrades instead of NaN-ing.
//   - The fitted cascade CONTAINS its domain — every corner, every elevation.
//   - The fit stays bounded as the sun approaches the horizon.
//   - Texel snapping absorbs sub-texel camera motion (no shadow swim) without
//     ever losing coverage.
//   - The two transforms agree: tex -> absolute world -> tex is the identity.
//   - Cascade w ranges are disjoint, so a trilinear tap cannot blend cloud
//     optical depth into a fog sample.
//   - The midpoint accumulation reproduces a constant medium's analytic optical
//     depth and never decreases along the ray.
//   - Fog volume world bounds contain the shape under translate/rotate/scale.
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/VolumetricShadowMap.h"

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        using namespace OloEngine::VolumetricShadowMath;

        constexpr u32 kResolution = VolumetricShadowMap::kResolution;
        constexpr u32 kSlices = VolumetricShadowMap::kSlicesPerCascade;
        constexpr u32 kCascades = VolumetricShadowMap::kCascadeCount;
        constexpr u32 kVolumeDepth = VolumetricShadowMap::kVolumeDepth;

        Bounds MakeBounds(const glm::vec3& center, const glm::vec3& halfExtent)
        {
            Bounds bounds;
            bounds.Min = center - halfExtent;
            bounds.Max = center + halfExtent;
            return bounds;
        }

        std::vector<glm::vec3> Corners(const Bounds& bounds)
        {
            std::vector<glm::vec3> corners;
            corners.reserve(8);
            for (u32 i = 0; i < 8u; ++i)
            {
                corners.emplace_back((i & 1u) ? bounds.Max.x : bounds.Min.x,
                                     (i & 2u) ? bounds.Max.y : bounds.Min.y,
                                     (i & 4u) ? bounds.Max.z : bounds.Min.z);
            }
            return corners;
        }

        // A sun direction at the given elevation above the horizon, pointing
        // TOWARD the light (the convention PrepareFrame takes).
        glm::vec3 TowardSunAtElevation(f32 elevationDegrees, f32 azimuthDegrees = 35.0f)
        {
            const f32 elevation = glm::radians(elevationDegrees);
            const f32 azimuth = glm::radians(azimuthDegrees);
            return glm::normalize(glm::vec3(std::cos(elevation) * std::cos(azimuth), std::sin(elevation),
                                            std::cos(elevation) * std::sin(azimuth)));
        }

        // ── Literal mirror of VolumetricShadowCommon.glsl's vsmCascadeW() ──
        f32 VsmCascadeW(f32 cascadeW, i32 cascade)
        {
            const f32 slices = std::max(static_cast<f32>(kSlices), 1.0f);
            const f32 w = std::clamp(cascadeW, 0.5f / slices, 1.0f - 0.5f / slices);
            return (static_cast<f32>(cascade) * slices + w * slices) * (1.0f / static_cast<f32>(kVolumeDepth));
        }

        // ── Literal mirror of VolumetricShadow_Generate.comp's march ──
        // Slice i stores the optical depth at the slice CENTRE: the running
        // total through slices [0, i) plus half of slice i's own contribution.
        std::vector<f32> MarchColumn(const std::vector<f32>& extinctionPerSlice, f32 stepLength)
        {
            std::vector<f32> stored;
            stored.reserve(extinctionPerSlice.size());
            f32 accumulated = 0.0f;
            for (const f32 extinction : extinctionPerSlice)
            {
                const f32 sliceOpticalDepth = std::max(extinction, 0.0f) * stepLength;
                stored.push_back(accumulated + sliceOpticalDepth * 0.5f);
                accumulated += sliceOpticalDepth;
            }
            return stored;
        }
    } // namespace

    // ─────────────────────────── Light frame ───────────────────────────

    TEST(VolumetricShadowMapMath, LightFrameIsOrthonormalAndRightHanded)
    {
        for (const f32 elevation : { 5.0f, 15.0f, 45.0f, 75.0f, 89.0f, 90.0f })
        {
            const glm::vec3 toward = TowardSunAtElevation(elevation);
            const LightFrame frame = BuildLightFrame(toward);

            EXPECT_NEAR(glm::length(frame.AxisX), 1.0f, 1e-5f) << "elevation " << elevation;
            EXPECT_NEAR(glm::length(frame.AxisY), 1.0f, 1e-5f) << "elevation " << elevation;
            EXPECT_NEAR(glm::length(frame.AxisZ), 1.0f, 1e-5f) << "elevation " << elevation;

            EXPECT_NEAR(glm::dot(frame.AxisX, frame.AxisY), 0.0f, 1e-5f) << "elevation " << elevation;
            EXPECT_NEAR(glm::dot(frame.AxisX, frame.AxisZ), 0.0f, 1e-5f) << "elevation " << elevation;
            EXPECT_NEAR(glm::dot(frame.AxisY, frame.AxisZ), 0.0f, 1e-5f) << "elevation " << elevation;

            // Right-handed: X x Y == Z.
            const glm::vec3 cross = glm::cross(frame.AxisX, frame.AxisY);
            EXPECT_NEAR(glm::length(cross - frame.AxisZ), 0.0f, 1e-5f) << "elevation " << elevation;

            // AxisZ is the direction the light TRAVELS, i.e. away from the body.
            EXPECT_NEAR(glm::dot(frame.AxisZ, toward), -1.0f, 1e-5f) << "elevation " << elevation;
        }
    }

    TEST(VolumetricShadowMapMath, LightFrameSurvivesADegenerateDirection)
    {
        for (const glm::vec3& degenerate : { glm::vec3(0.0f), glm::vec3(1e-9f, 0.0f, 0.0f) })
        {
            const LightFrame frame = BuildLightFrame(degenerate);
            for (const glm::vec3& axis : { frame.AxisX, frame.AxisY, frame.AxisZ })
            {
                EXPECT_TRUE(std::isfinite(axis.x) && std::isfinite(axis.y) && std::isfinite(axis.z));
                EXPECT_NEAR(glm::length(axis), 1.0f, 1e-5f);
            }
        }
    }

    // ─────────────────────────── Cascade fit ───────────────────────────

    TEST(VolumetricShadowMapMath, FittedCascadeContainsItsDomainAtEverySunElevation)
    {
        // The cloud cascade's shape: a 12 km window around the camera, layer
        // slab 1500..4000 m.
        const Bounds domain = MakeBounds(glm::vec3(0.0f, 2750.0f, 0.0f), glm::vec3(6000.0f, 1250.0f, 6000.0f));

        for (const f32 elevation : { 3.0f, 10.0f, 30.0f, 60.0f, 89.9f })
        {
            const LightFrame frame = BuildLightFrame(TowardSunAtElevation(elevation));
            const CascadeFit fit = FitCascade(domain, frame, kResolution, kSlices);
            ASSERT_TRUE(fit.IsValid()) << "elevation " << elevation;

            const glm::mat4 toTex = MakeRelWorldToTex(fit, glm::vec3(0.0f));
            for (const glm::vec3& corner : Corners(domain))
            {
                const glm::vec3 tex = glm::vec3(toTex * glm::vec4(corner, 1.0f));
                EXPECT_GE(tex.x, -1e-4f) << "elevation " << elevation;
                EXPECT_LE(tex.x, 1.0f + 1e-4f) << "elevation " << elevation;
                EXPECT_GE(tex.y, -1e-4f) << "elevation " << elevation;
                EXPECT_LE(tex.y, 1.0f + 1e-4f) << "elevation " << elevation;
                EXPECT_GE(tex.z, -1e-4f) << "elevation " << elevation;
                EXPECT_LE(tex.z, 1.0f + 1e-4f) << "elevation " << elevation;
            }
        }
    }

    TEST(VolumetricShadowMapMath, LowSunKeepsTheMarchBounded)
    {
        // The failure this guards: a "start at the layer top and walk down the
        // ray" formulation has depth = thickness / sin(elevation), which is
        // 19x the thickness at 3 degrees and unbounded at 0. Fitting the box to
        // the DOMAIN instead bounds it by the domain's own diagonal, so the
        // step length can never blow past the window.
        const Bounds domain = MakeBounds(glm::vec3(0.0f, 2750.0f, 0.0f), glm::vec3(6000.0f, 1250.0f, 6000.0f));
        // The domain's own diagonal is the hard ceiling on any projected
        // extent, plus the one-cell margin the snap adds (the cell is sized
        // from slices-1 so a snapped-down origin still covers the far face).
        const f32 diagonal = glm::length(domain.Max - domain.Min);
        const f32 ceiling = diagonal * static_cast<f32>(kSlices) / static_cast<f32>(kSlices - 1u) + 1.0f;

        // NOT a monotonicity check, deliberately: the depth peaks around 10
        // degrees and then eases off, because as the light axis flattens the
        // domain's 2500 m Y extent stops contributing to the projection while
        // its 12 km XZ extent already dominates. The property that matters is
        // that it stays between the medium's own thickness and the domain's
        // diagonal at EVERY elevation — bounded above, never degenerate below.
        const f32 layerThickness = domain.Max.y - domain.Min.y;
        for (const f32 elevation : { 60.0f, 30.0f, 10.0f, 3.0f, 0.5f, 0.0f })
        {
            const LightFrame frame = BuildLightFrame(TowardSunAtElevation(elevation));
            const CascadeFit fit = FitCascade(domain, frame, kResolution, kSlices);
            ASSERT_TRUE(fit.IsValid()) << "elevation " << elevation;
            EXPECT_LE(fit.SizeZ, ceiling) << "elevation " << elevation;
            EXPECT_GE(fit.SizeZ, layerThickness) << "elevation " << elevation;
            EXPECT_TRUE(std::isfinite(fit.SizeZ)) << "elevation " << elevation;
        }
    }

    TEST(VolumetricShadowMapMath, TexelSnappingAbsorbsSubTexelCameraMotion)
    {
        const glm::vec3 halfExtent(150.0f, 40.0f, 150.0f);
        const LightFrame frame = BuildLightFrame(TowardSunAtElevation(42.0f));

        const CascadeFit base = FitCascade(MakeBounds(glm::vec3(0.0f, 10.0f, 0.0f), halfExtent), frame, kResolution, kSlices);
        ASSERT_TRUE(base.IsValid());
        const f32 texelX = base.SizeX / static_cast<f32>(kResolution);

        // A translation far smaller than one texel must not move the origin at
        // all — on ANY axis. That is the whole point of the snap, and the
        // reason a moving camera does not make the shadow pattern swim across
        // the medium. The first implementation snapped X and Y but left the
        // LIGHT axis raw; the footprint held still while every sample slid
        // along the ray by a fraction of a slice each frame, which the map
        // itself would not have noticed (it keeps no history) and its temporal
        // consumers would have smeared into shimmer.
        const CascadeFit nudged =
            FitCascade(MakeBounds(glm::vec3(texelX * 0.1f, 10.0f, texelX * 0.05f), halfExtent), frame,
                       kResolution, kSlices);
        ASSERT_TRUE(nudged.IsValid());
        EXPECT_NEAR(glm::length(nudged.OriginAbs - base.OriginAbs), 0.0f, 1e-3f);
        EXPECT_NEAR(nudged.SizeX, base.SizeX, 1e-3f);
        EXPECT_NEAR(nudged.SizeY, base.SizeY, 1e-3f);
        EXPECT_NEAR(nudged.SizeZ, base.SizeZ, 1e-3f);

        // ...and a large translation must still produce a box that contains the
        // moved domain: the snap may never trade coverage for stability.
        const Bounds moved = MakeBounds(glm::vec3(5000.0f, 10.0f, -3000.0f), halfExtent);
        const CascadeFit distant = FitCascade(moved, frame, kResolution, kSlices);
        ASSERT_TRUE(distant.IsValid());
        const glm::mat4 toTex = MakeRelWorldToTex(distant, glm::vec3(0.0f));
        for (const glm::vec3& corner : Corners(moved))
        {
            const glm::vec3 tex = glm::vec3(toTex * glm::vec4(corner, 1.0f));
            EXPECT_GE(std::min({ tex.x, tex.y, tex.z }), -1e-4f);
            EXPECT_LE(std::max({ tex.x, tex.y, tex.z }), 1.0f + 1e-4f);
        }
    }

    TEST(VolumetricShadowMapMath, TheTwoTransformsAgree)
    {
        // RelWorldToTex and TexToAbsWorld are NOT inverses (one takes
        // render-relative, the other produces absolute), but they must compose
        // to the identity once the render origin is accounted for — this is
        // exactly what the generator relies on: it places a sample with
        // TexToAbsWorld and the consumer finds it again with RelWorldToTex.
        const glm::vec3 renderOrigin(1234.5f, -67.25f, 890.75f);
        const Bounds domain = MakeBounds(glm::vec3(1200.0f, 2750.0f, 900.0f), glm::vec3(6000.0f, 1250.0f, 6000.0f));
        const LightFrame frame = BuildLightFrame(TowardSunAtElevation(37.0f));
        const CascadeFit fit = FitCascade(domain, frame, kResolution, kSlices);
        ASSERT_TRUE(fit.IsValid());

        const glm::mat4 toTex = MakeRelWorldToTex(fit, renderOrigin);
        const glm::mat4 toWorld = MakeTexToAbsWorld(fit);

        for (const glm::vec3& tex : { glm::vec3(0.0f), glm::vec3(1.0f), glm::vec3(0.5f, 0.25f, 0.75f),
                                      glm::vec3(0.125f, 0.9f, 0.05f) })
        {
            const glm::vec3 absWorld = glm::vec3(toWorld * glm::vec4(tex, 1.0f));
            const glm::vec3 roundTrip = glm::vec3(toTex * glm::vec4(absWorld - renderOrigin, 1.0f));
            EXPECT_NEAR(roundTrip.x, tex.x, 1e-4f);
            EXPECT_NEAR(roundTrip.y, tex.y, 1e-4f);
            EXPECT_NEAR(roundTrip.z, tex.z, 1e-4f);
        }
    }

    TEST(VolumetricShadowMapMath, InvalidDomainProducesADisabledFit)
    {
        const LightFrame frame = BuildLightFrame(TowardSunAtElevation(45.0f));
        Bounds inverted;
        inverted.Min = glm::vec3(10.0f);
        inverted.Max = glm::vec3(-10.0f);
        EXPECT_FALSE(FitCascade(inverted, frame, kResolution, kSlices).IsValid());
        EXPECT_FALSE(FitCascade(Bounds{}, frame, kResolution, kSlices).IsValid());
    }

    // ───────────────────────── Cascade stacking ─────────────────────────

    TEST(VolumetricShadowMapMath, CascadeWRangesAreDisjoint)
    {
        // The two cascades share one 3D texture, so the clamp in vsmCascadeW is
        // the ONLY thing stopping a trilinear tap from blending kilometre-scale
        // cloud optical depth into a metre-scale fog sample. A gap of at least
        // one full texel between the ranges is what makes that impossible.
        f32 cloudMax = 0.0f;
        f32 fogMin = 1.0f;
        for (const f32 w : { -1.0f, 0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 2.0f })
        {
            cloudMax = std::max(cloudMax, VsmCascadeW(w, 0));
            fogMin = std::min(fogMin, VsmCascadeW(w, 1));
        }

        const f32 texel = 1.0f / static_cast<f32>(kVolumeDepth);
        EXPECT_LT(cloudMax, fogMin);
        EXPECT_GE(fogMin - cloudMax, texel * 0.999f)
            << "cascades must be separated by at least one texel or trilinear filtering bleeds across them";

        // Every mapped coordinate stays inside the volume.
        for (u32 cascade = 0; cascade < kCascades; ++cascade)
        {
            for (const f32 w : { -5.0f, 0.0f, 0.5f, 1.0f, 5.0f })
            {
                const f32 mapped = VsmCascadeW(w, static_cast<i32>(cascade));
                EXPECT_GE(mapped, 0.0f);
                EXPECT_LE(mapped, 1.0f);
            }
        }
    }

    TEST(VolumetricShadowMapMath, OutOfRangeWClampsToTheCascadesOwnEnds)
    {
        // Past the far face the physically right answer is the column TOTAL
        // (the point is entirely behind the medium); before the near face it is
        // the first slice. Clamping delivers both, without ever leaving the
        // cascade.
        for (u32 cascade = 0; cascade < kCascades; ++cascade)
        {
            const auto c = static_cast<i32>(cascade);
            EXPECT_FLOAT_EQ(VsmCascadeW(-3.0f, c), VsmCascadeW(0.0f, c));
            EXPECT_FLOAT_EQ(VsmCascadeW(9.0f, c), VsmCascadeW(1.0f, c));
        }
    }

    // ─────────────────────── Column accumulation ───────────────────────

    TEST(VolumetricShadowMapMath, MidpointMarchMatchesAConstantMediumAnalytically)
    {
        // A constant extinction sigma over a column of depth D has optical
        // depth sigma * d at distance d. The stored value at slice i sits at
        // d = (i + 0.5) * step, so midpoint quadrature is EXACT here — which is
        // the property that makes the half-step term load-bearing rather than
        // cosmetic. Storing the far-edge total instead would bias every tap by
        // sigma * step / 2.
        constexpr f32 kSigma = 0.004f;
        constexpr f32 kStep = 25.0f;
        const std::vector<f32> extinction(kSlices, kSigma);
        const std::vector<f32> stored = MarchColumn(extinction, kStep);

        ASSERT_EQ(stored.size(), kSlices);
        for (u32 i = 0; i < kSlices; ++i)
        {
            const f32 distance = (static_cast<f32>(i) + 0.5f) * kStep;
            EXPECT_NEAR(stored[i], kSigma * distance, 1e-4f) << "slice " << i;
        }
    }

    TEST(VolumetricShadowMapMath, OpticalDepthNeverDecreasesAlongTheRay)
    {
        // Optical depth is a running integral of a non-negative quantity, so a
        // decrease anywhere means the accumulation lost its running total —
        // which would read as a bright band inside a medium, not as an error.
        std::vector<f32> extinction(kSlices, 0.0f);
        for (u32 i = 0; i < kSlices; ++i)
        {
            // A lumpy medium with genuine holes, plus a negative sample the
            // shader's max() is expected to swallow.
            extinction[i] = ((i % 5u) == 0u) ? -0.01f : (0.002f * static_cast<f32>((i * 7u) % 11u));
        }

        const std::vector<f32> stored = MarchColumn(extinction, 12.0f);
        for (u32 i = 1; i < stored.size(); ++i)
        {
            EXPECT_GE(stored[i], stored[i - 1] - 1e-6f) << "slice " << i;
        }
        EXPECT_GE(stored.front(), 0.0f);
    }

    TEST(VolumetricShadowMapMath, AnEmptyColumnIsFullyLit)
    {
        const std::vector<f32> stored = MarchColumn(std::vector<f32>(kSlices, 0.0f), 50.0f);
        for (const f32 opticalDepth : stored)
        {
            EXPECT_FLOAT_EQ(opticalDepth, 0.0f);
            EXPECT_FLOAT_EQ(std::exp(-opticalDepth), 1.0f);
        }
    }

    // ───────────────────────── Fog volume bounds ─────────────────────────

    TEST(VolumetricShadowMapMath, FogVolumeBoundsContainTheShapeUnderATransform)
    {
        // Box, sphere and cylinder each read `extents` differently (the sphere
        // uses only x, the cylinder x as radius and y as half-height). Getting
        // that wrong would size the fog cascade to a shape that is not there,
        // and the volume would simply be reported unshadowed.
        const glm::mat4 localToWorld = glm::translate(glm::mat4(1.0f), glm::vec3(20.0f, 3.0f, -8.0f)) *
                                       glm::rotate(glm::mat4(1.0f), glm::radians(35.0f), glm::vec3(0.0f, 1.0f, 0.0f)) *
                                       glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 1.5f, 2.0f));
        const glm::mat4 worldToLocal = glm::inverse(localToWorld);

        struct Case
        {
            i32 Shape;
            glm::vec3 Extents;
            glm::vec3 LocalHalf;
        };
        const std::array<Case, 3> cases{ {
            { 0, glm::vec3(4.0f, 2.0f, 6.0f), glm::vec3(4.0f, 2.0f, 6.0f) }, // box
            { 1, glm::vec3(5.0f, 1.0f, 1.0f), glm::vec3(5.0f) },             // sphere: radius = x
            { 2, glm::vec3(3.0f, 7.0f, 1.0f), glm::vec3(3.0f, 7.0f, 3.0f) }, // cylinder: radius x, half-height y
        } };

        for (const Case& testCase : cases)
        {
            const Bounds bounds = FogVolumeWorldBounds(worldToLocal, testCase.Shape, testCase.Extents);
            ASSERT_TRUE(bounds.IsValid()) << "shape " << testCase.Shape;

            for (u32 i = 0; i < 8u; ++i)
            {
                const glm::vec3 local((i & 1u) ? testCase.LocalHalf.x : -testCase.LocalHalf.x,
                                      (i & 2u) ? testCase.LocalHalf.y : -testCase.LocalHalf.y,
                                      (i & 4u) ? testCase.LocalHalf.z : -testCase.LocalHalf.z);
                const glm::vec3 world = glm::vec3(localToWorld * glm::vec4(local, 1.0f));
                EXPECT_GE(world.x, bounds.Min.x - 1e-3f) << "shape " << testCase.Shape;
                EXPECT_LE(world.x, bounds.Max.x + 1e-3f) << "shape " << testCase.Shape;
                EXPECT_GE(world.y, bounds.Min.y - 1e-3f) << "shape " << testCase.Shape;
                EXPECT_LE(world.y, bounds.Max.y + 1e-3f) << "shape " << testCase.Shape;
                EXPECT_GE(world.z, bounds.Min.z - 1e-3f) << "shape " << testCase.Shape;
                EXPECT_LE(world.z, bounds.Max.z + 1e-3f) << "shape " << testCase.Shape;
            }
        }
    }

    TEST(VolumetricShadowMapMath, FogVolumeBoundsRejectADegenerateRecord)
    {
        EXPECT_FALSE(FogVolumeWorldBounds(glm::mat4(1.0f), 0, glm::vec3(0.0f)).IsValid());
        // A singular WorldToLocal cannot be inverted into a finite box.
        const glm::mat4 singular = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 1.0f));
        EXPECT_FALSE(FogVolumeWorldBounds(singular, 0, glm::vec3(2.0f)).IsValid());
    }

    // ───────────────────── Per-cascade enable gating ─────────────────────

    TEST(VolumetricShadowMapMath, PrepareFrameGatesEachCascadeIndependentlyAndReversibly)
    {
        // PrepareFrame is pure CPU by design, so the gate that decides whether a
        // medium self-shadows this frame is testable headlessly — which matters
        // because the obvious place to put this system was a render-graph pass,
        // and a graph Setup() that branches on a runtime toggle is FROZEN by the
        // frame-graph fingerprint cache (virtual-shadow-map-page-cache.md §5):
        // enable-after-the-first-frame then silently does nothing, with no error
        // anywhere. Running pre-graph dodges that entirely, and this test is
        // what says so out loud: every toggle below is flipped AFTER a frame
        // that ran with it off.
        CloudscapeRenderState clouds; // defaults: Enabled = false
        FogSettings fog;              // defaults: Enabled = false
        const FogVolumesUBOData volumes{};
        const glm::vec3 camera(0.0f, 10.0f, 0.0f);
        const glm::vec3 renderOrigin(0.0f);
        const glm::vec3 towardLight = glm::normalize(glm::vec3(0.3f, 0.8f, 0.2f));

        const auto prepare = [&](bool cloudFieldReady)
        {
            VolumetricShadowMap::PrepareFrame(clouds, fog, volumes, camera, renderOrigin, towardLight,
                                              cloudFieldReady);
        };
        const auto cloudCascade = []() -> const VolumetricShadowMap::CascadeState&
        { return VolumetricShadowMap::GetCascade(VolumetricShadowMap::Cascade::Cloud); };
        const auto fogCascade = []() -> const VolumetricShadowMap::CascadeState&
        { return VolumetricShadowMap::GetCascade(VolumetricShadowMap::Cascade::Fog); };

        prepare(true);
        EXPECT_FALSE(VolumetricShadowMap::AnyCascadeEnabled()) << "nothing enabled should march nothing";

        // Clouds on — and ONLY the cloud cascade, since the two media gate
        // separately.
        clouds.Enabled = true;
        clouds.VolumetricSelfShadow = true;
        prepare(true);
        EXPECT_TRUE(cloudCascade().Enabled);
        EXPECT_GT(cloudCascade().StepLength, 0.0f) << "an enabled cascade needs a real march step";
        EXPECT_FALSE(fogCascade().Enabled);

        // ...but not while the cloud NOISE FIELD is unusable. That gate is
        // separate from clouds.Enabled on purpose: the pipeline's fail-safe path
        // leaves the state enabled and disables the cloud UBO instead, and the
        // generator marches the same noise volumes the raymarch does.
        prepare(false);
        EXPECT_FALSE(cloudCascade().Enabled) << "the generator must fail safe with the field it samples";

        // Fog on, clouds off.
        clouds.Enabled = false;
        fog.Enabled = true;
        fog.EnableVolumetric = true;
        fog.EnableVolumetricSelfShadow = true;
        prepare(true);
        EXPECT_FALSE(cloudCascade().Enabled);
        EXPECT_TRUE(fogCascade().Enabled);
        EXPECT_GT(fogCascade().StepLength, 0.0f);

        // The froxel gate is load-bearing: the analytic fog fallback has no
        // per-sample march to attach a transmittance to, so shadowing it would
        // darken nothing while still costing a dispatch.
        fog.EnableVolumetric = false;
        prepare(true);
        EXPECT_FALSE(fogCascade().Enabled);

        // ...and back ON after a disabled frame. This is the assertion the
        // whole test exists for.
        fog.EnableVolumetric = true;
        prepare(true);
        EXPECT_TRUE(fogCascade().Enabled) << "re-enabling after a disabled frame must take effect";

        // A zero strength is a disable too — otherwise the generator burns a
        // dispatch producing optical depth the sampler multiplies away.
        fog.VolumetricSelfShadowStrength = 0.0f;
        prepare(true);
        EXPECT_FALSE(fogCascade().Enabled);

        // Leave the process-global snapshot clean for whatever runs next.
        clouds = CloudscapeRenderState{};
        fog = FogSettings{};
        prepare(true);
        EXPECT_FALSE(VolumetricShadowMap::AnyCascadeEnabled());
    }

    TEST(VolumetricShadowMapMath, FogCascadeHoldsStillUnderSubTexelCameraMotion)
    {
        // The end-to-end version of TexelSnappingAbsorbsSubTexelCameraMotion,
        // through the REAL domain builder — because the snap can be perfect and
        // still useless.
        //
        // FitCascade snaps the light-space origin to a grid whose cell size is
        // derived from the domain's EXTENT. That is stable only while the extent
        // is. The fog domain's vertical extent used to follow the camera
        // continuously (`bottom = min(cameraY, HeightOffset) - ...`), so once the
        // camera sat below HeightOffset every frame produced a slightly
        // different cell size, the floor() landed on a different grid, and every
        // sample slid sub-texel — straight into the froxel scatter's
        // 0.9-weight history, which is where it would have shown up as shimmer
        // rather than as anything pointing at this code.
        CloudscapeRenderState clouds; // disabled: fog cascade only
        FogSettings fog;
        fog.Enabled = true;
        fog.EnableVolumetric = true;
        fog.EnableVolumetricSelfShadow = true;
        const FogVolumesUBOData volumes{};
        const glm::vec3 renderOrigin(0.0f);
        const glm::vec3 towardLight = glm::normalize(glm::vec3(0.4f, 0.3f, 0.1f));

        // Below HeightOffset (0), which is the regime that used to slide.
        const glm::vec3 cameraBase(11.0f, -18.0f, -7.0f);
        VolumetricShadowMap::PrepareFrame(clouds, fog, volumes, cameraBase, renderOrigin, towardLight, false);
        const VolumetricShadowMap::CascadeState base =
            VolumetricShadowMap::GetCascade(VolumetricShadowMap::Cascade::Fog);
        ASSERT_TRUE(base.Enabled);

        // A camera drifting by centimetres, including vertically.
        for (const glm::vec3& nudge : { glm::vec3(0.03f, 0.02f, 0.01f), glm::vec3(-0.05f, 0.04f, 0.02f),
                                        glm::vec3(0.11f, -0.09f, 0.07f) })
        {
            VolumetricShadowMap::PrepareFrame(clouds, fog, volumes, cameraBase + nudge, renderOrigin,
                                              towardLight, false);
            const VolumetricShadowMap::CascadeState moved =
                VolumetricShadowMap::GetCascade(VolumetricShadowMap::Cascade::Fog);
            ASSERT_TRUE(moved.Enabled);

            // Identical transform means identical sampling: nothing slid.
            EXPECT_FLOAT_EQ(moved.StepLength, base.StepLength);
            for (u32 column = 0; column < 4u; ++column)
            {
                for (u32 row = 0; row < 4u; ++row)
                {
                    EXPECT_FLOAT_EQ(moved.RelWorldToTex[column][row], base.RelWorldToTex[column][row])
                        << "RelWorldToTex[" << column << "][" << row << "] moved under a sub-texel nudge";
                    EXPECT_FLOAT_EQ(moved.TexToAbsWorld[column][row], base.TexToAbsWorld[column][row])
                        << "TexToAbsWorld[" << column << "][" << row << "] moved under a sub-texel nudge";
                }
            }
        }

        // Leave the process-global snapshot clean.
        VolumetricShadowMap::PrepareFrame(CloudscapeRenderState{}, FogSettings{}, volumes, cameraBase,
                                          renderOrigin, towardLight, false);
    }

    TEST(VolumetricShadowMapMath, BoundsUnionAndWindowClampBehave)
    {
        const Bounds a = MakeBounds(glm::vec3(0.0f), glm::vec3(5.0f));
        const Bounds b = MakeBounds(glm::vec3(20.0f, 0.0f, 0.0f), glm::vec3(2.0f));

        const Bounds merged = UnionBounds(a, b);
        ASSERT_TRUE(merged.IsValid());
        EXPECT_FLOAT_EQ(merged.Min.x, -5.0f);
        EXPECT_FLOAT_EQ(merged.Max.x, 22.0f);

        // An invalid operand is ignored rather than poisoning the union — the
        // fog domain builder relies on that when height fog is off but volumes
        // are present, and vice versa.
        EXPECT_FLOAT_EQ(UnionBounds(a, Bounds{}).Max.x, a.Max.x);
        EXPECT_FLOAT_EQ(UnionBounds(Bounds{}, b).Max.x, b.Max.x);
        EXPECT_FALSE(UnionBounds(Bounds{}, Bounds{}).IsValid());

        // Clipping keeps one distant volume from stretching the cascade.
        const Bounds clipped = ClampBoundsToWindow(merged, glm::vec3(0.0f), 10.0f);
        ASSERT_TRUE(clipped.IsValid());
        EXPECT_FLOAT_EQ(clipped.Min.x, -5.0f);
        EXPECT_FLOAT_EQ(clipped.Max.x, 10.0f);

        // Entirely outside the window: contributes nothing at all.
        EXPECT_FALSE(ClampBoundsToWindow(b, glm::vec3(0.0f), 5.0f).IsValid());
    }
} // namespace OloEngine::Tests
