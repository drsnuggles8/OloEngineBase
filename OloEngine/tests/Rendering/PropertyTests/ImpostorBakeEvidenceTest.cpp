// =============================================================================
// ImpostorBakeEvidenceTest.cpp
//
// Visual + contract evidence for the octahedral impostor ATLAS BAKE (issue
// #433) — the novel GPU piece. Bakes a cone (a cheap tree stand-in: distinctly
// different from the top vs the side, so the octahedral view-dependence is
// obvious) into an octahedral atlas via ImpostorBaker::Bake, reads both atlases
// back, and:
//
//   1. Asserts the albedo atlas has real coverage (the bake actually rasterised
//      the mesh into the tiles) — the CI gate.
//   2. Asserts the tiles are VIEW-DEPENDENT: per-tile coverage varies across the
//      grid (a top-down tile of a cone is a filled disc; a side tile is a
//      triangle). A flat billboard would make every tile identical — this is the
//      check that the impostor is genuinely 3D, not a repeated card.
//   3. In OLOENGINE_GOLDEN_REBASE mode writes the albedo + normal/depth atlases
//      to OloEditor/assets/tests/visual/ so a human can eyeball the tile grid.
//
// Runs in the normal suite and SKIPs cleanly (not fails) when there is no GL 4.6
// context — mirrors WaterVisualEvidenceTest / FoliageGenerationEvidenceTest.
//
// OLO_TEST_LAYER: L8
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Impostor/ImpostorBaker.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Texture.h"

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
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        [[nodiscard]] bool GoldenRebaseRequested()
        {
            const char* v = std::getenv("OLOENGINE_GOLDEN_REBASE");
            return v && v[0] != '\0' && v[0] != '0';
        }

        void WriteAtlasPng(const std::string& name, const std::vector<u8>& rgba, u32 size)
        {
            // GL readback is bottom-up; flip vertically for a natural PNG.
            std::vector<u8> flipped(rgba);
            const std::size_t rowBytes = static_cast<std::size_t>(size) * 4u;
            for (u32 y = 0; y < size / 2u; ++y)
            {
                u8* a = flipped.data() + static_cast<std::size_t>(y) * rowBytes;
                u8* b = flipped.data() + static_cast<std::size_t>(size - 1u - y) * rowBytes;
                for (std::size_t i = 0; i < rowBytes; ++i)
                    std::swap(a[i], b[i]);
            }
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ::stbi_write_png((dir / name).string().c_str(), static_cast<int>(size), static_cast<int>(size), 4,
                             flipped.data(), static_cast<int>(size) * 4);
        }
    } // namespace

    class ImpostorBakeEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            // The bake only needs a live GL context + the Impostor_Bake shader,
            // both provided by RendererAttachedTest::SetUpTestSuite (Renderer::Init).
            // No scene entities required.
        }
    };

    TEST_F(ImpostorBakeEvidenceTest, ConeBakesViewDependentOctahedralAtlas)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // A cone stands in for a tree: filled disc from the top, triangle from
        // the side — a strong view-dependence signal.
        Ref<Mesh> cone = MeshPrimitives::CreateCone(1.0f, 2.0f, 24);
        ASSERT_TRUE(cone) << "MeshPrimitives::CreateCone returned null";

        constexpr u32 N = 8;
        constexpr u32 kAtlasRes = 512; // 64px tiles — cheap, still clearly view-dependent
        ImpostorAtlas atlas = ImpostorBaker::Bake(
            cone, /*albedoTexture=*/nullptr, glm::vec3(0.25f, 0.55f, 0.15f),
            N, kAtlasRes, /*hemi=*/true, /*alphaCutoff=*/0.5f);

        ASSERT_TRUE(atlas.IsValid()) << "impostor bake produced an invalid atlas";
        EXPECT_EQ(atlas.FramesPerAxis, N);
        EXPECT_GT(atlas.Radius, 0.0f);

        const u32 size = atlas.Albedo->GetWidth();
        ASSERT_EQ(atlas.Albedo->GetHeight(), size);
        const u32 tileRes = size / N;
        ASSERT_GT(tileRes, 0u);

        std::vector<u8> albedo;
        ASSERT_TRUE(atlas.Albedo->GetData(albedo)) << "albedo atlas readback failed";
        ASSERT_EQ(albedo.size(), static_cast<std::size_t>(size) * size * 4u);

        std::vector<u8> normalDepth;
        ASSERT_TRUE(atlas.NormalDepth->GetData(normalDepth)) << "normal/depth atlas readback failed";

        // (1) Real coverage — the bake rasterised the mesh, not an empty atlas.
        auto coverageAt = [&](u32 px, u32 py) -> bool
        {
            const std::size_t idx = (static_cast<std::size_t>(py) * size + px) * 4u + 3u; // alpha
            return albedo[idx] > 40u;
        };
        std::size_t totalCovered = 0;
        for (u32 py = 0; py < size; ++py)
            for (u32 px = 0; px < size; ++px)
                if (coverageAt(px, py))
                    ++totalCovered;
        const f64 coverageFrac = static_cast<f64>(totalCovered) / (static_cast<f64>(size) * size);
        EXPECT_GT(coverageFrac, 0.02) << "impostor atlas is nearly empty — the bake did not render the mesh";

        // (2) View-dependence — per-tile coverage varies across the grid. A flat
        // billboard would make every tile identical (variance ~0).
        f64 minTile = 1.0;
        f64 maxTile = 0.0;
        for (u32 fy = 0; fy < N; ++fy)
        {
            for (u32 fx = 0; fx < N; ++fx)
            {
                std::size_t covered = 0;
                for (u32 ty = 0; ty < tileRes; ++ty)
                    for (u32 tx = 0; tx < tileRes; ++tx)
                        if (coverageAt(fx * tileRes + tx, fy * tileRes + ty))
                            ++covered;
                const f64 frac = static_cast<f64>(covered) / (static_cast<f64>(tileRes) * tileRes);
                minTile = std::min(minTile, frac);
                maxTile = std::max(maxTile, frac);
            }
        }
        EXPECT_GT(maxTile - minTile, 0.03)
            << "all octahedral tiles have near-identical coverage (" << minTile << ".." << maxTile
            << ") — the impostor is not view-dependent (baked like a flat billboard)";

        if (GoldenRebaseRequested())
        {
            WriteAtlasPng("Impostor_cone_albedo.png", albedo, size);
            WriteAtlasPng("Impostor_cone_normaldepth.png", normalDepth, size);
        }
    }

    // =========================================================================
    // Card-render evidence: the full runtime path (foliage instances scattered on
    // terrain, baked into an octahedral atlas, drawn as camera-facing impostor
    // cards that sample + blend the atlas and re-light). Renders the same field
    // from several azimuths so the "reads as a flat billboard off-axis" failure
    // mode is visible. Uses the DamagedHelmet mesh as a compact stand-in subject
    // (baked green via the tint — no per-mesh material plumbing yet).
    // =========================================================================
    namespace
    {
        constexpr u32 kSceneW = 1280;
        constexpr u32 kSceneH = 720;

        void WriteScenePng(const std::string& name, const std::vector<u8>& px, u32 w, u32 h)
        {
            std::vector<u8> flipped(px);
            const std::size_t rowBytes = static_cast<std::size_t>(w) * 4u;
            for (u32 y = 0; y < h / 2u; ++y)
            {
                u8* a = flipped.data() + static_cast<std::size_t>(y) * rowBytes;
                u8* b = flipped.data() + static_cast<std::size_t>(h - 1u - y) * rowBytes;
                for (std::size_t i = 0; i < rowBytes; ++i)
                    std::swap(a[i], b[i]);
            }
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ::stbi_write_png((dir / name).string().c_str(), static_cast<int>(w), static_cast<int>(h), 4,
                             flipped.data(), static_cast<int>(w) * 4);
        }

        // The impostors are tinted a distinctive magenta so they can be isolated
        // from the green/sand/snow terrain — a terrain-green detector would give a
        // false positive (the auto-material is green). Magenta appears nowhere in
        // the terrain palette, so counting it counts impostor-card pixels only.
        [[nodiscard]] bool IsImpostorMagenta(u8 r, u8 g, u8 b)
        {
            const int ri = r, gi = g, bi = b;
            return ri > 70 && bi > 70 && ri > gi + 30 && bi > gi + 30;
        }
    } // namespace

    class ImpostorCardRenderEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kSceneW, kSceneH);

            {
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.85f, -0.3f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 3.0f;
            }

            m_Terrain = scene.CreateEntity("Terrain");
            {
                auto& terrain = m_Terrain.AddComponent<TerrainComponent>();
                terrain.m_ProceduralEnabled = true;
                terrain.m_ProceduralSeed = 11;
                terrain.m_ProceduralResolution = 160;
                terrain.m_ProceduralOctaves = 4;
                terrain.m_ProceduralFrequency = 1.4f;
                terrain.m_WorldSizeX = 256.0f;
                terrain.m_WorldSizeZ = 256.0f;
                terrain.m_HeightScale = 14.0f;
                terrain.m_TessellationEnabled = false;
                terrain.m_AutoMaterial = true;
                terrain.m_SplatmapGenResolution = 128;
                terrain.m_Material = Ref<TerrainMaterial>::Create();
                for (const auto& layer : TerrainGenerator::MakeDefaultLayers())
                    terrain.m_Material->AddLayer(layer);
                terrain.m_LayerRules = TerrainGenerator::MakeDefaultRules();
                terrain.m_MaterialNeedsRebuild = true;
                terrain.m_AutoSplatNeedsRebuild = true;

                auto& foliage = m_Terrain.AddComponent<FoliageComponent>();
                foliage.m_Enabled = true;

                FoliageLayer trees;
                trees.Name = "ImpostorTrees";
                trees.MeshPath = "SandboxProject/Assets/Models/DamagedHelmet/DamagedHelmet.gltf";
                trees.AlbedoPath = "";      // white fallback -> tint
                trees.Density = 0.02f;      // a scattered field of distinct subjects
                trees.SplatmapChannel = -1; // uniform scatter (no splat mask needed)
                trees.MinSlopeAngle = 0.0f;
                trees.MaxSlopeAngle = 70.0f;
                trees.MinScale = 3.0f;
                trees.MaxScale = 4.5f;
                trees.MinHeight = 1.0f;
                trees.MaxHeight = 1.0f;
                trees.RandomRotation = true;
                trees.ViewDistance = 600.0f;
                trees.FadeStartDistance = 560.0f;
                trees.BaseColor = glm::vec3(0.90f, 0.12f, 0.90f); // distinctive magenta
                trees.AlphaCutoff = 0.4f;
                trees.UseImpostor = true;
                trees.ImpostorStartDistance = 12.0f;
                trees.ImpostorTransitionBand = 10.0f;
                trees.ImpostorFramesPerAxis = 8;
                trees.ImpostorAtlasResolution = 512;
                trees.ImpostorHemiOctahedral = true;
                trees.Enabled = true;
                foliage.m_Layers.push_back(trees);
                foliage.m_NeedsRebuild = true;
            }
        }

        // Capture SceneColor (where the foliage pass composites) from a posed
        // fly camera. Returns RGBA8, count of foliage-green pixels via out param.
        int CaptureAzimuth(const std::string& pngName, f32 yaw)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kSceneW) / static_cast<f32>(kSceneH), 0.5f, 2000.0f);
            camera.SetViewportSize(static_cast<f32>(kSceneW), static_cast<f32>(kSceneH));
            // Orbit the field centre and tilt down (positive pitch) so the
            // scattered impostors fill the frame in the 20-120 unit band (past
            // ImpostorStartDistance). Focus keeps a real focal point + distance.
            const glm::vec3 centre(128.0f, 14.0f, 128.0f);
            camera.Focus(centre, /*distance=*/70.0f, yaw, /*pitch=*/0.32f);

            RunEditorFrames(camera, 6);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            if (!fb)
                return -1;
            std::vector<u8> px;
            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kSceneW, kSceneH, px);
            if (px.size() != static_cast<std::size_t>(kSceneW) * kSceneH * 4u)
                return -1;

            if (GoldenRebaseRequested())
                WriteScenePng(pngName, px, kSceneW, kSceneH);

            int n = 0;
            for (std::size_t i = 0; i + 3 < px.size(); i += 4)
                if (IsImpostorMagenta(px[i + 0], px[i + 1], px[i + 2]))
                    ++n;
            return n;
        }

        Entity m_Terrain;
    };

    TEST_F(ImpostorCardRenderEvidenceTest, ScatteredImpostorCardsRenderFromMultipleAzimuths)
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
        } scopedMockTime(3.0f);

        // The layer scattered instances on the generated terrain, and the impostor
        // atlas baked from the mesh — the closed loop, camera-independent.
        ASSERT_TRUE(m_Terrain && m_Terrain.HasComponent<FoliageComponent>());
        // (One tick already ran through BuildScene's rebuild path via the first
        // capture below; assert scatter after the first render.)

        const int a0 = CaptureAzimuth("Impostor_field_az0.png", 0.0f);
        const int a1 = CaptureAzimuth("Impostor_field_az120.png", glm::radians(120.0f));
        const int a2 = CaptureAzimuth("Impostor_field_az240.png", glm::radians(240.0f));

        auto& foliage = m_Terrain.GetComponent<FoliageComponent>();
        ASSERT_TRUE(foliage.m_Renderer) << "foliage renderer never created on the render path";
        EXPECT_GT(foliage.m_Renderer->GetTotalInstanceCount(), 0u)
            << "impostor layer scattered zero instances on the generated terrain";

        // Impostor cards composited real green coverage from every azimuth — a
        // flat card that culled or failed to sample would collapse one of these.
        ASSERT_GE(a0, 0);
        ASSERT_GE(a1, 0);
        ASSERT_GE(a2, 0);
        const int minCoverage = static_cast<int>(kSceneW * kSceneH) / 2000; // ~460 px
        EXPECT_GT(a0, minCoverage) << "azimuth 0 shows almost no impostor foliage";
        EXPECT_GT(a1, minCoverage) << "azimuth 120 shows almost no impostor foliage";
        EXPECT_GT(a2, minCoverage) << "azimuth 240 shows almost no impostor foliage";
    }
} // namespace OloEngine::Tests
