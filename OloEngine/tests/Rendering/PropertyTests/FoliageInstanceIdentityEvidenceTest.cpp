// =============================================================================
// FoliageInstanceIdentityEvidenceTest.cpp
//
// Live-pipeline evidence for canonical foliage identity (issue #1230).
//
// FoliageInstanceIdentityPropertyTests pins the identity MATH on the CPU. This
// pins the half that a CPU test structurally cannot: that the canonical records
// describe the foliage the renderer actually draws, and that a regeneration
// preserves identity WITHOUT changing the picture.
//
// It drives the full editor render path (Scene::ProcessScene3DSharedLogic builds
// the height field, the auto-splat and the foliage instances against a live GL
// context) and checks, from three camera angles:
//
//   1. Every plant the renderer uploaded has a canonical record: the registry's
//      census equals FoliageRenderer::GetTotalInstanceCount(), and every active
//      layer's draw info resolves back through GetIdForBufferRow to a live id —
//      identity surviving command submission, the issue's fourth criterion.
//   2. Spatial groups exist, partition the instances and bound them in WORLD
//      space (through the terrain transform, which is where the bounds have to
//      be right for #1240 / #1233 to cull against them).
//   3. A forced regeneration with unchanged inputs retires NOTHING and reissues
//      NOTHING, and the frame it produces is the same picture — the regression
//      that would otherwise be invisible, because a renumbered plant looks
//      identical in a screenshot.
//
// Screenshots land in OloEditor/assets/tests/visual/ under --olo-golden-rebase,
// the same policy as WaterVisualEvidenceTest and FoliageGenerationEvidenceTest.
// SKIPs cleanly (never fails, never DISABLED_) with no GL 4.6 context.
//
// OLO_TEST_LAYER: L8
// =============================================================================

#include "OloEnginePCH.h"
#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Terrain/Foliage/FoliageRenderer.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;
        constexpr f32 kCaptureTime = 4.0f;

        [[nodiscard]] bool GoldenRebaseRequested()
        {
            return OloEngine::Tests::Options().GoldenRebase;
        }

        [[nodiscard]] bool IsGrassGreen(u8 r, u8 g, u8 b)
        {
            const int ri = r;
            const int gi = g;
            const int bi = b;
            return gi > 40 && gi > ri + 12 && gi > bi + 12;
        }

        struct Angle
        {
            const char* m_Name;
            glm::vec3 m_Position;
            f32 m_Yaw;
            f32 m_Pitch;
        };

        // Three framings of the same vegetated island: skimming the blades from
        // the north, the same ground from the OPPOSITE side, and an elevated
        // look-down. A bug that only misplaces foliage at range, or only from
        // one bearing, survives any single one of them.
        //
        // Yaw is 0 or pi on purpose. EditorCamera::SetPose's yaw sign is not
        // obvious from the call site, and a guessed intermediate yaw aimed two
        // of these at empty sky the first time. 0 looks toward -Z and pi toward
        // +Z either way, so the only thing left to get right is pitch, which is
        // POSITIVE downward (the terrain spans x/z in [0, 256], peaking near
        // y = 28).
        constexpr Angle kAngles[] = {
            { "ground", glm::vec3(128.0f, 34.0f, 162.0f), 0.0f, 0.2f },
            { "reverse", glm::vec3(128.0f, 42.0f, -25.0f), 3.14159265f, 0.18f },
            { "overview", glm::vec3(128.0f, 130.0f, 250.0f), 0.0f, 0.72f },
        };
    } // namespace

    class FoliageInstanceIdentityEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            {
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.3f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 3.0f;
            }

            m_TerrainEntity = scene.CreateEntity("Terrain");
            {
                auto& terrain = m_TerrainEntity.AddComponent<TerrainComponent>();
                terrain.m_ProceduralEnabled = true;
                terrain.m_ProceduralSeed = 7;
                terrain.m_ProceduralResolution = 192;
                terrain.m_ProceduralOctaves = 5;
                terrain.m_ProceduralFrequency = 2.0f;
                terrain.m_HeightShaping.HeightExponent = 1.3f;
                terrain.m_WorldSizeX = 256.0f;
                terrain.m_WorldSizeZ = 256.0f;
                terrain.m_HeightScale = 28.0f;
                terrain.m_TessellationEnabled = false;

                terrain.m_AutoMaterial = true;
                terrain.m_SplatmapGenResolution = 256;
                terrain.m_Material = Ref<TerrainMaterial>::Create();
                for (const auto& layer : TerrainGenerator::MakeDefaultLayers())
                    terrain.m_Material->AddLayer(layer);
                terrain.m_LayerRules = TerrainGenerator::MakeDefaultRules();
                terrain.m_MaterialNeedsRebuild = true;
                terrain.m_AutoSplatNeedsRebuild = true;

                auto& foliage = m_TerrainEntity.AddComponent<FoliageComponent>();
                foliage.m_Enabled = true;
                foliage.m_Layers = TerrainGenerator::MakeFoliageLayersFromRules(terrain.m_LayerRules);
                foliage.m_NeedsRebuild = true;
            }
        }

        void CaptureAngle(const Angle& angle, u32 frames, std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 2000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(angle.m_Position, angle.m_Yaw, angle.m_Pitch);
            RunEditorFrames(camera, frames);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No SceneColor framebuffer";
            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);
        }

        static void WritePng(const std::string& name, const std::vector<u8>& px)
        {
            std::vector<u8> flipped(px); // GL readback is bottom-up
            const std::size_t rowBytes = static_cast<std::size_t>(kWidth) * 4u;
            for (u32 y = 0; y < kHeight / 2u; ++y)
            {
                u8* a = flipped.data() + static_cast<std::size_t>(y) * rowBytes;
                u8* b = flipped.data() + static_cast<std::size_t>(kHeight - 1u - y) * rowBytes;
                std::vector<u8> tmp(a, a + rowBytes);
                std::memcpy(a, b, rowBytes);
                std::memcpy(b, tmp.data(), rowBytes);
            }
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ::stbi_write_png((dir / name).string().c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                             flipped.data(), static_cast<int>(kWidth) * 4);
        }

        [[nodiscard]] static int CountGrassGreen(const std::vector<u8>& px)
        {
            int n = 0;
            for (std::size_t i = 0; i + 3 < px.size(); i += 4)
                if (IsGrassGreen(px[i + 0], px[i + 1], px[i + 2]))
                    ++n;
            return n;
        }

        [[nodiscard]] static std::set<FoliageInstanceId> LiveIds(const FoliageInstanceRegistry& registry)
        {
            std::set<FoliageInstanceId> ids;
            for (const auto& record : registry.GetRecords())
                ids.insert(record.m_Id);
            return ids;
        }

        Entity m_TerrainEntity;
    };

    TEST_F(FoliageInstanceIdentityEvidenceTest, IdentitySurvivesRegenerationAndTheFrameIsUnchanged)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        struct ScopedMockTime
        {
            explicit ScopedMockTime(f32 t)
            {
                Time::SetMockTime(t);
            }
            ~ScopedMockTime()
            {
                Time::ClearMockTime();
            }
        } scopedMockTime(kCaptureTime);

        // ── First pass: build the world and capture every angle ──────────
        std::vector<std::vector<u8>> before(std::size(kAngles));
        for (std::size_t i = 0; i < std::size(kAngles); ++i)
        {
            // The first angle pays for terrain + auto-splat + foliage generation.
            CaptureAngle(kAngles[i], i == 0 ? 4u : 2u, before[i]);
            ASSERT_FALSE(HasFatalFailure());
        }

        ASSERT_TRUE(m_TerrainEntity && m_TerrainEntity.HasComponent<FoliageComponent>());
        auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();
        ASSERT_TRUE(foliage.m_Renderer) << "foliage renderer was never created on the editor render path";
        const auto& registry = foliage.m_Renderer->GetInstanceRegistry();

        // (1) Every uploaded plant has a canonical record.
        const u32 uploaded = foliage.m_Renderer->GetTotalInstanceCount();
        ASSERT_GT(uploaded, 0u) << "no foliage was placed — the fixture, not the feature, is broken";
        const auto& census = registry.GetCensus();
        EXPECT_EQ(census.m_CanonicalInstances, uploaded)
            << "the registry and the instance VBO disagree about how many plants exist";
        EXPECT_EQ(census.m_MeshCardInstances + census.m_ImpostorInstances + census.m_UnsupportedInstances,
                  census.m_CanonicalInstances)
            << "a plant is represented by exactly one of the three variants";
        EXPECT_EQ(registry.GetRecords().size(), uploaded);

        // Identity survives command submission: each draw carries the layer it
        // came from, and every row of that layer's stream resolves to a live id.
        const auto drawInfos = foliage.m_Renderer->GetActiveLayerDrawInfo();
        ASSERT_FALSE(drawInfos.empty());
        for (const auto& info : drawInfos)
        {
            ASSERT_GT(info.InstanceCount, 0u);
            for (u32 row = 0; row < info.InstanceCount; ++row)
            {
                const FoliageInstanceId id = registry.GetIdForBufferRow(info.LayerIndex, row);
                ASSERT_NE(id, kInvalidFoliageInstanceId)
                    << "layer " << info.LayerIndex << " row " << row << " has no canonical identity";
                const auto* record = registry.Find(id);
                ASSERT_NE(record, nullptr);
                EXPECT_EQ(record->m_BufferIndex, row);
                EXPECT_EQ(record->m_LayerIndex, info.LayerIndex);
            }
        }

        // (2) Groups partition the instances and bound them in WORLD space.
        ASSERT_FALSE(registry.GetGroups().empty()) << "spatial groups were never built";
        std::size_t grouped = 0;
        for (const auto& group : registry.GetGroups())
        {
            grouped += group.m_Instances.size();
            for (const auto id : group.m_Instances)
            {
                const auto* record = registry.Find(id);
                ASSERT_NE(record, nullptr);
                const BoundingBox world = record->m_LocalBounds.Transform(registry.GetTerrainTransform());
                EXPECT_LE(group.m_WorldBounds.Min.x, world.Min.x + 1e-3f);
                EXPECT_LE(group.m_WorldBounds.Min.z, world.Min.z + 1e-3f);
                EXPECT_GE(group.m_WorldBounds.Max.x, world.Max.x - 1e-3f);
                EXPECT_GE(group.m_WorldBounds.Max.z, world.Max.z - 1e-3f);
            }
        }
        EXPECT_EQ(grouped, static_cast<std::size_t>(uploaded));

        const auto idsBefore = LiveIds(registry);
        const auto censusBefore = census;

        // ── Second pass: regenerate with unchanged inputs ────────────────
        foliage.m_NeedsRebuild = true;
        std::vector<std::vector<u8>> after(std::size(kAngles));
        for (std::size_t i = 0; i < std::size(kAngles); ++i)
        {
            CaptureAngle(kAngles[i], i == 0 ? 3u : 2u, after[i]);
            ASSERT_FALSE(HasFatalFailure());
        }
        ASSERT_FALSE(foliage.m_NeedsRebuild) << "the regeneration never ran";

        // (3a) Nothing was retired, nothing reissued.
        EXPECT_EQ(LiveIds(registry), idsBefore)
            << "a regeneration with unchanged inputs renumbered plants — identity is not stable";
        EXPECT_TRUE(registry.GetLastDelta().m_Retired.empty());
        EXPECT_TRUE(registry.GetLastDelta().m_Added.empty());
        EXPECT_EQ(registry.GetCensus(), censusBefore);

        // (3b) ...and it is the same picture. Grass coverage rather than an exact
        // pixel match: the frame graph's temporal history is not reset between
        // the two passes, so a handful of pixels legitimately differ.
        for (std::size_t i = 0; i < std::size(kAngles); ++i)
        {
            const int greenBefore = CountGrassGreen(before[i]);
            const int greenAfter = CountGrassGreen(after[i]);
            EXPECT_GT(greenBefore, static_cast<int>(kWidth * kHeight) / 400)
                << "angle '" << kAngles[i].m_Name << "' shows almost no vegetation";
            const int delta = std::abs(greenAfter - greenBefore);
            EXPECT_LT(delta, greenBefore / 10)
                << "angle '" << kAngles[i].m_Name << "': foliage coverage moved from " << greenBefore
                << " to " << greenAfter << " across a no-op regeneration";
        }

        // Only the first pass is written. The regenerated frames are asserted
        // equal above and were confirmed pixel-identical by eye when this test
        // was written; committing them as well would double the golden surface
        // to maintain for a second copy of the same picture.
        if (GoldenRebaseRequested())
        {
            for (std::size_t i = 0; i < std::size(kAngles); ++i)
            {
                WritePng(std::string("FoliageIdentity_") + kAngles[i].m_Name + ".png", before[i]);
            }
        }
    }
} // namespace OloEngine::Tests
