// OLO_TEST_LAYER: L1
// =============================================================================
// LightmapBakeParityTest.cpp
//
// End-to-end HEADLESS verification of the lightmap bake (issue #439): a small
// colour-bleed room is baked through the full CPU pipeline — xatlas unwrap →
// atlas packing → UV2-space rasterization → PathTracer::EstimateIrradiance per
// texel → dilation → LightmapAsset — and the result is checked three ways:
//
//   1. DETERMINISM: the bake contract is bit-identical output for identical
//      input (texel seeds derive from atlas coordinates), so two bakes are
//      compared with memcmp, and a stored texel re-derived with the SAME seed
//      must match bit-exactly. EXPECT_EQ on floats is correct here precisely
//      because the contract is bit-identity, not approximate equality (see
//      docs/agent-rules/reference-path-tracer.md §2).
//   2. STATISTICAL CORRECTNESS: re-estimating a texel with an INDEPENDENT seed
//      must land near the stored value — the stored texel is a Monte Carlo
//      estimate of a well-defined integral, not an arbitrary number.
//   3. THE PHYSICAL SIGNATURE: floor texels beside a red wall must read redder
//      than floor texels beside a green wall (both lit by the same white
//      light) — the colour bleeding that indirect transport exists to produce,
//      asserted as an ORDERING so no tolerance can wash it out.
//
// No GPU, no ECS: the bake inputs and the reference world are built from the
// same hand-authored data (the §4 "both worlds from ONE description" rule),
// via ReferenceSceneBuilder::AddMeshEntity so the adapter is on the tested
// path too.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/Baking/LightmapBaker.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/PathTracing/PathSampler.h"
#include "OloEngine/Renderer/PathTracing/PathTracer.h"
#include "OloEngine/Renderer/PathTracing/ReferenceSceneBuilder.h"
#include "OloEngine/Scene/Components.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstring>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // A fresh unit cube MeshSource per call, via the engine's own
        // primitive factory (24 vertices, 36 indices, outward normals,
        // per-face UVs, one submesh). Headless-safe: MeshSource::Build()
        // defers GPU buffer creation when no graphics device exists and the
        // CPU-side data stays complete — the same way
        // SceneLightmapStalenessTest uses it. Every call yields a DISTINCT
        // MeshSource, so each entity's UV2 unwrap never aliases another's.
        Ref<MeshSource> MakeCube()
        {
            Ref<Mesh> cube = MeshPrimitives::CreateCube();
            return cube ? cube->GetMeshSource() : nullptr;
        }

        [[nodiscard]] glm::mat4 MakeTransform(const glm::vec3& translation, const glm::vec3& scale)
        {
            return glm::translate(glm::mat4(1.0f), translation) * glm::scale(glm::mat4(1.0f), scale);
        }

        // The colour-bleed room: a grey floor slab, a RED wall at -X, a GREEN
        // wall at +X, a grey ceiling to keep bounce energy in, and one white
        // point light in the middle. Small on purpose — every texel costs
        // SamplesPerTexel full paths in a Debug-built test.
        struct BleedRoom
        {
            struct Piece
            {
                u64 Uuid;
                glm::mat4 Transform;
                glm::vec3 BaseColor;
            };
            std::vector<Piece> Pieces;
            PointLightComponent Light;
            glm::vec3 LightPosition{ 0.0f, 1.6f, 0.0f };
        };

        BleedRoom MakeBleedRoom()
        {
            BleedRoom room;

            // Floor: 6 x 0.2 x 4 at y = -0.1 (top surface at y = 0)
            room.Pieces.push_back({ 0x1001, MakeTransform({ 0.0f, -0.1f, 0.0f }, { 6.0f, 0.2f, 4.0f }), { 0.6f, 0.6f, 0.6f } });
            // Red wall at -X (inner face at x = -3)
            room.Pieces.push_back({ 0x1002, MakeTransform({ -3.1f, 1.5f, 0.0f }, { 0.2f, 3.0f, 4.0f }), { 0.7f, 0.05f, 0.05f } });
            // Green wall at +X (inner face at x = +3)
            room.Pieces.push_back({ 0x1003, MakeTransform({ 3.1f, 1.5f, 0.0f }, { 0.2f, 3.0f, 4.0f }), { 0.05f, 0.7f, 0.05f } });
            // Ceiling
            room.Pieces.push_back({ 0x1004, MakeTransform({ 0.0f, 3.1f, 0.0f }, { 6.0f, 0.2f, 4.0f }), { 0.6f, 0.6f, 0.6f } });

            room.Light.m_Color = { 1.0f, 1.0f, 1.0f };
            room.Light.m_Intensity = 10.0f;
            room.Light.m_Range = 14.0f;
            room.Light.m_Attenuation = 2.0f;
            return room;
        }

        struct BakedRoom
        {
            LightmapBakeResult Result;
            LightmapBakePrepared Prepared;
            PathTracing::ReferenceScene World;
            LightmapBakeSettings Settings;
        };

        // One full pipeline run. `sharedMesh == true` shares one MeshSource
        // across all pieces (the editor's real shape for primitive rooms —
        // exercises unwrap idempotence); false gives each piece its own.
        BakedRoom BakeRoom(const BleedRoom& room, bool sharedMesh)
        {
            BakedRoom baked;

            baked.Settings.AtlasSize = 64;
            baked.Settings.MinRegionSize = 8;
            baked.Settings.SamplesPerTexel = 48; // budget: ~2.5k texels * 48 spp * <=3 bounces
            baked.Settings.MaxBounces = 3;
            baked.Settings.TexelsPerMeter = 1.5f;
            baked.Settings.DilationPasses = 2;
            baked.Settings.BakeKey = 0xB4CE'0439u;

            // Fresh MeshSources per bake (the second bake must see the same
            // PRE-unwrap data as the first to be comparable — the unwrap
            // mutates the mesh in place).
            Ref<MeshSource> shared = sharedMesh ? MakeCube() : nullptr;

            std::vector<LightmapBakeInput> inputs;
            PathTracing::ReferenceSceneBuilder builder;
            for (const auto& piece : room.Pieces)
            {
                LightmapBakeInput input;
                input.EntityUUID = piece.Uuid;
                input.Mesh = sharedMesh ? shared : MakeCube();
                input.WorldTransform = piece.Transform;
                inputs.push_back(input);
            }

            // BOTH worlds from the one description: the reference world uses the
            // same mesh + transform via the builder, with per-piece materials
            // supplied through a Material override.
            std::vector<Ref<Material>> materials; // keep alive until Build()
            for (sizet i = 0; i < room.Pieces.size(); ++i)
            {
                Ref<Material> material = Material::CreatePBR("BleedRoomPiece", room.Pieces[i].BaseColor, 0.0f, 0.9f);
                materials.push_back(material);
                builder.AddMeshEntity(inputs[i].Mesh, inputs[i].WorldTransform, material.get());
            }
            builder.AddPointLight(room.Light, room.LightPosition);

            baked.World = builder.Build(PathTracing::ReferenceSceneBuildOptions{});

            std::string error;
            const bool preparedOk = LightmapBaker::Prepare(inputs, baked.Settings, baked.Prepared, error);
            EXPECT_TRUE(preparedOk) << error;
            if (!preparedOk)
                return baked;

            baked.Result = LightmapBaker::BakeTexels(baked.Prepared, baked.World, baked.Settings);
            return baked;
        }

        [[nodiscard]] f32 MeanChannel(const glm::vec3& v)
        {
            return (v.x + v.y + v.z) / 3.0f;
        }
    } // namespace

    TEST(LightmapBakeParity, BakeSucceedsAndEveryTexelIsFiniteNonNegative)
    {
        const BleedRoom room = MakeBleedRoom();
        const BakedRoom baked = BakeRoom(room, /*sharedMesh=*/true);

        ASSERT_TRUE(baked.Result.Success) << baked.Result.Error;
        ASSERT_TRUE(baked.Result.Asset);
        EXPECT_EQ(baked.Result.BakedEntityCount, 4u);
        EXPECT_EQ(baked.Result.SkippedEntityCount, 0u);
        EXPECT_TRUE(baked.Result.Asset->Validate());
        EXPECT_EQ(baked.Result.Asset->GetBakeKey(), baked.Settings.BakeKey);
        EXPECT_EQ(baked.Result.Asset->GetEntries().size(), 4u);

        const auto& texels = baked.Result.Asset->GetTexelData();
        u32 bakedTexelCount = 0;
        for (sizet t = 0; t < texels.size(); t += 4)
        {
            // Validate() already checked finiteness; irradiance must also be
            // non-negative (a negative estimate means broken transport math).
            EXPECT_GE(texels[t + 0], 0.0f);
            EXPECT_GE(texels[t + 1], 0.0f);
            EXPECT_GE(texels[t + 2], 0.0f);
            if (texels[t + 3] > 0.0f)
                ++bakedTexelCount;
        }
        // The room's charts must cover a real fraction of the 64x64 atlas.
        EXPECT_GT(bakedTexelCount, 200u);
    }

    TEST(LightmapBakeParity, StoredTexelsMatchTheOracleBitExactlyAtTheSameSeed)
    {
        const BleedRoom room = MakeBleedRoom();
        const BakedRoom baked = BakeRoom(room, /*sharedMesh=*/true);
        ASSERT_TRUE(baked.Result.Success) << baked.Result.Error;

        PathTracing::PathTracerSettings tracer;
        tracer.SamplesPerPixel = baked.Settings.SamplesPerTexel;
        tracer.MaxBounces = baked.Settings.MaxBounces;
        tracer.RussianRouletteStartBounce = 0;
        tracer.Seed = baked.Settings.Seed;

        const auto& texels = baked.Result.Asset->GetTexelData();
        const u32 atlasSize = baked.Prepared.AtlasSize;

        // A spread of jobs across the atlas: the same seed must reproduce the
        // stored value bit-exactly — this pins the raster→estimate→store path.
        const sizet stride = std::max<sizet>(baked.Prepared.Jobs.size() / 16, 1);
        u32 checked = 0;
        for (sizet j = 0; j < baked.Prepared.Jobs.size(); j += stride)
        {
            const LightmapTexelJob& job = baked.Prepared.Jobs[j];
            const u32 seed = PathTracing::MakePixelSeed(job.AtlasX, job.AtlasY, baked.Settings.Seed);
            const glm::vec3 expected = PathTracing::PathTracer::EstimateIrradiance(
                baked.World, job.WorldPos, job.WorldNormal, tracer, seed);

            const sizet t = (static_cast<sizet>(job.AtlasY) * atlasSize + job.AtlasX) * 4;
            EXPECT_EQ(texels[t + 0], expected.x); // bit-identity is the contract, hence float ==
            EXPECT_EQ(texels[t + 1], expected.y);
            EXPECT_EQ(texels[t + 2], expected.z);
            ++checked;
        }
        EXPECT_GE(checked, 8u);
    }

    TEST(LightmapBakeParity, IndependentSeedReestimateLandsNearTheStoredTexel)
    {
        const BleedRoom room = MakeBleedRoom();
        const BakedRoom baked = BakeRoom(room, /*sharedMesh=*/true);
        ASSERT_TRUE(baked.Result.Success) << baked.Result.Error;

        PathTracing::PathTracerSettings tracer;
        tracer.SamplesPerPixel = 160; // more samples than the bake: the re-estimate is the better estimate
        tracer.MaxBounces = baked.Settings.MaxBounces;
        tracer.RussianRouletteStartBounce = 0;
        tracer.Seed = 0x5EED'D1FFu; // independent seed — a genuinely different sample set

        const auto& texels = baked.Result.Asset->GetTexelData();
        const u32 atlasSize = baked.Prepared.AtlasSize;

        // Compare REGION MEANS, not single texels: a 48-spp texel is a noisy
        // estimate and a per-texel band would either flake or be vacuous (the
        // "assert on region means" rule from reference-path-tracer.md §6).
        glm::vec3 storedSum(0.0f);
        glm::vec3 freshSum(0.0f);
        u32 count = 0;
        const sizet stride = std::max<sizet>(baked.Prepared.Jobs.size() / 24, 1);
        for (sizet j = 0; j < baked.Prepared.Jobs.size() && count < 24; j += stride)
        {
            const LightmapTexelJob& job = baked.Prepared.Jobs[j];
            const u32 seed = PathTracing::MakePixelSeed(job.AtlasX, job.AtlasY, tracer.Seed);
            freshSum += PathTracing::PathTracer::EstimateIrradiance(baked.World, job.WorldPos, job.WorldNormal, tracer, seed);
            const sizet t = (static_cast<sizet>(job.AtlasY) * atlasSize + job.AtlasX) * 4;
            storedSum += glm::vec3(texels[t + 0], texels[t + 1], texels[t + 2]);
            ++count;
        }
        ASSERT_GE(count, 12u);

        const f32 storedMean = MeanChannel(storedSum / static_cast<f32>(count));
        const f32 freshMean = MeanChannel(freshSum / static_cast<f32>(count));
        ASSERT_GT(freshMean, 1e-4f) << "room mean irradiance is ~zero — the fixture light is not reaching the surfaces";
        const f32 ratio = storedMean / freshMean;
        // 24 texels x 48 spp against 24 x 160 spp: the ratio's own noise is a
        // few percent; 0.85..1.18 separates "correct estimator" from any
        // systematic bias (a lost bounce, a wrong seed, a units slip) without
        // flaking on Monte Carlo noise.
        EXPECT_GT(ratio, 0.85f);
        EXPECT_LT(ratio, 1.18f);
    }

    TEST(LightmapBakeParity, FloorBleedsRedBesideTheRedWallAndGreenBesideTheGreenWall)
    {
        const BleedRoom room = MakeBleedRoom();
        const BakedRoom baked = BakeRoom(room, /*sharedMesh=*/true);
        ASSERT_TRUE(baked.Result.Success) << baked.Result.Error;

        const auto& texels = baked.Result.Asset->GetTexelData();
        const u32 atlasSize = baked.Prepared.AtlasSize;

        // Classify FLOOR texel jobs (upward normal, top surface) by which wall
        // they sit beside, and average each side.
        glm::vec3 nearRed(0.0f);
        glm::vec3 nearGreen(0.0f);
        u32 redCount = 0;
        u32 greenCount = 0;
        for (const auto& job : baked.Prepared.Jobs)
        {
            if (job.WorldNormal.y < 0.9f || std::abs(job.WorldPos.y) > 0.05f)
                continue; // not the floor's top surface
            const sizet t = (static_cast<sizet>(job.AtlasY) * atlasSize + job.AtlasX) * 4;
            const glm::vec3 e(texels[t + 0], texels[t + 1], texels[t + 2]);
            if (job.WorldPos.x < -2.0f)
            {
                nearRed += e;
                ++redCount;
            }
            else if (job.WorldPos.x > 2.0f)
            {
                nearGreen += e;
                ++greenCount;
            }
        }
        ASSERT_GT(redCount, 3u) << "no floor texels landed beside the red wall — raster/unwrap regression";
        ASSERT_GT(greenCount, 3u) << "no floor texels landed beside the green wall — raster/unwrap regression";

        nearRed /= static_cast<f32>(redCount);
        nearGreen /= static_cast<f32>(greenCount);

        // The ordering IS the physics: bounce light carries the wall albedo.
        // Both sides see the same white direct-on-wall light, so r/g ordering
        // cannot come from anything but indirect transport.
        EXPECT_GT(nearRed.r, nearRed.g * 1.15f)
            << "floor beside the RED wall is not red-shifted: r=" << nearRed.r << " g=" << nearRed.g;
        EXPECT_GT(nearGreen.g, nearGreen.r * 1.15f)
            << "floor beside the GREEN wall is not green-shifted: g=" << nearGreen.g << " r=" << nearGreen.r;
    }

    TEST(LightmapBakeParity, TwoBakesOfTheSameRoomAreBitIdentical)
    {
        const BleedRoom room = MakeBleedRoom();
        const BakedRoom first = BakeRoom(room, /*sharedMesh=*/true);
        const BakedRoom second = BakeRoom(room, /*sharedMesh=*/true);
        ASSERT_TRUE(first.Result.Success) << first.Result.Error;
        ASSERT_TRUE(second.Result.Success) << second.Result.Error;

        const auto& a = first.Result.Asset->GetTexelData();
        const auto& b = second.Result.Asset->GetTexelData();
        ASSERT_EQ(a.size(), b.size());
        // memcmp, deliberately: the determinism contract is bit-identity.
        EXPECT_EQ(std::memcmp(a.data(), b.data(), a.size() * sizeof(f32)), 0)
            << "two bakes of an identical room diverged — the bake is not deterministic";

        ASSERT_EQ(first.Result.Asset->GetEntries().size(), second.Result.Asset->GetEntries().size());
        for (sizet i = 0; i < first.Result.Asset->GetEntries().size(); ++i)
        {
            EXPECT_EQ(std::memcmp(&first.Result.Asset->GetEntries()[i], &second.Result.Asset->GetEntries()[i],
                                  sizeof(LightmapEntityEntry)),
                      0);
        }
    }

    TEST(LightmapBakeParity, DilationNeverBleedsAcrossAtlasRegions)
    {
        // Two colour-isolated floor+wall pairs, 60 m apart, with the coloured
        // walls placed BETWEEN the pairs so no sight line connects them: every
        // photon arriving at floor A bounced off the RED wall (albedo
        // 0.7/0.05/0.05) and every photon at floor B off the GREEN wall (both
        // lights are white, so only wall albedo can create an r/g asymmetry).
        // Every legitimately baked OR dilated texel in A's atlas region is
        // therefore strongly red-dominant and every one in B's strongly
        // green-dominant, and no texel outside either region may carry data.
        // Regression for the pre-region-aware dilation, which pulled an empty
        // border texel from ANY neighbouring texel — including another
        // entity's region and unallocated atlas space.
        constexpr u64 kRedFloorUuid = 0x2001;
        constexpr u64 kGreenFloorUuid = 0x2002;

        LightmapBakeSettings settings;
        settings.AtlasSize = 32;
        settings.MinRegionSize = 8;
        settings.SamplesPerTexel = 16; // colour DOMINANCE is the signal — cheap suffices
        settings.MaxBounces = 2;
        settings.TexelsPerMeter = 1.5f;
        settings.DilationPasses = 3; // extra passes push any bleed deeper into a region
        settings.BakeKey = 0xB1ED'0439u;

        std::vector<LightmapBakeInput> inputs;
        {
            LightmapBakeInput red;
            red.EntityUUID = kRedFloorUuid;
            red.Mesh = MakeCube();
            red.WorldTransform = MakeTransform({ -30.0f, -0.1f, 0.0f }, { 2.0f, 0.2f, 2.0f });
            inputs.push_back(red);

            LightmapBakeInput green;
            green.EntityUUID = kGreenFloorUuid;
            green.Mesh = MakeCube();
            green.WorldTransform = MakeTransform({ 30.0f, -0.1f, 0.0f }, { 2.0f, 0.2f, 2.0f });
            inputs.push_back(green);
        }

        // Reference world: the two baked floors plus WORLD-ONLY coloured walls
        // (not bake inputs) and one white range-limited light per pair. The
        // walls sit between the pairs (inner faces at x = -28.9 / +28.9), so
        // each wall also occludes the other pair from its floor — rays that
        // clear a wall's top rise far above the distant pair, and a ray
        // skirting a wall's z-edge diverges monotonically out of the ±1 m
        // corridor both pairs live in.
        PathTracing::ReferenceSceneBuilder builder;
        std::vector<Ref<Material>> materials; // keep alive until Build()
        auto addPiece = [&](const Ref<MeshSource>& mesh, const glm::mat4& transform, const glm::vec3& baseColor)
        {
            Ref<Material> material = Material::CreatePBR("RegionBleedPiece", baseColor, 0.0f, 0.9f);
            materials.push_back(material);
            builder.AddMeshEntity(mesh, transform, material.get());
        };
        addPiece(inputs[0].Mesh, inputs[0].WorldTransform, { 0.6f, 0.6f, 0.6f });
        addPiece(inputs[1].Mesh, inputs[1].WorldTransform, { 0.6f, 0.6f, 0.6f });
        addPiece(MakeCube(), MakeTransform({ -28.8f, 1.4f, 0.0f }, { 0.2f, 3.0f, 2.0f }), { 0.7f, 0.05f, 0.05f });
        addPiece(MakeCube(), MakeTransform({ 28.8f, 1.4f, 0.0f }, { 0.2f, 3.0f, 2.0f }), { 0.05f, 0.7f, 0.05f });

        PointLightComponent light;
        light.m_Color = { 1.0f, 1.0f, 1.0f };
        light.m_Intensity = 12.0f;
        light.m_Range = 10.0f; // far below the 60 m pair separation
        light.m_Attenuation = 2.0f;
        builder.AddPointLight(light, { -30.0f, 1.5f, 0.0f });
        builder.AddPointLight(light, { 30.0f, 1.5f, 0.0f });

        PathTracing::ReferenceScene world = builder.Build(PathTracing::ReferenceSceneBuildOptions{});

        LightmapBakePrepared prepared;
        std::string error;
        ASSERT_TRUE(LightmapBaker::Prepare(inputs, settings, prepared, error)) << error;
        ASSERT_EQ(prepared.Entries.size(), 2u);
        ASSERT_EQ(prepared.Regions.size(), 2u);

        const LightmapBakeResult result = LightmapBaker::BakeTexels(prepared, world, settings);
        ASSERT_TRUE(result.Success) << result.Error;

        const auto& texels = result.Asset->GetTexelData();
        const u32 atlasSize = prepared.AtlasSize;

        auto insideRegion = [](const LightmapAtlasRegion& region, u32 x, u32 y)
        {
            return x >= region.X && x < region.X + region.Size && y >= region.Y && y < region.Y + region.Size;
        };

        u32 checkedTexels[2] = { 0, 0 };
        for (u32 y = 0; y < atlasSize; ++y)
        {
            for (u32 x = 0; x < atlasSize; ++x)
            {
                const sizet t = (static_cast<sizet>(y) * atlasSize + x) * 4;
                if (texels[t + 3] <= 0.0f)
                    continue;

                // 1. Region-restricted dilation: data may only exist inside a
                //    baked entity's own rect.
                i32 owner = -1;
                for (sizet e = 0; e < prepared.Regions.size(); ++e)
                {
                    if (insideRegion(prepared.Regions[e], x, y))
                        owner = static_cast<i32>(e);
                }
                ASSERT_GE(owner, 0) << "baked/dilated texel (" << x << "," << y << ") lies outside every atlas region";
                ++checkedTexels[owner];

                // 2. Colour purity: every non-zero path contribution passed
                //    through the pair's wall albedo at least once, so the true
                //    minority:majority channel ratio is <= 1:14 per region. A
                //    factor-2 violation can only be the other region's colour
                //    family, dilated across the rect border.
                const f32 r = texels[t + 0];
                const f32 g = texels[t + 1];
                if (prepared.Entries[static_cast<sizet>(owner)].EntityUUID == kRedFloorUuid)
                {
                    EXPECT_LE(g, r * 0.5f + 1e-6f)
                        << "green-family texel inside the red floor's region at (" << x << "," << y << "): r=" << r << " g=" << g;
                }
                else
                {
                    EXPECT_LE(r, g * 0.5f + 1e-6f)
                        << "red-family texel inside the green floor's region at (" << x << "," << y << "): r=" << r << " g=" << g;
                }
            }
        }
        // Both regions must actually hold data or the assertions above are
        // vacuous (a raster/unwrap regression, not a dilation pass).
        EXPECT_GT(checkedTexels[0], 4u);
        EXPECT_GT(checkedTexels[1], 4u);
    }
} // namespace OloEngine::Tests
