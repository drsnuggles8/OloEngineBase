// =============================================================================
// FoliageAuthoredMeshEvidenceTest.cpp
//
// Visual + integration evidence that a foliage layer renders its AUTHORED PLANT
// MESH up close and hands back to the flat card at distance (issue #1233).
// Drives the full editor render pipeline on a live GL context, with a real
// procedural terrain and the Drift pine (SandboxProject/Assets/Models/
// Vegetation/pine.obj — 36 vertices, base at origin, unit height).
//
// The pixel evidence is an A/B against a CONTROL, not a threshold on one frame:
// the same scene, same camera, same clock, rendered with UseAuthoredMesh on and
// off. What that buys is a falsifiable claim in both directions —
//
//   NEAR  the two frames must DIFFER substantially. A pine's silhouette is not
//         a quad's, so if they match, the mesh path drew a card (or drew
//         nothing) and the feature is not on screen no matter what the instance
//         counts say.
//   FAR   the two frames must MATCH. Past the hand-over band the mesh has
//         handed everything back to the card, so the authored-mesh switch must
//         make no difference out there. If it does, the mesh is still drawing
//         past its band, or the card was cut and nothing replaced it.
//
// One frame with a coverage threshold could not tell either of those apart.
//
// BOTH bounds are measured, not guessed. The far pose is captured TWICE through
// the same path to get the renderer's run-to-run noise floor
// (VisualEvidence::Rgba8Rmse over the pair), and then:
//   * NEAR is `ExpectCapturesAreDistinct` against that floor — the repo's guard
//     for "these two frames really are different";
//   * FAR must sit AT the floor, an absolute bound.
// The far bound used to be relative (`farDelta < nearDelta * 0.5`), which grows
// with the near difference and would pass a real far-field regression whenever
// the near silhouette was large enough — caught in review on this PR.
//
// Also checked, camera-independently (the CI gate):
//   * the layer emits a mesh draw AND a card draw over one instance stream,
//     carrying the SAME hand-over band — the partition's precondition;
//   * conservative bounds grew past a quad's, from the real geometry;
//   * the census reports the plants as AuthoredMesh, not as cards.
//
// In --olo-golden-rebase mode three views are written to
// OloEditor/assets/tests/visual/ for a human to eyeball: the near mesh, the near
// control, and the far hand-over.
//
// Runs in the normal suite and SKIPs cleanly (not fails) when there is no GL 4.6
// context — mirrors FoliageGenerationEvidenceTest / WaterVisualEvidenceTest.
//
// OLO_TEST_LAYER: L8
// =============================================================================

#include "OloEnginePCH.h"
#include <span>
#include "OloEngine/Containers/Array.h"
#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"
#include "VisualEvidenceGuards.h"

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

#include <algorithm>
#include <cmath>
#include <cstring>
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
        constexpr f32 kCaptureTime = 4.0f;

        // The Drift conifer, relative to OloEditor/ (the suite's working
        // directory) — the same path Woodland.olo and the impostor bake use.
        constexpr const char* kPineMesh = "SandboxProject/Assets/Models/Vegetation/pine.obj";
        constexpr const char* kFoliageAlbedo = "assets/textures/grass.png";

        // The hand-over band this test authors. Wide enough that the near camera
        // sits well inside it and the far camera well outside, so neither
        // assertion depends on where exactly the dither lands.
        constexpr f32 kMeshFadeStart = 55.0f;
        constexpr f32 kMeshViewDistance = 70.0f;

        [[nodiscard]] bool GoldenRebaseRequested()
        {
            return OloEngine::Tests::Options().GoldenRebase;
        }

        // Fraction of pixels whose colour differs perceptibly between two
        // frames. A count, not an RMSE: the question is "did the silhouette
        // change", and a count is framing-tolerant where an RMSE is not.
        [[nodiscard]] f64 DifferingFraction(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size() || a.empty())
                return 1.0;

            std::size_t differing = 0;
            std::size_t total = 0;
            for (std::size_t i = 0; i + 3 < a.size(); i += 4)
            {
                ++total;
                const int dr = std::abs(static_cast<int>(a[i + 0]) - static_cast<int>(b[i + 0]));
                const int dg = std::abs(static_cast<int>(a[i + 1]) - static_cast<int>(b[i + 1]));
                const int db = std::abs(static_cast<int>(a[i + 2]) - static_cast<int>(b[i + 2]));
                if (dr + dg + db > 24)
                    ++differing;
            }
            return total == 0 ? 1.0 : static_cast<f64>(differing) / static_cast<f64>(total);
        }
    } // namespace

    class FoliageAuthoredMeshEvidenceTest : public RendererAttachedTest
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
                terrain.m_ProceduralSeed = 11;
                terrain.m_ProceduralResolution = 128;
                terrain.m_ProceduralOctaves = 4;
                terrain.m_ProceduralFrequency = 1.5f;
                terrain.m_WorldSizeX = 256.0f;
                terrain.m_WorldSizeZ = 256.0f;
                // Low relief: the plants, not the hillside, have to be what
                // changes between the two arms.
                terrain.m_HeightScale = 6.0f;
                terrain.m_TessellationEnabled = false;
                terrain.m_Material = Ref<TerrainMaterial>::Create();
                for (const auto& layer : TerrainGenerator::MakeDefaultLayers())
                    terrain.m_Material->AddLayer(layer);

                auto& foliage = m_TerrainEntity.AddComponent<FoliageComponent>();
                foliage.m_Enabled = true;

                FoliageLayer pines;
                pines.Name = "Pines";
                pines.MeshPath = kPineMesh;
                pines.AlbedoPath = kFoliageAlbedo;
                pines.Density = 0.02f;
                pines.SplatmapChannel = -1;
                pines.MinSlopeAngle = 0.0f;
                pines.MaxSlopeAngle = 60.0f;
                pines.MinScale = 1.0f;
                pines.MaxScale = 1.0f;
                // Sizeable plants: the mesh and the card have to be
                // distinguishable at the near camera's distance.
                pines.MinHeight = 8.0f;
                pines.MaxHeight = 12.0f;
                pines.ViewDistance = 400.0f;
                pines.FadeStartDistance = 360.0f;
                pines.UseAuthoredMesh = true;
                pines.MeshViewDistance = kMeshViewDistance;
                pines.MeshFadeStartDistance = kMeshFadeStart;
                pines.AlphaCutoff = 0.25f;
                pines.WindStrength = 0.0f; // deterministic silhouette across the A/B
                pines.BaseColor = glm::vec3(0.18f, 0.42f, 0.14f);
                foliage.m_Layers.Add(pines);
                foliage.m_NeedsRebuild = true;
            }
        }

        void SetAuthoredMesh(bool on)
        {
            auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();
            for (auto& layer : foliage.m_Layers)
                layer.UseAuthoredMesh = on;
            foliage.m_NeedsRebuild = true;
        }

        // Renders from `eye` and reads SceneColor, where the foliage pass
        // composites before post/UI.
        void Capture(const glm::vec3& eye, f32 yaw, f32 pitch, std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 2000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(eye, yaw, pitch);
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

        Entity m_TerrainEntity;
    };

    TEST_F(FoliageAuthoredMeshEvidenceTest, AuthoredMeshDrawsUpCloseAndHandsBackToTheCard)
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

        // Standing among the plants, looking along the ground so the plants are
        // seen against the hillside rather than from above. Everything in the
        // foreground is inside the 55 m band start, so it is drawn as mesh.
        const glm::vec3 nearEye(128.0f, 12.0f, 150.0f);
        // Back off until EVERY plant is past the 70 m band end — the terrain
        // ends at z = 256, so from z = 360 at 60 m up the nearest plant is
        // ~120 m away. A "far" camera that still had plants inside the band
        // would make the second assertion vacuous.
        const glm::vec3 farEye(128.0f, 60.0f, 360.0f);

        std::vector<u8> nearMesh;
        std::vector<u8> farMesh;
        std::vector<u8> farMeshRepeat;
        Capture(nearEye, 0.0f, 0.12f, nearMesh);
        Capture(farEye, 0.0f, 0.30f, farMesh);
        // The SAME pose again, same arm, same code path: this pair is the
        // renderer's own run-to-run variance and nothing else, which is the only
        // number that separates "the frame is jittery" from "the frame changed".
        Capture(farEye, 0.0f, 0.30f, farMeshRepeat);

        // ── The camera-independent gate, before any pixel is looked at ───────
        ASSERT_TRUE(m_TerrainEntity && m_TerrainEntity.HasComponent<FoliageComponent>());
        auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();
        ASSERT_TRUE(foliage.m_Renderer);
        ASSERT_GT(foliage.m_Renderer->GetTotalInstanceCount(), 0u) << "no pines were scattered — the fixture is broken";

        const auto& census = foliage.m_Renderer->GetInstanceRegistry().GetCensus();
        EXPECT_GT(census.m_AuthoredMeshInstances, 0u)
            << "the pines are not counted as authored-mesh plants — either the mesh did not load (check "
               "OloEngine.log for the explicit diagnostic) or the near path never engaged";
        EXPECT_EQ(census.m_UnsupportedVariants, 0u)
            << "a layer asked for a representation it did not get; see OloEngine.log";

        const auto draws = foliage.m_Renderer->GetActiveLayerDrawInfo();
        ASSERT_GE(draws.Num(), 2u) << "a layer with an authored mesh must emit its mesh AND its card";

        u32 meshDraws = 0;
        u32 cardDraws = 0;
        u32 meshIndices = 0;
        for (const auto& draw : draws)
        {
            EXPECT_GT(draw.InstanceCount, 0u);
            if (draw.IsAuthoredMesh)
            {
                ++meshDraws;
                meshIndices += draw.IndexCount;
            }
            else
            {
                ++cardDraws;
                EXPECT_EQ(draw.IndexCount, 6u) << "the card draw is no longer the two-triangle quad";
            }
            // The precondition for the partition: both sides read the SAME band,
            // so the fraction one keeps is exactly the fraction the other drops.
            EXPECT_FLOAT_EQ(draw.MeshHandoverStartDistance, kMeshFadeStart);
            EXPECT_FLOAT_EQ(draw.MeshHandoverEndDistance, kMeshViewDistance);
        }
        EXPECT_GE(meshDraws, 1u);
        EXPECT_EQ(cardDraws, 1u);
        EXPECT_GT(meshIndices, 6u) << "the 'mesh' draw carries no more geometry than a quad";

        // Conservative bounds came from the real mesh: a pine 8-12 m tall with a
        // canopy is not bounded by a 1 m quad.
        const auto& records = foliage.m_Renderer->GetInstanceRegistry().GetRecords();
        ASSERT_FALSE(records.IsEmpty());
        bool sawWideBound = false;
        for (const auto& record : records)
        {
            const glm::vec3 extent = record.m_LocalBounds.Max - record.m_LocalBounds.Min;
            if (extent.x > record.m_Scale * 1.5f)
                sawWideBound = true;
        }
        EXPECT_TRUE(sawWideBound) << "every instance is still bounded like a quad — culling will pop the canopies";

        // ── The pixel A/B against the control ────────────────────────────────
        SetAuthoredMesh(false);
        std::vector<u8> nearCard;
        std::vector<u8> farCard;
        Capture(nearEye, 0.0f, 0.12f, nearCard);
        Capture(farEye, 0.0f, 0.30f, farCard);

        if (GoldenRebaseRequested())
        {
            WritePng("FoliageAuthoredMesh_near.png", nearMesh);
            WritePng("FoliageAuthoredMesh_near_control.png", nearCard);
            WritePng("FoliageAuthoredMesh_far.png", farMesh);
        }

        const f64 nearDelta = DifferingFraction(nearMesh, nearCard);
        const f64 farDelta = DifferingFraction(farMesh, farCard);

        // The measured noise floor, in the same RMSE units as the comparisons
        // below. Clamped off zero: a perfectly deterministic frame gives 0.0,
        // and a bound of "<= 0" would fail on the first least-significant bit
        // any driver or clock ever changes.
        const f64 noiseFloorRmse = std::max(VisualEvidence::Rgba8Rmse(farMesh, farMeshRepeat), 0.05);
        const f64 farRmse = VisualEvidence::Rgba8Rmse(farMesh, farCard);

        // Printed on success too: the measured separation between the two arms
        // is the evidence, and a number that quietly drifted toward the
        // threshold is the thing a pass/fail alone would hide.
        GTEST_LOG_(INFO) << "authored-mesh A/B: near " << nearDelta * 100.0 << "% of pixels differ, far "
                         << farDelta * 100.0 << "% | noise floor RMSE " << noiseFloorRmse << ", far RMSE " << farRmse;

        // NEAR: the repo's own distinctness guard, against the measured floor.
        VisualEvidence::ExpectCapturesAreDistinct({ nearMesh, nearCard }, { "near authored mesh", "near card control" },
                                                  noiseFloorRmse);
        EXPECT_GT(nearDelta, 0.05)
            << "up close the authored mesh and the flat card render the same frame (" << nearDelta * 100.0
            << "% of pixels differ) — the plant geometry is not reaching the screen";

        // FAR: an ABSOLUTE bound at the noise floor. Past the band the two arms
        // must be the same frame, so the only difference allowed is the jitter
        // measured above — not a fraction of however different the near frames
        // happened to be.
        constexpr f64 kNoiseMargin = 4.0;
        EXPECT_LE(farRmse, noiseFloorRmse * kNoiseMargin)
            << "past the hand-over band the authored-mesh switch still changes the frame (RMSE " << farRmse
            << " against a measured noise floor of " << noiseFloorRmse << " x " << kNoiseMargin
            << ") — the mesh is drawing beyond its band, or the card was cut with nothing to replace it";
        // Secondary, kept for the shape of the result: far must also be far
        // smaller than near in the pixel-count metric.
        EXPECT_LT(farDelta, nearDelta * 0.5)
            << "far " << farDelta * 100.0 << "% vs near " << nearDelta * 100.0 << "%";
    }
} // namespace OloEngine::Tests
