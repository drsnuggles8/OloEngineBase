// OLO_TEST_LAYER: L1
// =============================================================================
// LightProbePathTracedBakeTest.cpp
//
// Headless verification of the path-traced light-probe bake (issue #439):
// LightProbeBaker::BakeVolumePathTraced estimates each probe's L2 SH from a
// built ReferenceScene via PathTracer::TracePath — no GL, no ECS — and must be
// photometrically interchangeable with the cubemap route for the same incident
// field. Checked four ways:
//
//   1. MECHANICS: the bake succeeds, syncs the asset's grid parameters, writes
//      finite coefficients, sets validity flags, and reports progress.
//   2. PARITY: for a spread of (probe, normal) points, the SH-predicted
//      irradiance is compared against PathTracer::EstimateIrradiance with an
//      independent seed — generously on magnitude (L2 SH is a low-order
//      approximation of a non-smooth field), strictly on the FIELD SHAPE
//      (pairwise ordering, oracle-declared ties skipped) and on the colour
//      signature (a probe facing the red wall reconstructs red-shifted).
//   3. CONVENTION: the pipeline stores RAW RADIANCE PROJECTIONS
//      (c_i = ∫ L·Y_i dω, matching LightProbeBaker::ProjectToSH — no cosine
//      convolution, no 1/π), so the shader's evaluateSH reconstructs
//      band-limited RADIANCE, not irradiance E. A uniform environment pins
//      that numerically: evaluateSH returns L while EstimateIrradiance
//      returns π·L. See the convention note in LightProbeBaker.cpp.
//   4. DETERMINISM: two bakes of the same room are bit-identical (memcmp),
//      per the bake's per-probe-seed contract.
//
// RUNTIME BUDGET (Debug, where the tracer is ~40x slower than Release —
// reference-path-tracer.md §7): the whole file traces on the order of 13k
// paths at <= 3 bounces (four 18-probe room bakes at 160 spp across the
// tests, 9 oracle points at 160 spp, plus two tiny single-probe fixtures) —
// a small fraction of the existing PathTracing suite's ~65 s Debug cost,
// well under the ~15 s target.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "PathTracing/ReferenceSceneFixtures.h"

#include "OloEngine/Renderer/LightProbeBaker.h"
#include "OloEngine/Renderer/LightProbeVolumeAsset.h"
#include "OloEngine/Renderer/PathTracing/PathSampler.h"
#include "OloEngine/Renderer/PathTracing/PathTracer.h"
#include "OloEngine/Renderer/PathTracing/ReferenceScene.h"
#include "OloEngine/Renderer/SphericalHarmonics.h"
#include "OloEngine/Scene/Components.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // ---------------------------------------------------------------------
        // Faithful CPU port of the shader's SH evaluation, cited from
        // OloEditor/assets/shaders/include/SphericalHarmonics.glsl:
        //   - evaluateSHBasis (lines 23-34): the 9 L2 basis values,
        //   - evaluateSH (lines 38-49): plain dot product against the stored
        //     coefficient vec3s, clamped at zero.
        // Deliberately NO cosine-convolution factors and NO normalization —
        // that is exactly what the shader computes from the 9 vec4s that
        // sampleLightProbeGrid fetches (LightProbeSampling.glsl lines 72-153;
        // u_ProbeIntensity defaults to 1 and is not modelled here).
        // ---------------------------------------------------------------------
        [[nodiscard]] glm::vec3 ShaderEvaluateSH(const SHCoefficients& sh, const glm::vec3& n)
        {
            const f32 basis[SH_COEFFICIENT_COUNT] = {
                0.282095f,                             // SH_Y00
                0.488603f * n.y,                       // SH_Y1n1
                0.488603f * n.z,                       // SH_Y10
                0.488603f * n.x,                       // SH_Y11
                1.092548f * n.x * n.y,                 // SH_Y2n2
                1.092548f * n.y * n.z,                 // SH_Y2n1
                0.315392f * (3.0f * n.z * n.z - 1.0f), // SH_Y20
                1.092548f * n.x * n.z,                 // SH_Y21
                0.546274f * (n.x * n.x - n.y * n.y)    // SH_Y22
            };
            glm::vec3 result(0.0f);
            for (u32 i = 0; i < SH_COEFFICIENT_COUNT; ++i)
            {
                result += sh.Coefficients[i] * basis[i];
            }
            return glm::max(result, glm::vec3(0.0f));
        }

        // ---------------------------------------------------------------------
        // The physically comparable quantity to PathTracer::EstimateIrradiance.
        // The pipeline stores radiance projections c_i = ∫ L·Y_i dω, so the SH
        // prediction of the irradiance E(n) = ∫ L(ω)·max(0, n·ω) dω is
        //     E_SH(n) = Σ c_i · Â_l(i) · Y_i(n)
        // with the cosine-lobe convolution factors Â_0 = π, Â_1 = 2π/3,
        // Â_2 = π/4 (Ramamoorthi & Hanrahan 2001). The SHADER applies none of
        // these — see ShaderEvaluateSH above and the convention test below —
        // so this bridge lives in the test only, to compare like with like.
        // ---------------------------------------------------------------------
        [[nodiscard]] glm::vec3 ShIrradiance(const SHCoefficients& sh, const glm::vec3& n)
        {
            const f32 pi = glm::pi<f32>();
            const f32 bandFactor[SH_COEFFICIENT_COUNT] = {
                pi,                                                   // l = 0
                2.0f * pi / 3.0f, 2.0f * pi / 3.0f, 2.0f * pi / 3.0f, // l = 1
                pi / 4.0f, pi / 4.0f, pi / 4.0f, pi / 4.0f, pi / 4.0f // l = 2
            };
            const f32 basis[SH_COEFFICIENT_COUNT] = {
                0.282095f,
                0.488603f * n.y,
                0.488603f * n.z,
                0.488603f * n.x,
                1.092548f * n.x * n.y,
                1.092548f * n.y * n.z,
                0.315392f * (3.0f * n.z * n.z - 1.0f),
                1.092548f * n.x * n.z,
                0.546274f * (n.x * n.x - n.y * n.y)
            };
            glm::vec3 result(0.0f);
            for (u32 i = 0; i < SH_COEFFICIENT_COUNT; ++i)
            {
                result += sh.Coefficients[i] * (bandFactor[i] * basis[i]);
            }
            return glm::max(result, glm::vec3(0.0f));
        }

        [[nodiscard]] f32 MeanChannel(const glm::vec3& v)
        {
            return (v.x + v.y + v.z) / 3.0f;
        }

        // ---------------------------------------------------------------------
        // The colour-bleed room: closed grey box (floor y=0, ceiling y=2.4,
        // z-walls at ±1.5) with a RED wall at x = -2 and a GREEN wall at
        // x = +2, all normals inward, lit by one white point light near the
        // ceiling. Closed on purpose: bounce energy stays in and the zero
        // environment cannot leak through a missing face.
        // ---------------------------------------------------------------------
        constexpr f32 kRoomMinX = -2.0f; // RED wall (inner face, normal +X)
        constexpr f32 kRoomMaxX = 2.0f;  // GREEN wall (inner face, normal -X)
        constexpr f32 kFloorY = 0.0f;
        constexpr f32 kCeilingY = 2.4f;
        constexpr f32 kRoomMinZ = -1.5f;
        constexpr f32 kRoomMaxZ = 1.5f;

        struct ProbeRoom
        {
            PathTracing::ReferenceScene World;
            LightProbeVolumeComponent Volume;
        };

        [[nodiscard]] ProbeRoom MakeProbeRoom()
        {
            namespace Fx = PathTracingFixtures;
            ProbeRoom room;
            PathTracing::ReferenceScene& scene = room.World;

            PathTracing::ReferenceMaterial grey;
            grey.BaseColor = glm::vec3(0.6f);
            grey.Roughness = 1.0f;
            u32 const greyMat = scene.AddMaterial(grey);

            PathTracing::ReferenceMaterial red = grey;
            red.BaseColor = glm::vec3(0.7f, 0.04f, 0.04f);
            u32 const redMat = scene.AddMaterial(red);

            PathTracing::ReferenceMaterial green = grey;
            green.BaseColor = glm::vec3(0.04f, 0.7f, 0.04f);
            u32 const greenMat = scene.AddMaterial(green);

            Fx::AddFloorQuad(scene, kFloorY, kRoomMinX, kRoomMaxX, kRoomMinZ, kRoomMaxZ, greyMat);      // normal +Y
            Fx::AddCeilingQuad(scene, kCeilingY, kRoomMinX, kRoomMaxX, kRoomMinZ, kRoomMaxZ, greyMat);  // normal -Y
            Fx::AddLeftWallQuad(scene, kRoomMinX, kFloorY, kCeilingY, kRoomMinZ, kRoomMaxZ, redMat);    // normal +X: RED
            Fx::AddRightWallQuad(scene, kRoomMaxX, kFloorY, kCeilingY, kRoomMinZ, kRoomMaxZ, greenMat); // normal -X: GREEN
            Fx::AddBackWallQuad(scene, kRoomMinZ, kRoomMinX, kRoomMaxX, kFloorY, kCeilingY, greyMat);   // normal +Z

            // Front wall at z = kRoomMaxZ with an INWARD (-Z) normal — the
            // winding AddBox uses for its -Z face (cross(p1-p0, p2-p0) = -Z).
            u32 const frontGeometry = scene.AddQuadGeometry(glm::vec3(kRoomMinX, kFloorY, kRoomMaxZ),
                                                            glm::vec3(kRoomMinX, kCeilingY, kRoomMaxZ),
                                                            glm::vec3(kRoomMaxX, kCeilingY, kRoomMaxZ),
                                                            glm::vec3(kRoomMaxX, kFloorY, kRoomMaxZ));
            scene.AddInstance(frontGeometry, glm::mat4(1.0f), greyMat);

            // One white point light near the ceiling, slightly off-centre so
            // no probe or symmetry axis coincides with it. Attenuation packed
            // per Scene.cpp's MultiLight convention: (1, 0, quadratic, range).
            PathTracing::ReferenceLight light;
            light.Type = PathTracing::ReferenceLightType::Point;
            light.Position = glm::vec3(0.15f, 2.1f, 0.1f);
            light.Color = glm::vec3(1.0f);
            light.Intensity = 6.0f;
            light.AttenuationParams = glm::vec4(1.0f, 0.0f, 0.35f, 30.0f);
            scene.AddLight(light);

            scene.Build();

            // 3x2x3 grid strictly inside the room: x probes at -1.5/0/+1.5,
            // y at 0.5/1.8, z at -0.9/0/+0.9 — all in open air, so every
            // probe must come back valid.
            room.Volume.m_BoundsMin = glm::vec3(-1.5f, 0.5f, -0.9f);
            room.Volume.m_BoundsMax = glm::vec3(1.5f, 1.8f, 0.9f);
            room.Volume.m_Resolution = glm::ivec3(3, 2, 3);
            return room;
        }

        [[nodiscard]] LightProbePathTracedBakeSettings MakeBakeSettings()
        {
            LightProbePathTracedBakeSettings settings;
            settings.SamplesPerProbe = 160; // 18 probes x 160 paths x <= 3 bounces per bake
            settings.MaxBounces = 3;
            settings.Seed = 0x439u;
            return settings;
        }

        // Mirror of the probe-position derivation BakeVolume/BakeVolumePathTraced
        // share (bounds lerp by grid fraction; single-slice axes at BoundsMin) —
        // the oracle must be asked at exactly the position the bake integrated.
        [[nodiscard]] glm::vec3 ProbePosition(const LightProbeVolumeComponent& volume, i32 x, i32 y, i32 z)
        {
            glm::vec3 const extent = volume.m_BoundsMax - volume.m_BoundsMin;
            glm::vec3 t(0.0f);
            if (volume.m_Resolution.x > 1)
                t.x = static_cast<f32>(x) / static_cast<f32>(volume.m_Resolution.x - 1);
            if (volume.m_Resolution.y > 1)
                t.y = static_cast<f32>(y) / static_cast<f32>(volume.m_Resolution.y - 1);
            if (volume.m_Resolution.z > 1)
                t.z = static_cast<f32>(z) / static_cast<f32>(volume.m_Resolution.z - 1);
            return volume.m_BoundsMin + extent * t;
        }

        struct BakedVolume
        {
            ProbeRoom Room;
            Ref<LightProbeVolumeAsset> Asset;
            bool Success = false;
            i32 ProgressCalls = 0;
            i32 LastCurrent = 0;
            i32 LastTotal = 0;
        };

        [[nodiscard]] BakedVolume BakeRoomVolume(const LightProbePathTracedBakeSettings& settings)
        {
            BakedVolume baked;
            baked.Room = MakeProbeRoom();
            baked.Asset = Ref<LightProbeVolumeAsset>::Create();
            baked.Success = LightProbeBaker::BakeVolumePathTraced(
                baked.Room.World, baked.Room.Volume, baked.Asset, settings,
                [&baked](i32 current, i32 total)
                {
                    ++baked.ProgressCalls;
                    baked.LastCurrent = current;
                    baked.LastTotal = total;
                });
            return baked;
        }
    } // namespace

    TEST(LightProbePathTracedBake, BakeSucceedsCoefficientsFiniteAndProbesValid)
    {
        const LightProbePathTracedBakeSettings settings = MakeBakeSettings();
        const BakedVolume baked = BakeRoomVolume(settings);

        ASSERT_TRUE(baked.Success);
        ASSERT_TRUE(baked.Asset);

        // Asset parameters synced from the component, exactly like BakeVolume.
        EXPECT_EQ(baked.Asset->Resolution, baked.Room.Volume.m_Resolution);
        EXPECT_EQ(baked.Asset->BoundsMin, baked.Room.Volume.m_BoundsMin); // authored literals round-trip bit-exactly
        EXPECT_EQ(baked.Asset->BoundsMax, baked.Room.Volume.m_BoundsMax);

        i32 const totalProbes = baked.Room.Volume.GetTotalProbeCount();
        ASSERT_EQ(totalProbes, 18);
        ASSERT_EQ(baked.Asset->CoefficientData.size(),
                  static_cast<size_t>(totalProbes) * SH_COEFFICIENT_COUNT);

        // Progress reported once per probe, ending at (total, total).
        EXPECT_EQ(baked.ProgressCalls, totalProbes);
        EXPECT_EQ(baked.LastCurrent, totalProbes);
        EXPECT_EQ(baked.LastTotal, totalProbes);

        for (i32 probe = 0; probe < totalProbes; ++probe)
        {
            auto const base = static_cast<size_t>(probe) * SH_COEFFICIENT_COUNT;
            for (u32 c = 0; c < SH_COEFFICIENT_COUNT; ++c)
            {
                const glm::vec4& coeff = baked.Asset->CoefficientData[base + c];
                EXPECT_TRUE(std::isfinite(coeff.x) && std::isfinite(coeff.y) &&
                            std::isfinite(coeff.z) && std::isfinite(coeff.w))
                    << "non-finite SH coefficient at probe " << probe << " coeff " << c;
            }
            // Every probe sits in open lit air -> validity flag 1.0 in
            // coeff[0].w, 0.0 in every other .w (SHCoefficients::ToGPULayout).
            // The flags are written as literals, so float == is bit-exact by
            // contract here (same reasoning as the lightmap parity test).
            EXPECT_EQ(baked.Asset->CoefficientData[base].w, 1.0f) << "probe " << probe << " flagged invalid";
            EXPECT_EQ(baked.Asset->CoefficientData[base + 1].w, 0.0f);
        }
    }

    TEST(LightProbePathTracedBake, ShIrradianceTracksTheOracleAndPreservesFieldShape)
    {
        const LightProbePathTracedBakeSettings settings = MakeBakeSettings();
        const BakedVolume baked = BakeRoomVolume(settings);
        ASSERT_TRUE(baked.Success);

        // Three probes with distinct surroundings x three normals each:
        //   - grid (0,0,1): x=-1.5, 0.5 m from the RED wall, low
        //   - grid (2,0,1): x=+1.5, 0.5 m from the GREEN wall, low
        //   - grid (1,1,1): room centre at y=1.8, 0.3 m under the light
        struct ParityPoint
        {
            glm::ivec3 Grid;
            glm::vec3 Normal;
        };
        const ParityPoint points[] = {
            { { 0, 0, 1 }, { 0.0f, 1.0f, 0.0f } },
            { { 0, 0, 1 }, { -1.0f, 0.0f, 0.0f } }, // faces the red wall
            { { 0, 0, 1 }, { 1.0f, 0.0f, 0.0f } },
            { { 2, 0, 1 }, { 0.0f, 1.0f, 0.0f } },
            { { 2, 0, 1 }, { -1.0f, 0.0f, 0.0f } },
            { { 2, 0, 1 }, { 1.0f, 0.0f, 0.0f } }, // faces the green wall
            { { 1, 1, 1 }, { 0.0f, 1.0f, 0.0f } }, // toward the brightly lit ceiling
            { { 1, 1, 1 }, { -1.0f, 0.0f, 0.0f } },
            { { 1, 1, 1 }, { 1.0f, 0.0f, 0.0f } },
        };
        constexpr sizet kPointCount = std::size(points);

        // Oracle: EstimateIrradiance with an INDEPENDENT seed (a genuinely
        // different sample set than the bake's) at the same transport depth.
        PathTracing::PathTracerSettings tracer;
        tracer.SamplesPerPixel = 160;
        tracer.MaxBounces = settings.MaxBounces;
        tracer.RussianRouletteStartBounce = 0;
        tracer.Seed = 0x5EEDD1FFu;

        glm::vec3 shaderEval[kPointCount];
        glm::vec3 shIrradiance[kPointCount];
        glm::vec3 oracle[kPointCount];
        for (sizet i = 0; i < kPointCount; ++i)
        {
            const ParityPoint& point = points[i];
            i32 const linearIdx = baked.Room.Volume.GridIndex(point.Grid.x, point.Grid.y, point.Grid.z);
            const SHCoefficients sh = baked.Asset->GetProbeData(linearIdx);
            glm::vec3 const position = ProbePosition(baked.Room.Volume, point.Grid.x, point.Grid.y, point.Grid.z);

            shaderEval[i] = ShaderEvaluateSH(sh, point.Normal);
            shIrradiance[i] = ShIrradiance(sh, point.Normal);
            u32 const oracleSeed = PathTracing::MakePixelSeed(static_cast<u32>(i), 0xADDu, tracer.Seed);
            oracle[i] = PathTracing::PathTracer::EstimateIrradiance(
                baked.Room.World, position, point.Normal, tracer, oracleSeed);
        }

        // ---- magnitude: generous, per point ---------------------------------
        // L2 SH is a low-order approximation of a field with hard occlusion
        // edges (walls filling half a probe's sky), so per-point magnitude can
        // legitimately deviate; [0.6, 1.6] catches units slips (a stray π, a
        // lost 4π/N, a cosine factor applied twice) without failing on the
        // approximation itself. Compared via the cosine-convolved SH
        // irradiance, the quantity in the ORACLE's units — the raw shader
        // reconstruction is radiance-convention and sits ~1/π below E by
        // design (pinned by the UniformEnvironment test below).
        for (sizet i = 0; i < kPointCount; ++i)
        {
            f32 const oracleMean = MeanChannel(oracle[i]);
            ASSERT_GT(oracleMean, 1e-4f) << "oracle irradiance ~zero at point " << i
                                         << " — the fixture light is not reaching the room";
            f32 const ratio = MeanChannel(shIrradiance[i]) / oracleMean;
            EXPECT_GT(ratio, 0.6f) << "point " << i << ": SH irradiance far below ground truth";
            EXPECT_LT(ratio, 1.6f) << "point " << i << ": SH irradiance far above ground truth";
        }

        // ---- field shape: strict pairwise ordering --------------------------
        // A global scale error passes any magnitude band; it cannot pass an
        // ordering assertion. Pairs the ORACLE itself calls close (within 15%)
        // are skipped as ties (reference-path-tracer.md §4). The ordering is
        // asserted on the shader-faithful evaluation — the value the renderer
        // will actually show — which shares the oracle's ordering because the
        // per-band convention factors are positive.
        u32 comparedPairs = 0;
        for (sizet a = 0; a < kPointCount; ++a)
        {
            for (sizet b = a + 1; b < kPointCount; ++b)
            {
                f32 const oracleA = MeanChannel(oracle[a]);
                f32 const oracleB = MeanChannel(oracle[b]);
                f32 const larger = std::max(oracleA, oracleB);
                if (larger <= 0.0f || std::abs(oracleA - oracleB) <= 0.15f * larger)
                {
                    continue; // oracle-declared tie
                }
                ++comparedPairs;
                f32 const shA = MeanChannel(shaderEval[a]);
                f32 const shB = MeanChannel(shaderEval[b]);
                if (oracleA > oracleB)
                {
                    EXPECT_GT(shA, shB) << "ordering flip: oracle ranks point " << a << " (" << oracleA
                                        << ") above point " << b << " (" << oracleB
                                        << ") but the baked SH ranks them " << shA << " vs " << shB;
                }
                else
                {
                    EXPECT_LT(shA, shB) << "ordering flip: oracle ranks point " << b << " (" << oracleB
                                        << ") above point " << a << " (" << oracleA
                                        << ") but the baked SH ranks them " << shB << " vs " << shA;
                }
            }
        }
        // The fixture is built to have real contrasts (under-the-light vs
        // low-corner points); if nearly everything tied, the assertion above
        // became vacuous and the fixture needs re-tuning, not the bake.
        EXPECT_GE(comparedPairs, 10u);

        // ---- colour signature -----------------------------------------------
        // Both walls receive the same WHITE direct light, so any r/g asymmetry
        // in a wall-facing reconstruction can only come from the wall albedo
        // carried by bounce radiance: the red-wall probe's -X evaluation must
        // be red-shifted and the green-wall probe's +X evaluation
        // green-shifted. Points[1] and points[5] above. The oracle is asserted
        // first so a fixture regression reads as a fixture failure.
        EXPECT_GT(oracle[1].r, oracle[1].g) << "oracle: red wall bounce not red — fixture broken";
        EXPECT_GT(oracle[5].g, oracle[5].r) << "oracle: green wall bounce not green — fixture broken";
        EXPECT_GT(shaderEval[1].r, shaderEval[1].g * 1.15f)
            << "red-wall-facing SH not red-shifted: r=" << shaderEval[1].r << " g=" << shaderEval[1].g;
        EXPECT_GT(shaderEval[5].g, shaderEval[5].r * 1.15f)
            << "green-wall-facing SH not green-shifted: g=" << shaderEval[5].g << " r=" << shaderEval[5].r;
    }

    TEST(LightProbePathTracedBake, UniformEnvironmentPinsTheRadianceConvention)
    {
        // An EMPTY world with a uniform environment: every path escapes
        // immediately and returns exactly the environment radiance, so the
        // baked SH of the constant field L pins the storage convention with
        // no transport noise in the way.
        PathTracing::ReferenceScene world;
        PathTracing::ReferenceEnvironment environment;
        environment.Radiance = glm::vec3(0.8f, 0.6f, 0.4f); // chromatic, to pin per-channel scaling
        world.SetEnvironment(environment);
        world.Build();

        LightProbePathTracedBakeSettings settings;
        settings.SamplesPerProbe = 256;
        settings.MaxBounces = 1; // a primary-ray escape collects the environment
        settings.Seed = 0x439u;

        u32 const probeSeed = PathTracing::MakePixelSeed(0u, 0u, settings.Seed);
        bool valid = false;
        const SHCoefficients sh = LightProbeBaker::BakeProbeAtPositionPathTraced(
            world, glm::vec3(0.0f), settings, probeSeed, &valid);
        EXPECT_TRUE(valid);

        // THE CONVENTION (matches ProjectToSH, documented in
        // LightProbeBaker.cpp): coefficients are raw radiance projections, so
        // the shader-faithful reconstruction of a uniform field returns the
        // RADIANCE L itself — c_0 = 4π·Y00·L and c_0·Y00 = L exactly; the
        // higher coefficients only carry the direction set's residual
        // integration error (6% covers it comfortably at 256 Sobol' samples).
        const glm::vec3 normals[] = {
            { 0.0f, 1.0f, 0.0f },
            { -1.0f, 0.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f },
            glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f)),
        };
        for (const glm::vec3& n : normals)
        {
            const glm::vec3 eval = ShaderEvaluateSH(sh, n);
            EXPECT_NEAR(eval.r, 0.8f, 0.8f * 0.06f);
            EXPECT_NEAR(eval.g, 0.6f, 0.6f * 0.06f);
            EXPECT_NEAR(eval.b, 0.4f, 0.4f * 0.06f);
        }

        // ...while TRUE irradiance of the same field is π·L — the reference
        // oracle integrates a constant, so it returns it near-exactly. This is
        // the measured divergence between the probe pipeline's shader output
        // and PathTracer::EstimateIrradiance units: a factor of π on band 0.
        PathTracing::PathTracerSettings tracer;
        tracer.SamplesPerPixel = 32;
        tracer.MaxBounces = 1;
        tracer.RussianRouletteStartBounce = 0;
        const glm::vec3 oracleE = PathTracing::PathTracer::EstimateIrradiance(
            world, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), tracer, 0x1234u);
        const f32 pi = glm::pi<f32>();
        EXPECT_NEAR(oracleE.r, pi * 0.8f, pi * 0.8f * 0.005f);
        EXPECT_NEAR(oracleE.g, pi * 0.6f, pi * 0.6f * 0.005f);
        EXPECT_NEAR(oracleE.b, pi * 0.4f, pi * 0.4f * 0.005f);

        // And the test-side cosine-convolved bridge recovers the oracle's
        // units from the stored coefficients — the identity the parity test
        // above relies on.
        const glm::vec3 eSh = ShIrradiance(sh, glm::vec3(0.0f, 1.0f, 0.0f));
        EXPECT_NEAR(eSh.r / oracleE.r, 1.0f, 0.06f);
        EXPECT_NEAR(eSh.g / oracleE.g, 1.0f, 0.06f);
        EXPECT_NEAR(eSh.b / oracleE.b, 1.0f, 0.06f);
    }

    TEST(LightProbePathTracedBake, BuriedProbeIsFlaggedInvalid)
    {
        // A probe sealed inside a closed unlit box: every path terminates on
        // the box's dark interior (the environment outside is unreachable), so
        // the captured energy is zero and the mostly-black heuristic — the
        // SAME heuristic and threshold BakeProbeAtPosition uses — must flag
        // the probe invalid (coeff[0].w == 0.0, the sampleLightProbeGrid skip
        // convention).
        PathTracing::ReferenceScene world;
        PathTracing::ReferenceMaterial grey;
        grey.BaseColor = glm::vec3(0.6f);
        grey.Roughness = 1.0f;
        u32 const mat = world.AddMaterial(grey);
        PathTracingFixtures::AddBox(world, glm::vec3(-0.5f), glm::vec3(0.5f), mat);
        PathTracing::ReferenceEnvironment environment;
        environment.Radiance = glm::vec3(1.0f); // present but sealed OUT
        world.SetEnvironment(environment);
        world.Build();

        LightProbeVolumeComponent volume;
        volume.m_BoundsMin = glm::vec3(0.0f);
        volume.m_BoundsMax = glm::vec3(0.0f);
        volume.m_Resolution = glm::ivec3(1, 1, 1); // single probe at BoundsMin = the box centre

        auto asset = Ref<LightProbeVolumeAsset>::Create();
        LightProbePathTracedBakeSettings settings;
        settings.SamplesPerProbe = 64;
        settings.MaxBounces = 2;
        ASSERT_TRUE(LightProbeBaker::BakeVolumePathTraced(world, volume, asset, settings));

        ASSERT_EQ(asset->CoefficientData.size(), static_cast<size_t>(SH_COEFFICIENT_COUNT));
        // Written as a literal 0.0f — bit-exact by contract, hence float ==.
        EXPECT_EQ(asset->CoefficientData[0].w, 0.0f) << "a sealed probe must be flagged invalid";
    }

    TEST(LightProbePathTracedBake, TwoBakesAreBitIdentical)
    {
        const LightProbePathTracedBakeSettings settings = MakeBakeSettings();
        const BakedVolume first = BakeRoomVolume(settings);
        const BakedVolume second = BakeRoomVolume(settings);
        ASSERT_TRUE(first.Success);
        ASSERT_TRUE(second.Success);

        const auto& a = first.Asset->CoefficientData;
        const auto& b = second.Asset->CoefficientData;
        ASSERT_EQ(a.size(), b.size());
        ASSERT_FALSE(a.empty());
        // memcmp, deliberately: the determinism contract is bit-identity
        // (stateless per-probe seeds + fixed ascending accumulation order),
        // the same contract the path tracer and lightmap bake assert.
        EXPECT_EQ(std::memcmp(a.data(), b.data(), a.size() * sizeof(glm::vec4)), 0)
            << "two path-traced probe bakes of an identical room diverged — the bake is not deterministic";
    }

    TEST(LightProbePathTracedBake, RejectsNullAssetAndUnbuiltWorld)
    {
        ProbeRoom room = MakeProbeRoom();
        LightProbePathTracedBakeSettings settings;
        settings.SamplesPerProbe = 8; // failure paths must not trace anyway
        settings.MaxBounces = 1;

        // Null asset.
        Ref<LightProbeVolumeAsset> nullAsset;
        EXPECT_FALSE(LightProbeBaker::BakeVolumePathTraced(room.World, room.Volume, nullAsset, settings));

        // Unbuilt world.
        PathTracing::ReferenceScene unbuilt;
        auto asset = Ref<LightProbeVolumeAsset>::Create();
        EXPECT_FALSE(LightProbeBaker::BakeVolumePathTraced(unbuilt, room.Volume, asset, settings));
        EXPECT_TRUE(asset->CoefficientData.empty()) << "a rejected bake must not write coefficients";
    }
} // namespace OloEngine::Tests
