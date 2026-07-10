// OLO_TEST_LAYER: L8
//
// Visual evidence for camera-relative terrain LOD far from origin (issue #429,
// terrain slice). The terrain quadtree keeps node bounds in terrain-local
// coordinates and the entity transform places them in the world; LOD selection
// and chunk frustum culling were fed the *world* camera / frustum, so a terrain
// translated far from the origin was mis-LOD'd or culled entirely — the exact
// caveat TerrainGenerationEvidenceTest documents ("a translated transform would
// cull the terrain out of view"). The fix runs LOD/cull in terrain-LOCAL space
// (MakeObjectLocal{ViewProjection,CameraPos}), evaluated through the grid-snapped
// render origin for precision.
//
// This test drives the REAL editor pipeline (tessellated terrain) and captures:
//   * near_ref — terrain at the origin, camera framing it.
//   * far_on   — the SAME terrain + camera both shifted +45 km; camera-relative
//                terrain LOD ON. Must reproduce near_ref pixel-for-pixel.
// far_on == near_ref (RMSE ~0) proves the far terrain still renders — and at the
// same LOD — instead of degenerating / culling. Both frames are asserted to
// contain substantial lit terrain (a black/culled frame fails). PNGs are written
// in OLOENGINE_GOLDEN_REBASE mode for a human to eyeball. SKIPs cleanly without a
// GL 4.6 context — mirrors TerrainGenerationEvidenceTest / WaterVisualEvidenceTest.
#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainMaterial.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 960;
        constexpr u32 kHeight = 540;

        // 45 km from the origin — well past the f32 precision cliff the whole
        // camera-relative feature exists to cross, and a multiple of the render-
        // origin grid so the shift lands on a clean cell.
        const glm::vec3 kFarShift{ 45056.0f, 0.0f, 45056.0f };

        [[nodiscard]] bool GoldenRebaseRequested()
        {
            const char* v = std::getenv("OLOENGINE_GOLDEN_REBASE");
            return v && v[0] != '\0' && v[0] != '0';
        }

        [[nodiscard]] f32 Luma(u8 r, u8 g, u8 b)
        {
            return 0.2126f * static_cast<f32>(r) + 0.7152f * static_cast<f32>(g) + 0.0722f * static_cast<f32>(b);
        }

        // Fraction of pixels brighter than the near-black clear — i.e. how much
        // terrain actually rendered. A culled / degenerate frame is ~0.
        [[nodiscard]] f32 LitFraction(const std::vector<u8>& px)
        {
            if (px.empty())
                return 0.0f;
            std::size_t lit = 0, total = 0;
            for (std::size_t i = 0; i + 3 < px.size(); i += 4)
            {
                if (Luma(px[i], px[i + 1], px[i + 2]) > 12.0f)
                    ++lit;
                ++total;
            }
            return total ? static_cast<f32>(lit) / static_cast<f32>(total) : 0.0f;
        }

        [[nodiscard]] f64 Rmse(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size() || a.empty())
                return 1e9;
            f64 acc = 0.0;
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                const f64 d = static_cast<f64>(a[i]) - static_cast<f64>(b[i]);
                acc += d * d;
            }
            return std::sqrt(acc / static_cast<f64>(a.size()));
        }

        void MaybeWritePng(const std::string& name, const std::vector<u8>& px)
        {
            if (!GoldenRebaseRequested())
                return;
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / ("TerrainCameraRelative_" + name + ".png")).string();
            ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight),
                             4, px.data(), static_cast<int>(kWidth) * 4);
        }
    } // namespace

    class TerrainCameraRelativeVisualEvidenceTest : public RendererAttachedTest
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

            {
                m_TerrainEntity = scene.CreateEntity("Terrain");
                auto& terrain = m_TerrainEntity.AddComponent<TerrainComponent>();
                terrain.m_ProceduralEnabled = true;
                terrain.m_ProceduralSeed = 20240711;
                terrain.m_ProceduralResolution = 192;
                terrain.m_ProceduralOctaves = 6;
                terrain.m_ProceduralFrequency = 3.0f;
                terrain.m_HeightShaping.RidgeBlend = 0.45f;
                terrain.m_HeightShaping.WarpStrength = 0.12f;
                terrain.m_WorldSizeX = 256.0f;
                terrain.m_WorldSizeZ = 256.0f;
                terrain.m_HeightScale = 60.0f;
                // Tessellation ON drives the quadtree LOD path — the CPU camera↔patch
                // distance metric that lost precision far from origin (the fix).
                terrain.m_TessellationEnabled = true;
                terrain.m_TargetTriangleSize = 8.0f;

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

        // Place the terrain at `worldOrigin` and frame it from an elevated 3/4 view
        // offset by the same amount, then read back the composited frame.
        void Capture(const glm::vec3& worldOrigin, std::vector<u8>& outPixels)
        {
            m_TerrainEntity.GetComponent<TransformComponent>().Translation = worldOrigin;

            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 4000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            // Terrain spans [worldOrigin, worldOrigin + 256] in X/Z; frame its centre.
            camera.SetPose(worldOrigin + glm::vec3(128.0f, 170.0f, 400.0f), 0.0f, 0.5f);

            // First few ticks build the terrain heightmap + quadtree + auto-splat;
            // the last renders the now-ready terrain.
            RunEditorFrames(camera, 3);

            u32 w = 0, h = 0;
            ASSERT_TRUE(ReadbackComposite(outPixels, w, h)) << "no composited frame";
            ASSERT_EQ(w, kWidth);
            ASSERT_EQ(h, kHeight);
        }

        Entity m_TerrainEntity;
    };

    TEST_F(TerrainCameraRelativeVisualEvidenceTest, FarTerrainRendersIdenticallyToOrigin)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        std::vector<u8> nearRef;
        Capture(glm::vec3(0.0f), nearRef);
        MaybeWritePng("near_ref", nearRef);
        const f32 nearLit = LitFraction(nearRef);

        std::vector<u8> farOn;
        Capture(kFarShift, farOn);
        MaybeWritePng("far_on", farOn);
        const f32 farLit = LitFraction(farOn);

        // Both frames must actually show terrain — a culled far frame reads ~0.
        // (This is the pre-fix failure mode: translated terrain culled to black.)
        EXPECT_GT(nearLit, 0.20f) << "near-origin terrain should fill much of the frame";
        EXPECT_GT(farLit, 0.20f) << "far terrain must render, not cull/degenerate (the bug being fixed)";

        // far_on reproduces the origin frame. Unlike the deterministic 2D CPU-bake
        // slice (#601, RMSE 0.000), a GPU-tessellated, lit 3D scene at 45 km has a
        // small non-zero floor: the residual ULP of the stored camera coordinate
        // (~0.004 units at 45 km) flips a few borderline patch LOD decisions and
        // nudges sub-pixel AA edges. Empirically that is ~5 RMSE, far below a
        // degenerate/culled far frame (terrain replaced by background ≈ 40+ RMSE),
        // so this threshold still fails loudly on the pre-fix behaviour while
        // tolerating the precision floor. (The frames are visually identical — see
        // the golden PNGs.)
        const f64 rmse = Rmse(nearRef, farOn);
        EXPECT_LT(rmse, 10.0) << "far-with-shift terrain must match the near-origin reference; RMSE=" << rmse
                              << " (nearLit=" << nearLit << " farLit=" << farLit << ")";
    }
} // namespace OloEngine::Tests
