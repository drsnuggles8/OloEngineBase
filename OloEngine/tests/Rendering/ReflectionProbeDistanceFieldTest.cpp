// OLO_TEST_LAYER: L1
// =============================================================================
// Distance-impostor reflection probes (issue #705) — CPU contracts.
//
// Pins the encoding contract and the reference raymarch that
// include/ReflectionProbes.glsl mirrors expression-for-expression:
//   - the raymarch converges to the ANALYTIC intersection on star-shaped
//     rooms (sphere, box) where the truth is known in closed form,
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

#include <filesystem>
#include <fstream>
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
        std::vector<f32> mip0(static_cast<sizet>(kRes) * kRes * 6u, 10.0f);
        mip0[0] = 2.0f; // one near texel must NOT survive a MAX-downsample

        auto const mip1 = BuildNextMaxMip(mip0, kRes);
        ASSERT_EQ(mip1.size(), static_cast<sizet>(4) * 4 * 6);
        EXPECT_FLOAT_EQ(mip1[0], 10.0f); // max(2, 10, 10, 10)

        // Chain to 1x1: every face's single texel is the face max.
        auto const mip2 = BuildNextMaxMip(mip1, 4);
        auto const mip3 = BuildNextMaxMip(mip2, 2);
        ASSERT_EQ(mip3.size(), 6u);
        for (sizet face = 0; face < 6; ++face)
        {
            EXPECT_FLOAT_EQ(mip3[face], 10.0f);
        }
    }

    TEST(ReflectionProbeDistanceFieldTest, MaxFiniteDistanceIgnoresTheMissSentinel)
    {
        constexpr u32 kRes = 4;
        std::vector<f32> mip0(static_cast<sizet>(kRes) * kRes * 6u, 7.5f);
        mip0[3] = kProbeDistanceFar;           // sky texel — excluded
        mip0[9] = kProbeDistanceMissThreshold; // exactly at threshold — excluded
        EXPECT_FLOAT_EQ(ComputeMaxFiniteProbeDistance(mip0), 7.5f);

        // All-sky probes keep a valid (if useless) march bound.
        std::vector<f32> allSky(static_cast<sizet>(kRes) * kRes * 6u, kProbeDistanceFar);
        EXPECT_FLOAT_EQ(ComputeMaxFiniteProbeDistance(allSky), kProbeDistanceFar);
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
        std::vector<f32> mip0(static_cast<sizet>(kRes) * kRes * 6u);
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
        std::vector<f32> wrongSize(10, 1.0f);
        EXPECT_EQ(ReflectionProbeDistanceField::Create(std::move(wrongSize), kRes), nullptr);
        std::vector<f32> notPow2(static_cast<sizet>(6) * 6 * 6, 1.0f);
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
        std::vector<f32> mip0(static_cast<sizet>(kRes) * kRes * 6u, 5.0f);
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
    }
} // namespace OloEngine
