// =============================================================================
// FoliageGenerationEvidenceTest.cpp
//
// Visual + integration evidence for foliage auto-population from terrain
// generation rules. Builds a scene with a procedural TerrainComponent whose
// splatmap is auto-generated from the default sand/grass/rock/snow rules, plus a
// FoliageComponent whose layers come straight from
// TerrainGenerator::MakeFoliageLayersFromRules() — i.e. the foliage the generator
// emits to match those rules — and drives the FULL editor render pipeline
// (Scene::ProcessScene3DSharedLogic builds the height field, the auto-splat, and
// the foliage instances on a live GL context). Two things are checked:
//
//   1. The generated FoliageLayers scatter instances on the *generated* terrain
//      (FoliageRenderer::GetTotalInstanceCount() > 0) — the closed loop
//      "rules → matching FoliageLayers → placed vegetation", exercised against a
//      real GL TerrainData + splatmap. Camera-independent; this is the CI gate.
//
//   2. The grass billboards actually composite into the SceneColor render target
//      (the auto-material terrain + the grass.png cutout). We read SceneColor
//      directly — the foliage draw pass writes there before the post/UI composite
//      — and assert the frame contains a non-trivial number of "grass-green"
//      pixels (a framing-tolerant contract, not a brittle golden RMSE). In
//      OLOENGINE_GOLDEN_REBASE mode the frame is written to
//      OloEditor/assets/tests/visual/FoliageGen_grassland.png for a human to
//      eyeball.
//
// Runs in the normal suite and SKIPs cleanly (not fails) when there is no GL 4.6
// context — mirrors TerrainGenerationEvidenceTest / WaterVisualEvidenceTest.
//
// OLO_TEST_LAYER: L8
// =============================================================================

#include "OloEnginePCH.h"

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

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
            const char* v = std::getenv("OLOENGINE_GOLDEN_REBASE");
            return v && v[0] != '\0' && v[0] != '0';
        }

        // A pixel that reads as foliage/grass green: clearly green-dominant and
        // not near-black background. The grass.png blades + grass material both
        // land here; bare rock/snow/sand do not.
        [[nodiscard]] bool IsGrassGreen(u8 r, u8 g, u8 b)
        {
            const int ri = r;
            const int gi = g;
            const int bi = b;
            return gi > 40 && gi > ri + 12 && gi > bi + 12;
        }
    } // namespace

    class FoliageGenerationEvidenceTest : public RendererAttachedTest
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

            // Procedural terrain with auto-material default biome (sand/grass/rock/
            // snow). Gentle relief (low ridge, modest height) so a large grass band
            // exists for the foliage to land on — the same recipe as the
            // FoliageGenerationTest.olo sandbox demo scene.
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

                // Foliage emitted from the SAME rules — the feature under test.
                auto& foliage = m_TerrainEntity.AddComponent<FoliageComponent>();
                foliage.m_Enabled = true;
                foliage.m_Layers = TerrainGenerator::MakeFoliageLayersFromRules(terrain.m_LayerRules);
                foliage.m_NeedsRebuild = true;
            }
        }

        // Read the SceneColor render target (where the foliage pass composites,
        // before post/UI). Returns RGBA8.
        void CaptureSceneColor(std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 2000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            // Close, low oblique skimming just over the grassland surface so the
            // individual grass.png billboards stand against the ground (not just
            // the green auto-material seen from altitude).
            camera.SetPose(glm::vec3(128.0f, 34.0f, 162.0f), 0.0f, 0.2f);

            // Several ticks: build terrain + auto-splat + foliage instances, then
            // let the frame graph settle so the foliage pass composites.
            RunEditorFrames(camera, 4);

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

        Entity m_TerrainEntity;
    };

    TEST_F(FoliageGenerationEvidenceTest, GeneratedFoliageScattersAndRenders)
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

        std::vector<u8> frame;
        CaptureSceneColor(frame);

        // (1) The generator emitted layers and they placed instances on the real
        // generated terrain — the closed loop, camera-independent CI gate.
        ASSERT_TRUE(m_TerrainEntity && m_TerrainEntity.HasComponent<FoliageComponent>());
        auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();
        ASSERT_FALSE(foliage.m_Layers.empty()) << "MakeFoliageLayersFromRules emitted no layers for the default biome";
        ASSERT_TRUE(foliage.m_Renderer) << "foliage renderer was never created on the editor render path";
        EXPECT_GT(foliage.m_Renderer->GetTotalInstanceCount(), 0u)
            << "generated FoliageLayers scattered zero instances on the generated terrain — "
               "the rule→layer→scatter loop is broken";

        if (GoldenRebaseRequested())
            WritePng("FoliageGen_grassland.png", frame);

        // (2) The vegetated frame shows real grass-green coverage (foliage blades +
        // the grass auto-material both contribute). A flat/untextured terrain or a
        // frame where nothing composited collapses this to ~0.
        const int grassPixels = CountGrassGreen(frame);
        EXPECT_GT(grassPixels, static_cast<int>(kWidth * kHeight) / 200)
            << "only " << grassPixels << " grass-green pixels — the generated world did not come out vegetated";
    }
} // namespace OloEngine::Tests
