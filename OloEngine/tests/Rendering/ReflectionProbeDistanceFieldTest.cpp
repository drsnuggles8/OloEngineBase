// OLO_TEST_LAYER: L1
// =============================================================================
// Distance-impostor reflection probes (issue #705) — CPU contracts.
//
// Pins the encoding contract and the reference raymarch that
// include/ReflectionProbes.glsl mirrors expression-for-expression:
//   - the raymarch converges to the ANALYTIC intersection on star-shaped
//     rooms (sphere, box) where the truth is known in closed form, and with
//     the peeled layers (#1804) past an occluder to the wall it hides,
//   - a ray that leaves through sky is a MISS, never a bogus far-plane hit
//     (the sentinel-crossing bug class the miss threshold exists for),
//   - the max-mip chain is a conservative UPPER bound (the cheap reject may
//     over-admit, never over-reject),
//   - cube-face addressing matches the GL major-axis selection,
//   - the GLSL twin's constants cannot drift from the C++ ones.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Renderer/ReflectionProbeDistanceField.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace OloEngine
{
    namespace
    {
        // Analytic sphere room of radius R centred on the probe.
        struct SphereRoom
        {
            f32 Radius;
            f32 operator()(const glm::vec3&) const
            {
                return Radius;
            }
        };

        // Analytic axis-aligned box room [-h, h]^3 centred on the probe:
        // distance to the wall along unit direction u is h / max(|u_axis|).
        struct BoxRoom
        {
            f32 HalfExtent;
            f32 operator()(const glm::vec3& u) const
            {
                f32 const m = std::max(std::abs(u.x), std::max(std::abs(u.y), std::abs(u.z)));
                return HalfExtent / std::max(m, 1e-6f);
            }
        };

        // Closed-form ray/sphere exit point (origin inside the sphere).
        [[nodiscard]] f32 SphereExitT(const glm::vec3& p0, const glm::vec3& dir, f32 radius)
        {
            f32 const b = glm::dot(p0, dir);
            f32 const c = glm::dot(p0, p0) - radius * radius;
            return -b + std::sqrt(b * b - c);
        }

        // Closed-form ray/box exit point (origin inside the box) — slab method.
        [[nodiscard]] f32 BoxExitT(const glm::vec3& p0, const glm::vec3& dir, f32 h)
        {
            f32 tExit = std::numeric_limits<f32>::max();
            for (int axis = 0; axis < 3; ++axis)
            {
                if (std::abs(dir[axis]) < 1e-8f)
                {
                    continue;
                }
                f32 const t1 = (h - p0[axis]) / dir[axis];
                f32 const t2 = (-h - p0[axis]) / dir[axis];
                tExit = std::min(tExit, std::max(t1, t2));
            }
            return tExit;
        }
    } // namespace

    // ---- Reference raymarch vs analytic rooms ----

    TEST(ReflectionProbeRaymarch, ConvergesToTheAnalyticHitAcrossASphereRoom)
    {
        SphereRoom const room{ 5.0f };
        // Shading point just inside the +X wall, reflection straight across.
        glm::vec3 const p0(4.95f, 0.0f, 0.0f);
        glm::vec3 const dir(-1.0f, 0.0f, 0.0f);

        auto const result = RaymarchProbeDistanceField(p0, dir, room.Radius, room);
        ASSERT_TRUE(result.Hit);
        // The inside bias admits up to ~1% of radius past the surface; the
        // bisection bracket adds tMax / 32 / 2^6 ≈ 0.005.
        EXPECT_NEAR(result.HitT, SphereExitT(p0, dir, room.Radius), 0.15f);
        EXPECT_GT(glm::dot(result.HitDirection, glm::vec3(-1.0f, 0.0f, 0.0f)), 0.999f);
    }

    TEST(ReflectionProbeRaymarch, ConvergesToTheAnalyticHitOnAnOffAxisRay)
    {
        SphereRoom const room{ 5.0f };
        glm::vec3 const p0(4.95f, 0.0f, 0.0f);
        glm::vec3 const dir = glm::normalize(glm::vec3(-1.0f, 0.55f, -0.3f));

        auto const result = RaymarchProbeDistanceField(p0, dir, room.Radius, room);
        ASSERT_TRUE(result.Hit);

        f32 const tTrue = SphereExitT(p0, dir, room.Radius);
        EXPECT_NEAR(result.HitT, tTrue, 0.15f);
        glm::vec3 const trueDir = glm::normalize(p0 + dir * tTrue);
        EXPECT_GT(glm::dot(result.HitDirection, trueDir), 0.999f);
    }

    TEST(ReflectionProbeRaymarch, ConvergesInsideABoxRoom)
    {
        BoxRoom const room{ 3.0f };
        f32 const dMax = room.HalfExtent * std::sqrt(3.0f); // corner distance
        glm::vec3 const p0(2.9f, -0.4f, 0.7f);
        glm::vec3 const dir = glm::normalize(glm::vec3(-0.8f, 0.45f, -0.2f));

        auto const result = RaymarchProbeDistanceField(p0, dir, dMax, room);
        ASSERT_TRUE(result.Hit);

        f32 const tTrue = BoxExitT(p0, dir, room.HalfExtent);
        EXPECT_NEAR(result.HitT, tTrue, 0.25f);
        glm::vec3 const trueDir = glm::normalize(p0 + dir * tTrue);
        EXPECT_GT(glm::dot(result.HitDirection, trueDir), 0.995f);
    }

    TEST(ReflectionProbeRaymarch, AllSkyEnvironmentIsAMissNotAFarPlaneHit)
    {
        // Every direction stores the miss sentinel. Without the sentinel
        // check the march "crosses" the far sphere and reports a hit at
        // ~kProbeDistanceFar — the exact silent-wrong-reflection the
        // acceptance criteria's sky fallback forbids.
        auto const sky = [](const glm::vec3&)
        { return kProbeDistanceFar; };
        auto const result = RaymarchProbeDistanceField(glm::vec3(1.0f, 0.0f, 0.0f),
                                                       glm::vec3(0.0f, 1.0f, 0.0f),
                                                       kProbeDistanceFar, sky);
        EXPECT_FALSE(result.Hit);
    }

    TEST(ReflectionProbeRaymarch, RayLeavingThroughAWindowMisses)
    {
        // A room whose upper cone (u.y > 0.5) is open sky: a ray from the
        // FLOOR straight up passes through the probe centre and out the
        // window without ever meeting the r = 4 shell in a wall direction.
        // dMax comes from the walls, so the march budget ends long before
        // the sentinel distance — the ray must miss (sky fallback).
        //
        // (A tangential ray from a SIDE wall would legitimately hit the
        // shell below the window cone — the star-shaped surface really is
        // there — so the floor-to-window ray is the honest miss case.)
        auto const room = [](const glm::vec3& u)
        {
            return u.y > 0.5f ? kProbeDistanceFar : 4.0f;
        };
        auto const result = RaymarchProbeDistanceField(glm::vec3(0.0f, -3.95f, 0.0f),
                                                       glm::vec3(0.0f, 1.0f, 0.0f),
                                                       4.0f, room);
        EXPECT_FALSE(result.Hit);
    }

    // ---- Field construction: max-mips, dMax, addressing ----

    TEST(ReflectionProbeDistanceFieldTest, MaxMipChainIsAConservativeUpperBound)
    {
        constexpr u32 kRes = 8;
        TArray<f32> mip0(static_cast<sizet>(kRes) * kRes * 6u, 10.0f);
        mip0[0] = 2.0f; // one near texel must NOT survive a MAX-downsample

        auto const mip1 = BuildNextMaxMip({ mip0.GetData(), static_cast<sizet>(mip0.Num()) }, kRes);
        ASSERT_EQ(mip1.Num(), static_cast<sizet>(4) * 4 * 6);
        EXPECT_FLOAT_EQ(mip1[0], 10.0f); // max(2, 10, 10, 10)

        // Chain to 1x1: every face's single texel is the face max.
        auto const mip2 = BuildNextMaxMip({ mip1.GetData(), static_cast<sizet>(mip1.Num()) }, 4);
        auto const mip3 = BuildNextMaxMip({ mip2.GetData(), static_cast<sizet>(mip2.Num()) }, 2);
        ASSERT_EQ(mip3.Num(), 6u);
        for (sizet face = 0; face < 6; ++face)
        {
            EXPECT_FLOAT_EQ(mip3[face], 10.0f);
        }
    }

    TEST(ReflectionProbeDistanceFieldTest, MaxFiniteDistanceIgnoresTheMissSentinel)
    {
        constexpr u32 kRes = 4;
        TArray<f32> mip0(static_cast<sizet>(kRes) * kRes * 6u, 7.5f);
        mip0[3] = kProbeDistanceFar;           // sky texel — excluded
        mip0[9] = kProbeDistanceMissThreshold; // exactly at threshold — excluded
        EXPECT_FLOAT_EQ(ComputeMaxFiniteProbeDistance({ mip0.GetData(), static_cast<sizet>(mip0.Num()) }), 7.5f);

        // All-sky probes keep a valid (if useless) march bound.
        TArray<f32> allSky(static_cast<sizet>(kRes) * kRes * 6u, kProbeDistanceFar);
        EXPECT_FLOAT_EQ(ComputeMaxFiniteProbeDistance({ allSky.GetData(), static_cast<sizet>(allSky.Num()) }), kProbeDistanceFar);
    }

    TEST(ReflectionProbeDistanceFieldTest, CubeFaceSelectionMatchesTheGLMajorAxisRule)
    {
        constexpr u32 kRes = 8;
        // Axis directions land on their face's centre texels.
        EXPECT_EQ(DirectionToCubeFaceTexel({ 1.0f, 0.0f, 0.0f }, kRes).Face, 0u);
        EXPECT_EQ(DirectionToCubeFaceTexel({ -1.0f, 0.0f, 0.0f }, kRes).Face, 1u);
        EXPECT_EQ(DirectionToCubeFaceTexel({ 0.0f, 1.0f, 0.0f }, kRes).Face, 2u);
        EXPECT_EQ(DirectionToCubeFaceTexel({ 0.0f, -1.0f, 0.0f }, kRes).Face, 3u);
        EXPECT_EQ(DirectionToCubeFaceTexel({ 0.0f, 0.0f, 1.0f }, kRes).Face, 4u);
        EXPECT_EQ(DirectionToCubeFaceTexel({ 0.0f, 0.0f, -1.0f }, kRes).Face, 5u);

        // +X face orientation (GL table 8.19): s grows toward -Z, t toward -Y.
        auto const towardNegZ = DirectionToCubeFaceTexel({ 1.0f, 0.0f, -0.9f }, kRes);
        EXPECT_EQ(towardNegZ.Face, 0u);
        EXPECT_GT(towardNegZ.X, kRes / 2);
        auto const towardNegY = DirectionToCubeFaceTexel({ 1.0f, -0.9f, 0.0f }, kRes);
        EXPECT_EQ(towardNegY.Face, 0u);
        EXPECT_GT(towardNegY.Y, kRes / 2);
    }

    TEST(ReflectionProbeDistanceFieldTest, CreateBuildsTheFullChainAndSamplesPerFace)
    {
        constexpr u32 kRes = 8;
        TArray<f32> mip0(static_cast<sizet>(kRes) * kRes * 6u);
        for (u32 face = 0; face < 6; ++face)
        {
            for (u32 i = 0; i < kRes * kRes; ++i)
            {
                mip0[static_cast<sizet>(face) * kRes * kRes + i] = 10.0f + static_cast<f32>(face);
            }
        }

        auto field = ReflectionProbeDistanceField::Create(std::move(mip0), kRes);
        ASSERT_NE(field, nullptr);
        EXPECT_EQ(field->GetResolution(), kRes);
        EXPECT_EQ(field->GetMipCount(), 4u); // 8, 4, 2, 1
        EXPECT_FLOAT_EQ(field->GetMaxFiniteDistance(), 15.0f);

        EXPECT_FLOAT_EQ(field->SampleNearest({ 1.0f, 0.0f, 0.0f }, 0), 10.0f);
        EXPECT_FLOAT_EQ(field->SampleNearest({ -1.0f, 0.0f, 0.0f }, 0), 11.0f);
        EXPECT_FLOAT_EQ(field->SampleNearest({ 0.0f, 1.0f, 0.0f }, 1), 12.0f);
        EXPECT_FLOAT_EQ(field->SampleNearest({ 0.0f, -1.0f, 0.0f }, 2), 13.0f);
        EXPECT_FLOAT_EQ(field->SampleNearest({ 0.0f, 0.0f, 1.0f }, 3), 14.0f);
        EXPECT_FLOAT_EQ(field->SampleNearest({ 0.0f, 0.0f, -1.0f }, 5), 15.0f); // mip clamped to last

        // Malformed inputs fail loudly at the boundary, not downstream.
        TArray<f32> wrongSize(10, 1.0f);
        EXPECT_EQ(ReflectionProbeDistanceField::Create(std::move(wrongSize), kRes), nullptr);
        TArray<f32> notPow2(static_cast<sizet>(6) * 6 * 6, 1.0f);
        EXPECT_EQ(ReflectionProbeDistanceField::Create(std::move(notPow2), 6), nullptr);
    }

    TEST(ReflectionProbeRaymarch, OccludedStartSkipsTheOccluderAndHitsTheRealSurface)
    {
        // A sphere room of radius 6 with a captured OCCLUDER at distance 2
        // filling the downward cone (u.y < -0.8) — the probe looked down at
        // an object. A shading point BELOW that occluder (probe-space
        // (0,-4,0), i.e. occluded from the probe) reflects sideways: the
        // march starts OUTSIDE the impostor surface inside the occluder's
        // cone. Without the leading-outside skip it reports a bogus hit on
        // the occluder shell (|p| past dist immediately); with it, the ray
        // exits the cone, re-enters the visible region and converges on the
        // real room wall — this is the "dark crescents on every
        // probe-hidden surface" artifact the skip exists for.
        auto const room = [](const glm::vec3& u)
        {
            return u.y < -0.8f ? 2.0f : 6.0f;
        };
        glm::vec3 const p0(0.0f, -4.0f, 0.0f);
        glm::vec3 const dir = glm::normalize(glm::vec3(1.0f, 0.2f, 0.0f));

        auto const result = RaymarchProbeDistanceField(p0, dir, 6.0f, room);
        ASSERT_TRUE(result.Hit);
        // The hit must land on the r = 6 room shell, in a direction OUTSIDE
        // the occluder cone — not on the r = 2 occluder.
        EXPECT_GT(result.HitDirection.y, -0.8f)
            << "hit landed inside the occluder cone — the bogus occluder-shell hit";
        f32 const tTrue = SphereExitT(p0, dir, 6.0f);
        EXPECT_NEAR(result.HitT, tTrue, 0.5f);
    }

    TEST(ReflectionProbeRaymarch, RunsAgainstARealFieldThroughCubeAddressing)
    {
        // Same sphere-room expectation as the analytic test, but sampled
        // through a real field object — exercises Create + SampleNearest +
        // the cube addressing inside the march loop.
        constexpr u32 kRes = 64;
        TArray<f32> mip0(static_cast<sizet>(kRes) * kRes * 6u, 5.0f);
        auto field = ReflectionProbeDistanceField::Create(std::move(mip0), kRes);
        ASSERT_NE(field, nullptr);

        glm::vec3 const p0(4.9f, 0.2f, -0.1f);
        glm::vec3 const dir = glm::normalize(glm::vec3(-1.0f, 0.35f, 0.15f));
        auto const result = RaymarchProbeDistanceField(
            p0, dir, field->GetMaxFiniteDistance(),
            [&field](const glm::vec3& u)
            { return field->SampleNearest(u, 0); });

        ASSERT_TRUE(result.Hit);
        f32 const tTrue = SphereExitT(p0, dir, 5.0f);
        EXPECT_NEAR(result.HitT, tTrue, 0.15f);
        EXPECT_GT(glm::dot(result.HitDirection, glm::normalize(p0 + dir * tTrue)), 0.999f);
    }

    // ---- Cheap reject predicate ----

    TEST(ReflectionProbeDistanceFieldTest, CheapRejectAdmitsVisibleAndRejectsOccludedPoints)
    {
        // Visible: point at the stored surface distance (margins absorb
        // capture quantisation and low-mip interpolation).
        EXPECT_TRUE(ProbeCanSeePoint(5.0f, 5.0f));
        EXPECT_TRUE(ProbeCanSeePoint(5.4f, 5.0f)); // within 1.05x + 0.2
        // Occluded: the point sits well past everything the probe can see
        // in that cone.
        EXPECT_FALSE(ProbeCanSeePoint(5.0f, 3.0f));
        EXPECT_FALSE(ProbeCanSeePoint(100.0f, 4.0f));
    }

    // ---- UBO layout + GLSL constant parity ----

    TEST(ReflectionProbeDistanceFieldTest, ProbeUBOLayoutMatchesStd140)
    {
        using ProbeUBO = UBOStructures::ReflectionProbeUBO;
        static_assert(sizeof(ProbeUBO::Probe) == 32, "std140: two vec4s per probe");
        static_assert(sizeof(ProbeUBO) == 48 + 32 * sizeof(ProbeUBO::Probe),
                      "std140: uvec4 + vec4 + vec4 header, then the probe array");
        static_assert(ProbeUBO::MAX_PROBES == kMaxReflectionProbes,
                      "the UBO cap and the contract cap are the same constant");
        SUCCEED();
    }

    TEST(ReflectionProbeDistanceFieldTest, GlslTwinConstantsMatchTheContract)
    {
        namespace fs = std::filesystem;
        fs::path const header = fs::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders" / "include" /
                                "ReflectionProbes.glsl";
        ASSERT_TRUE(fs::exists(header)) << header.string();

        std::ifstream in(header, std::ios::binary);
        std::ostringstream buf;
        buf << in.rdbuf();
        std::string const src = buf.str();

        auto const defineValue = [&src](std::string_view name) -> f32
        {
            std::regex const re(std::string{ "#define\\s+" } + std::string{ name } + R"(\s+([0-9.]+))");
            std::smatch m;
            EXPECT_TRUE(std::regex_search(src, m, re)) << "missing #define " << name;
            return m.empty() ? -1.0f : std::stof(m[1].str());
        };

        EXPECT_EQ(static_cast<u32>(defineValue("OLO_PROBE_MARCH_ITERATIONS")), kProbeMarchIterations);
        EXPECT_EQ(static_cast<u32>(defineValue("OLO_PROBE_MARCH_TAPS")), kProbeMarchTaps);
        EXPECT_EQ(static_cast<u32>(defineValue("OLO_PROBE_REFINE_STEPS")), kProbeRefineSteps);
        EXPECT_FLOAT_EQ(defineValue("OLO_PROBE_INSIDE_BIAS_ABS"), kProbeInsideBiasAbs);
        EXPECT_FLOAT_EQ(defineValue("OLO_PROBE_INSIDE_BIAS_REL"), kProbeInsideBiasRel);
        EXPECT_FLOAT_EQ(defineValue("OLO_PROBE_MARCH_SLACK_REL"), kProbeMarchSlackRel);
        EXPECT_FLOAT_EQ(defineValue("OLO_PROBE_MARCH_SLACK_ABS"), kProbeMarchSlackAbs);
        EXPECT_FLOAT_EQ(defineValue("OLO_PROBE_REJECT_REL_MARGIN"), kProbeRejectRelMargin);
        EXPECT_FLOAT_EQ(defineValue("OLO_PROBE_REJECT_ABS_MARGIN"), kProbeRejectAbsMargin);
        EXPECT_EQ(static_cast<u32>(defineValue("OLO_PROBE_REJECT_MIP")), kProbeRejectMip);
        EXPECT_FLOAT_EQ(defineValue("OLO_PROBE_MISS_THRESHOLD"), kProbeDistanceMissThreshold);
        EXPECT_EQ(static_cast<u32>(defineValue("OLO_PROBE_GAP_REFINE_STEPS")), kProbeGapRefineSteps);
        EXPECT_FLOAT_EQ(defineValue("OLO_PROBE_INPAINT_MARGIN"), kProbeInpaintMarginRadians);
        EXPECT_EQ(static_cast<u32>(defineValue("OLO_PROBE_INPAINT_RING_COUNT")), kProbeInpaintRingCount);
        EXPECT_EQ(static_cast<u32>(defineValue("OLO_PROBE_INPAINT_AZIMUTHS")), kProbeInpaintAzimuths);
        EXPECT_FLOAT_EQ(defineValue("OLO_PROBE_INPAINT_FIRST_RING"), kProbeInpaintFirstRing);
        EXPECT_FLOAT_EQ(defineValue("OLO_PROBE_INPAINT_MATCH_ABS"), kProbeInpaintMatchAbs);
        EXPECT_FLOAT_EQ(defineValue("OLO_PROBE_INPAINT_MATCH_REL"), kProbeInpaintMatchRel);
        EXPECT_FLOAT_EQ(defineValue("OLO_PROBE_INPAINT_MATCH_SLOPE"), kProbeInpaintMatchSlope);

        // The bake's peel epsilon lives in the capture shader.
        fs::path const bake = fs::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders" / "ReflectionProbe_Distance.glsl";
        std::ifstream bakeIn(bake, std::ios::binary);
        std::ostringstream bakeBuf;
        bakeBuf << bakeIn.rdbuf();
        std::string const bakeSrc = bakeBuf.str();
        auto const bakeDefine = [&bakeSrc](std::string_view name) -> f32
        {
            std::regex const re(std::string{ "#define\\s+" } + std::string{ name } + R"(\s+([0-9.]+))");
            std::smatch m;
            EXPECT_TRUE(std::regex_search(bakeSrc, m, re)) << "missing #define " << name;
            return m.empty() ? -1.0f : std::stof(m[1].str());
        };
        EXPECT_FLOAT_EQ(bakeDefine("OLO_PROBE_PEEL_EPSILON_ABS"), kProbePeelEpsilonAbs);
        EXPECT_FLOAT_EQ(bakeDefine("OLO_PROBE_PEEL_EPSILON_REL"), kProbePeelEpsilonRel);
    }

    // ---- Layered distances: an occluder inside a room (issue #1804) ----

    namespace
    {
        // A box room [-6, 6]^3 around the probe with a floor-to-ceiling
        // pillar at x in [1, 1.6], z in [-3.3, -2.7] — the corridor's pillar
        // in miniature. Layers along u are what the three bake passes record:
        // the pillar's entry and exit, then the wall it hides; the wall alone
        // (no gap) elsewhere.
        constexpr f32 kRoomHalf = 6.0f;
        const glm::vec3 kPillarLo{ 1.0f, -kRoomHalf, -3.3f };
        const glm::vec3 kPillarHi{ 1.6f, kRoomHalf, -2.7f };

        // Ray/box entry and exit from outside; false on a miss.
        [[nodiscard]] bool RayBoxSpan(const glm::vec3& o, const glm::vec3& d, const glm::vec3& lo, const glm::vec3& hi,
                                      f32& tIn, f32& tOut)
        {
            tIn = 0.0f;
            tOut = std::numeric_limits<f32>::max();
            for (int axis = 0; axis < 3; ++axis)
            {
                if (std::abs(d[axis]) < 1e-8f)
                {
                    if (o[axis] < lo[axis] || o[axis] > hi[axis])
                    {
                        return false;
                    }
                    continue;
                }
                f32 t0 = (lo[axis] - o[axis]) / d[axis];
                f32 t1 = (hi[axis] - o[axis]) / d[axis];
                if (t0 > t1)
                {
                    std::swap(t0, t1);
                }
                tIn = std::max(tIn, t0);
                tOut = std::min(tOut, t1);
            }
            return tIn <= tOut;
        }

        [[nodiscard]] ProbeDistanceLayers PillarRoomLayers(const glm::vec3& u)
        {
            f32 const wall = BoxExitT(glm::vec3(0.0f), u, kRoomHalf);
            f32 tIn = 0.0f;
            f32 tOut = 0.0f;
            if (RayBoxSpan(glm::vec3(0.0f), u, kPillarLo, kPillarHi, tIn, tOut))
            {
                return NormalizeProbeDistanceLayers(tIn, tOut, wall);
            }
            return NormalizeProbeDistanceLayers(wall, wall, kProbeDistanceFar);
        }

        // Inside the pillar's radial shadow from the probe (x / -z between the
        // ratios of its two silhouette corners).
        [[nodiscard]] bool InPillarShadowCone(const glm::vec3& u)
        {
            if (u.z >= 0.0f)
            {
                return false;
            }
            f32 const ratio = u.x / -u.z;
            return ratio > (kPillarLo.x / -kPillarLo.z) && ratio < (kPillarHi.x / -kPillarHi.z);
        }
    } // namespace

    TEST(ReflectionProbeRaymarch, RayPassingBehindAnOccluderReachesTheWallItHides)
    {
        // Start beside the probe, heading down the room just right of the
        // pillar: the ray clears its right face, enters its radial shadow
        // behind it, and meets the far wall inside that shadow.
        glm::vec3 const p0(1.0f, 0.0f, 0.0f);
        glm::vec3 const dir = glm::normalize(glm::vec3(0.25f, 0.0f, -1.0f));
        f32 tIn = 0.0f;
        f32 tOut = 0.0f;
        ASSERT_FALSE(RayBoxSpan(p0, dir, kPillarLo, kPillarHi, tIn, tOut)) << "the ray must miss the pillar";
        f32 const tWall = BoxExitT(p0, dir, kRoomHalf);
        ASSERT_TRUE(InPillarShadowCone(glm::normalize(p0 + dir * tWall))) << "the wall hit must be hidden from the probe";

        // The single-distance encoding: everything behind the pillar is solid,
        // so the ray "hits" the pillar's shadow where it enters it (#1804).
        auto const frontOnly = RaymarchProbeDistanceField(p0, dir, kRoomHalf * std::sqrt(3.0f),
                                                          [](const glm::vec3& u)
                                                          { return PillarRoomLayers(u).Front; });
        ASSERT_TRUE(frontOnly.Hit);
        EXPECT_LT(frontOnly.HitT, tWall - 1.5f) << "control: the single layer should stop in the pillar's shadow";

        auto const layered = RaymarchProbeDistanceField(p0, dir, kRoomHalf * std::sqrt(3.0f), PillarRoomLayers);
        ASSERT_TRUE(layered.Hit);
        EXPECT_NEAR(layered.HitT, tWall, 0.2f) << "the ray stopped short of the wall";
        EXPECT_TRUE(layered.Occluded);
        // The radiance comes from the wall the ray hit, where the probe sees it
        // past the pillar's silhouette: outside the shadow cone, at the hit's
        // own distance.
        EXPECT_FALSE(InPillarShadowCone(layered.RadianceDirection));
        const f32 hitRadius = glm::length(p0 + dir * layered.HitT);
        const f32 ringAngle = std::acos(std::clamp(glm::dot(layered.RadianceDirection, layered.HitDirection), -1.0f, 1.0f));
        EXPECT_NEAR(PillarRoomLayers(layered.RadianceDirection).Front, hitRadius,
                    std::max(kProbeInpaintMatchAbs,
                             hitRadius * (kProbeInpaintMatchRel + (kProbeInpaintMatchSlope * ringAngle))) +
                        1.0e-4f)
            << "the radiance direction does not see the surface that was hit";
    }

    TEST(ReflectionProbeRaymarch, RayIntoAnOccludersHiddenSideStillHitsIt)
    {
        // The pillar's +X face is hidden from the probe. A ray aimed at it
        // crosses a thin sliver of gap just before the face; it must still
        // stop on the face, not slip through into the wall behind.
        glm::vec3 const p0(2.5f, 0.0f, -1.0f);
        glm::vec3 const target(1.6f, 0.0f, -3.0f);
        glm::vec3 const dir = glm::normalize(target - p0);
        f32 const tFace = glm::length(target - p0);

        auto const result = RaymarchProbeDistanceField(p0, dir, kRoomHalf * std::sqrt(3.0f), PillarRoomLayers);
        ASSERT_TRUE(result.Hit);
        EXPECT_NEAR(result.HitT, tFace, 0.15f);
        EXPECT_FALSE(result.Occluded) << "a hit on the pillar itself shades with the pillar's radiance";
        EXPECT_GT(glm::dot(result.RadianceDirection, result.HitDirection), 0.9999f);
    }

    TEST(ReflectionProbeRaymarch, ThinOccluderBetweenCoarseStepsIsNotSkipped)
    {
        // A 0.05 m slab across the ray, far thinner than one coarse step:
        // samples land in front of it and in the gap behind it, never inside.
        // The gap search between them must find it.
        auto const slabRoom = [](const glm::vec3& u) -> ProbeDistanceLayers
        {
            f32 const wall = BoxExitT(glm::vec3(0.0f), u, kRoomHalf);
            if (u.z < -0.5f)
            {
                f32 const front = 3.0f / -u.z;
                return NormalizeProbeDistanceLayers(front, 3.05f / -u.z, wall);
            }
            return NormalizeProbeDistanceLayers(wall, wall, kProbeDistanceFar);
        };
        glm::vec3 const p0(0.0f, -1.0f, 0.0f);
        glm::vec3 const dir = glm::normalize(glm::vec3(0.0f, 0.1f, -1.0f));
        f32 const tSlab = 3.0f / -dir.z;

        auto const result = RaymarchProbeDistanceField(p0, dir, kRoomHalf * std::sqrt(3.0f), slabRoom);
        ASSERT_TRUE(result.Hit);
        EXPECT_NEAR(result.HitT, tSlab, 0.1f);
        EXPECT_FALSE(result.Occluded);
    }

    TEST(ReflectionProbeDistanceFieldTest, NormalizeLayersCollapsesNothingBehindToNoGap)
    {
        // Sky: every layer at the sentinel, no gap.
        ProbeDistanceLayers const sky = NormalizeProbeDistanceLayers(kProbeDistanceFar, kProbeDistanceFar, kProbeDistanceFar);
        EXPECT_FLOAT_EQ(sky.Front, kProbeDistanceFar);
        EXPECT_FLOAT_EQ(sky.Behind, sky.Back);
        // A room's wall slab with nothing behind it: d2 = d1, so everything
        // beyond the wall stays solid (the single-distance behaviour).
        ProbeDistanceLayers const wall = NormalizeProbeDistanceLayers(5.0f, 5.25f, kProbeDistanceFar);
        EXPECT_FLOAT_EQ(wall.Back, 5.25f);
        EXPECT_FLOAT_EQ(wall.Behind, 5.25f);
        EXPECT_EQ(ClassifyProbeRayPoint(7.0f, wall), ProbeRayPointKind::Solid);
        // An open surface with nothing beyond it at all.
        ProbeDistanceLayers const plane = NormalizeProbeDistanceLayers(4.0f, kProbeDistanceFar, kProbeDistanceFar);
        EXPECT_EQ(ClassifyProbeRayPoint(50.0f, plane), ProbeRayPointKind::Solid);
        // An occluder in front of a wall keeps its gap.
        ProbeDistanceLayers const pillar = NormalizeProbeDistanceLayers(3.0f, 3.6f, 12.0f);
        EXPECT_EQ(ClassifyProbeRayPoint(2.0f, pillar), ProbeRayPointKind::Front);
        EXPECT_EQ(ClassifyProbeRayPoint(3.3f, pillar), ProbeRayPointKind::Solid);
        EXPECT_EQ(ClassifyProbeRayPoint(8.0f, pillar), ProbeRayPointKind::Gap);
        EXPECT_EQ(ClassifyProbeRayPoint(13.0f, pillar), ProbeRayPointKind::Solid);
        // Non-finite and out-of-order readbacks never reach the field.
        ProbeDistanceLayers const bad = NormalizeProbeDistanceLayers(std::numeric_limits<f32>::quiet_NaN(), 2.0f,
                                                                     std::numeric_limits<f32>::infinity());
        EXPECT_FLOAT_EQ(bad.Front, kProbeDistanceFar);
        EXPECT_GE(bad.Back, bad.Front);
        EXPECT_GE(bad.Behind, bad.Back);
        ProbeDistanceLayers const unordered = NormalizeProbeDistanceLayers(6.0f, 2.0f, 9.0f);
        EXPECT_FLOAT_EQ(unordered.Back, 6.0f);
        EXPECT_FLOAT_EQ(unordered.Behind, 9.0f);
    }

    TEST(ReflectionProbeDistanceFieldTest, LayeredFieldKeepsEveryLayerThroughTheMaxChain)
    {
        constexpr u32 kRes = 4;
        constexpr sizet kTexels = static_cast<sizet>(kRes) * kRes * 6u;
        TArray<f32> mip0(static_cast<i32>(kTexels * kProbeDistanceChannels), 0.0f);
        for (sizet i = 0; i < kTexels; ++i)
        {
            mip0[(i * kProbeDistanceChannels) + 0u] = 2.0f + static_cast<f32>(i % 3u);
            mip0[(i * kProbeDistanceChannels) + 1u] = 3.0f;
            mip0[(i * kProbeDistanceChannels) + 2u] = 20.0f; // a hidden wall beyond every d0
        }
        auto field = ReflectionProbeDistanceField::CreateLayered(std::move(mip0), kRes);
        ASSERT_NE(field, nullptr);
        EXPECT_EQ(field->GetMipCount(), 3u); // 4, 2, 1
        EXPECT_FLOAT_EQ(field->GetMaxFiniteDistance(), 20.0f) << "dMax must cover the d2 surfaces a march can hit";
        ProbeDistanceLayers const top = field->SampleLayersNearest({ 0.0f, 0.0f, -1.0f }, 2);
        EXPECT_FLOAT_EQ(top.Front, 4.0f);
        EXPECT_FLOAT_EQ(top.Back, 3.0f);
        EXPECT_FLOAT_EQ(top.Behind, 20.0f);
        EXPECT_EQ(field->GetMip(1).size(), static_cast<sizet>(2u * 2u * 6u * kProbeDistanceChannels));

        // The single-distance constructor has no gap anywhere.
        TArray<f32> flat(static_cast<i32>(kTexels), 7.0f);
        auto single = ReflectionProbeDistanceField::Create(std::move(flat), kRes);
        ASSERT_NE(single, nullptr);
        ProbeDistanceLayers const s = single->SampleLayersNearest({ 1.0f, 0.0f, 0.0f }, 0);
        EXPECT_FLOAT_EQ(s.Back, 7.0f);
        EXPECT_FLOAT_EQ(s.Behind, 7.0f);
    }
} // namespace OloEngine
