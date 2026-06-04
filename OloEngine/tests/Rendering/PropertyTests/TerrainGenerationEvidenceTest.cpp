// =============================================================================
// TerrainGenerationEvidenceTest.cpp
//
// Visual evidence + driver-independent contracts for procedural terrain
// generation with automatic material assignment (issue #113). Builds a scene
// with a single procedural TerrainComponent (ridged + domain-warped height
// field) whose splatmap is auto-generated from the default sand → grass → rock
// → snow height/slope rules, renders it through the FULL editor pipeline from
// an elevated angle + top-down, and writes each frame to
//   OloEditor/assets/tests/visual/TerrainGen_<pose>.png
//
// Why a render test on top of the CPU contracts (TerrainGeneratorTest.cpp):
// the math being right doesn't prove the terrain comes out *textured* — the
// splatmap has to reach the GPU and the terrain shader has to blend the layers.
// A generated terrain that renders as one flat colour is the exact failure this
// guards. So rather than a brittle golden RMSE, we assert framing-independent
// banding contracts: the rendered terrain must show clear multi-material colour
// variation (many distinct hue buckets + a wide luminance spread), not a single
// uniform surface. PNGs are written only in OLOENGINE_GOLDEN_REBASE mode for a
// human to eyeball; a normal run asserts the contracts and writes nothing.
//
// Runs in the normal suite and SKIPs cleanly (not fails) when there is no GL
// 4.6 context — mirrors WaterVisualEvidenceTest / SceneRenderEvidenceTest.
//
// Classification: L8 / integration (full GL pipeline + RGBA8 readback + PNG).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <array>
#include <cmath>
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
        constexpr f32 kCaptureTime = 8.0f;

        [[nodiscard]] bool GoldenRebaseRequested()
        {
            const char* v = std::getenv("OLOENGINE_GOLDEN_REBASE");
            return v && v[0] != '\0' && v[0] != '0';
        }

        [[nodiscard]] f32 Luma(u8 r, u8 g, u8 b)
        {
            return 0.2126f * static_cast<f32>(r) + 0.7152f * static_cast<f32>(g) + 0.0722f * static_cast<f32>(b);
        }
    } // namespace

    class TerrainGenerationEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            // Strong sun so slopes shade and the material bands read clearly.
            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 80.0f, 0.0f };
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.3f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 3.0f;
            }

            // Procedural terrain with auto-assigned material. Ridged + warped so
            // the field spans valleys (sand) → slopes (grass/rock) → peaks (snow).
            {
                m_TerrainEntity = scene.CreateEntity("Terrain");
                // Identity transform: the terrain mesh spans world X/Z ∈ [0, 256].
                // (Chunk frustum culling uses the chunks' local-space bounds, so a
                // translated transform would cull the terrain out of view even
                // though it would render — keep it at the origin for the capture.)

                auto& terrain = m_TerrainEntity.AddComponent<TerrainComponent>();
                terrain.m_ProceduralEnabled = true;
                terrain.m_ProceduralSeed = 20240611;
                terrain.m_ProceduralResolution = 192;
                terrain.m_ProceduralOctaves = 6;
                terrain.m_ProceduralFrequency = 3.0f;
                terrain.m_HeightShaping.RidgeBlend = 0.45f;
                terrain.m_HeightShaping.WarpStrength = 0.12f;
                terrain.m_WorldSizeX = 256.0f;
                terrain.m_WorldSizeZ = 256.0f;
                terrain.m_HeightScale = 60.0f;
                terrain.m_TessellationEnabled = false; // simplest path; chunk mesh is enough for evidence

                // Auto-material: default biome layers + rules.
                terrain.m_AutoMaterial = true;
                terrain.m_SplatmapGenResolution = 256;
                terrain.m_Material = Ref<TerrainMaterial>::Create();
                for (const auto& layer : TerrainGenerator::MakeDefaultLayers())
                    terrain.m_Material->AddLayer(layer);
                terrain.m_LayerRules = TerrainGenerator::MakeDefaultRules();
                terrain.m_MaterialNeedsRebuild = true;
                terrain.m_AutoSplatNeedsRebuild = true;
            }
        }

        void Capture(const std::string& poseName, const glm::vec3& position, f32 yaw, f32 pitch,
                     std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 2000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, yaw, pitch);

            // Two ticks: the first builds the terrain + auto-splat (m_NeedsRebuild),
            // the second renders the now-textured terrain.
            RunEditorFrames(camera, 2);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for pose '" << poseName << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

            // GL readback is bottom-up; flip so the saved PNG is right-side up.
            {
                const std::size_t rowBytes = static_cast<std::size_t>(kWidth) * 4u;
                std::vector<u8> tmp(rowBytes);
                for (u32 y = 0; y < kHeight / 2u; ++y)
                {
                    u8* top = outPixels.data() + static_cast<std::size_t>(y) * rowBytes;
                    u8* bot = outPixels.data() + static_cast<std::size_t>(kHeight - 1u - y) * rowBytes;
                    std::memcpy(tmp.data(), top, rowBytes);
                    std::memcpy(top, bot, rowBytes);
                    std::memcpy(bot, tmp.data(), rowBytes);
                }
            }

            if (GoldenRebaseRequested())
            {
                const fs::path dir = fs::path("assets") / "tests" / "visual";
                std::error_code ec;
                fs::create_directories(dir, ec);
                const std::string path = (dir / ("TerrainGen_" + poseName + ".png")).string();
                const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight),
                                                   4, outPixels.data(), static_cast<int>(kWidth) * 4);
                EXPECT_NE(wrote, 0) << "stbi_write_png failed to write '" << path << "'";
            }
        }

        // Count distinct coarse colour buckets (3 bits/channel) and the luminance
        // spread over "lit" pixels (brighter than near-black background). A flat /
        // single-material terrain collapses to a couple of buckets and a tiny
        // luminance spread; a properly textured one spans many.
        struct BandingStats
        {
            int DistinctBuckets = 0;
            f32 LumaSpread = 0.0f;
            int LitPixels = 0;
        };

        [[nodiscard]] static BandingStats AnalyzeBanding(const std::vector<u8>& px)
        {
            std::array<bool, 512> seen{}; // 8 levels per channel
            f32 lumaMin = 1e9f;
            f32 lumaMax = -1e9f;
            int lit = 0;
            for (std::size_t i = 0; i + 3 < px.size(); i += 4)
            {
                const u8 r = px[i + 0];
                const u8 g = px[i + 1];
                const u8 b = px[i + 2];
                const f32 l = Luma(r, g, b);
                if (l < 12.0f) // skip the near-black clear background
                    continue;
                ++lit;
                const int bucket = ((r >> 5) << 6) | ((g >> 5) << 3) | (b >> 5);
                seen[static_cast<std::size_t>(bucket)] = true;
                lumaMin = std::min(lumaMin, l);
                lumaMax = std::max(lumaMax, l);
            }
            BandingStats s;
            s.LitPixels = lit;
            for (bool v : seen)
                if (v)
                    ++s.DistinctBuckets;
            s.LumaSpread = (lit > 0) ? (lumaMax - lumaMin) : 0.0f;
            return s;
        }

        Entity m_TerrainEntity;
    };

    TEST_F(TerrainGenerationEvidenceTest, GeneratedTerrainIsTexturedAndBanded)
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

        // Elevated 3/4 view looking down the -Z axis across the terrain (which
        // spans world [0,256] in X/Z) — sees valleys, slopes and peaks at once,
        // so every material band is on screen. Camera centred on X=128, pulled
        // back past the far edge (Z=380 > 256) and tilted down. NOTE: with
        // EditorCamera::SetPose, *positive* pitch tilts the view DOWN.
        std::vector<u8> oblique;
        Capture("oblique", glm::vec3(128.0f, 160.0f, 380.0f), 0.0f, 0.48f, oblique);

        // Camera-independent contract on the generator output itself: the
        // auto-generated splatmap must assign at least two materials across the
        // surface (the height/slope rules produced real bands), not collapse to a
        // single layer. This catches a flat height field directly — e.g. the
        // large-seed f32-precision bug made every texel identical here long before
        // it showed up (or didn't) in the rendered frame.
        ASSERT_TRUE(m_TerrainEntity && m_TerrainEntity.HasComponent<TerrainComponent>());
        {
            auto& terrain = m_TerrainEntity.GetComponent<TerrainComponent>();
            ASSERT_TRUE(terrain.m_Material && terrain.m_Material->HasCPUSplatmaps());
            const auto& sp0 = terrain.m_Material->GetSplatmapData(0);
            std::array<int, 4> chanMin{ 255, 255, 255, 255 };
            std::array<int, 4> chanMax{ 0, 0, 0, 0 };
            for (std::size_t i = 0; i + 3 < sp0.size(); i += 4)
                for (int c = 0; c < 4; ++c)
                {
                    chanMin[static_cast<std::size_t>(c)] = std::min(chanMin[static_cast<std::size_t>(c)], static_cast<int>(sp0[i + c]));
                    chanMax[static_cast<std::size_t>(c)] = std::max(chanMax[static_cast<std::size_t>(c)], static_cast<int>(sp0[i + c]));
                }
            int variedChannels = 0;
            for (int c = 0; c < 4; ++c)
                if (chanMax[static_cast<std::size_t>(c)] - chanMin[static_cast<std::size_t>(c)] > 100)
                    ++variedChannels;
            EXPECT_GE(variedChannels, 2)
                << "auto-material splatmap assigned fewer than 2 materials — the height/slope "
                   "rules did not produce bands (flat height field?)";

            if (GoldenRebaseRequested())
            {
                const u32 sres = terrain.m_Material->GetSplatmapResolution();
                const fs::path dir = fs::path("assets") / "tests" / "visual";
                std::error_code ec;
                fs::create_directories(dir, ec);
                ::stbi_write_png((dir / "TerrainGen_splat0.png").string().c_str(), static_cast<int>(sres),
                                 static_cast<int>(sres), 4, sp0.data(), static_cast<int>(sres) * 4);
            }
        }

        const BandingStats stats = AnalyzeBanding(oblique);
        ASSERT_GT(stats.LitPixels, static_cast<int>(kWidth * kHeight) / 20)
            << "terrain barely covers the frame — camera framing is wrong, not a banding result";
        EXPECT_GE(stats.DistinctBuckets, 6)
            << "generated terrain shows too few distinct colours (" << stats.DistinctBuckets
            << ") — the auto-material splatmap likely did not reach the shader (flat terrain)";
        EXPECT_GT(stats.LumaSpread, 35.0f)
            << "luminance spread " << stats.LumaSpread << " too small — terrain looks like a single flat layer";

        // High oblique, near-top-down evidence pose (also written as a PNG in
        // rebase mode). Kept slightly off nadir to avoid the look-straight-down
        // basis singularity while still showing the layout from above.
        std::vector<u8> topDown;
        Capture("topdown", glm::vec3(128.0f, 300.0f, 210.0f), 0.0f, 1.25f, topDown);
        const BandingStats top = AnalyzeBanding(topDown);
        EXPECT_GE(top.DistinctBuckets, 6)
            << "top-down view shows too few distinct colours (" << top.DistinctBuckets << ")";
    }
} // namespace OloEngine::Tests
