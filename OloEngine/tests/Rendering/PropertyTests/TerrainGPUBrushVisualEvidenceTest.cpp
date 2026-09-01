// OLO_TEST_LAYER: L8
//
// Visual evidence for GPU-resident terrain sculpting and its undo (issue #716).
//
// The parity test (TerrainGPUBrushParityTest) proves the compute kernel writes the
// same height field the CPU brush did, and the undo-ring test proves a snapshot
// restores those texels byte for byte. Neither of them looks at a frame. What is
// still unproven by those two is the composition: that the terrain the renderer
// DRAWS comes from the texture the kernel wrote, and therefore that a stroke is
// visible at all and an undo actually puts the picture back.
//
// That gap is not academic here. Terrain geometry on the GPU-driven LOD path is
// generated in the vertex stage by sampling the heightmap; on the CPU path it
// comes from chunk MESHES that are rebuilt from the CPU mirror. This change makes
// the GPU texture authoritative and defers the mesh rebuild to stroke settle, so
// "the heightmap is correct" and "the screen shows it" became two different
// claims. A regression that severed them — a brush writing a scratch copy, an
// undo restoring a texture nothing samples — would leave every other test in this
// PR green.
//
// Three frames, one camera: before, after a stroke, after undoing it.
//
//   * StrokeChangesTheRenderedSurface — the after-frame differs from the before
//     frame over a region consistent with the brush footprint, not everywhere
//     (which would mean the camera moved) and not nowhere.
//   * UndoRestoresTheRenderedSurface — the undo-frame matches the before frame.
//     This is a blit-based undo of an R32F texture, so the restored HEIGHTS are
//     exact; the rendered frame is compared with a small tolerance because the
//     terrain LOD descent is camera-and-frame-dependent and need not re-select an
//     identical node set.
//
// SKIPs cleanly without a GL 4.6 context, like every other evidence test here.
#include "OloEnginePCH.h"
#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Terrain/Editor/TerrainBrush.h"
#include "OloEngine/Terrain/Editor/TerrainGPUBrush.h"
#include "OloEngine/Terrain/Editor/TerrainTextureUndoStack.h"
#include "OloEngine/Terrain/TerrainChunkManager.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainMaterial.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <cmath>
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

        constexpr f32 kWorldSize = 1024.0f;
        constexpr f32 kHeightScale = 120.0f;
        constexpr u32 kTerrainRes = 256;

        [[nodiscard]] bool GoldenRebaseRequested()
        {
            return OloEngine::Tests::Options().GoldenRebase;
        }

        // Fraction of pixels that differ by more than `threshold` on any channel.
        // A per-pixel count rather than an RMSE on purpose: a sculpt stroke moves a
        // COMPACT REGION a lot, and an RMSE over the whole frame would dilute that
        // into a number a camera nudge could equally produce.
        [[nodiscard]] f32 ChangedFraction(const std::vector<u8>& a, const std::vector<u8>& b, i32 threshold)
        {
            const sizet n = std::min(a.size(), b.size());
            if (n == 0)
                return 0.0f;
            sizet changed = 0, total = 0;
            for (sizet i = 0; i + 3 < n; i += 4)
            {
                const i32 dr = std::abs(static_cast<i32>(a[i]) - static_cast<i32>(b[i]));
                const i32 dg = std::abs(static_cast<i32>(a[i + 1]) - static_cast<i32>(b[i + 1]));
                const i32 db = std::abs(static_cast<i32>(a[i + 2]) - static_cast<i32>(b[i + 2]));
                if (dr > threshold || dg > threshold || db > threshold)
                    ++changed;
                ++total;
            }
            return total ? static_cast<f32>(changed) / static_cast<f32>(total) : 0.0f;
        }

        void MaybeWritePng(const std::string& name, const std::vector<u8>& px)
        {
            if (!GoldenRebaseRequested())
                return;
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / ("TerrainGpuBrush_" + name + ".png")).string();
            ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight),
                             4, px.data(), static_cast<int>(kWidth) * 4);
        }
    } // namespace

    class TerrainGPUBrushVisualEvidenceTest : public RendererAttachedTest
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
                terrain.m_ProceduralSeed = 716716;
                terrain.m_ProceduralResolution = kTerrainRes;
                terrain.m_ProceduralOctaves = 6;
                terrain.m_ProceduralFrequency = 3.0f;
                terrain.m_WorldSizeX = kWorldSize;
                terrain.m_WorldSizeZ = kWorldSize;
                terrain.m_HeightScale = kHeightScale;
                // The GPU authoring path is gated on the GPU-driven LOD path being
                // live — see the useGPU comment in TerrainEditorPanel. Without this
                // the editor would use the CPU brush and this file would be
                // testing the wrong thing while still passing.
                terrain.m_TessellationEnabled = true;

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

        // One fixed camera for all three frames — any difference between them is
        // then the terrain and nothing else.
        void Capture(std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 6000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(glm::vec3(512.0f, 420.0f, 512.0f), 0.0f, 1.50f);

            RunEditorFrames(camera, 4);

            u32 w = 0, h = 0;
            ASSERT_TRUE(ReadbackComposite(outPixels, w, h)) << "no composited frame";
            ASSERT_EQ(w, kWidth);
            ASSERT_EQ(h, kHeight);
        }

        Entity m_TerrainEntity;
    };

    TEST_F(TerrainGPUBrushVisualEvidenceTest, StrokeIsVisibleAndUndoRestoresTheFrame)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        TerrainGPUBrush brush;
        if (!brush.IsSculptReady())
        {
            GTEST_SKIP() << "Terrain_SculptBrush.comp did not compile in this environment";
        }

        std::vector<u8> before;
        ASSERT_NO_FATAL_FAILURE(Capture(before));
        MaybeWritePng("before", before);

        // Guard against the whole test being vacuous: if the fixture rendered no
        // terrain at all, every frame comparison below is a comparison of two blank
        // images, and "undo restored the frame" would pass for the worst possible
        // reason. Measured as channel variance rather than a luma band, so it does
        // not depend on the clear colour.
        {
            f64 sum = 0.0, sumSq = 0.0;
            sizet n = 0;
            for (sizet i = 0; i + 3 < before.size(); i += 4)
            {
                const f64 luma = 0.2126 * before[i] + 0.7152 * before[i + 1] + 0.0722 * before[i + 2];
                sum += luma;
                sumSq += luma * luma;
                ++n;
            }
            ASSERT_GT(n, 0u);
            const f64 mean = sum / static_cast<f64>(n);
            const f64 variance = (sumSq / static_cast<f64>(n)) - (mean * mean);
            ASSERT_GT(std::sqrt(std::max(variance, 0.0)), 4.0)
                << "the pre-stroke frame is nearly uniform (mean luma " << mean
                << "), so the fixture is not rendering terrain and every comparison below is vacuous";
        }

        auto& terrain = m_TerrainEntity.GetComponent<TerrainComponent>();
        ASSERT_TRUE(terrain.m_TerrainData) << "the terrain never built a heightmap";
        Ref<Texture2D> heightmap = terrain.m_TerrainData->GetGPUHeightmap();
        ASSERT_TRUE(heightmap);

        // Snapshot the whole map before the stroke, exactly as the panel does at
        // mouse-down, then sculpt a large raised dome in the middle of the frame.
        TerrainTextureUndoStack undoStack;
        const auto snapshot = undoStack.Capture(heightmap, 0, 0, kTerrainRes, kTerrainRes);
        ASSERT_NE(snapshot, TerrainTextureUndoStack::kInvalidSnapshot);

        TerrainBrushSettings settings;
        settings.Tool = TerrainBrushTool::Raise;
        settings.Radius = 220.0f;
        settings.Strength = 5.0f; // the panel's maximum
        settings.Falloff = 0.5f;

        // How many applies it takes to be VISIBLE, derived rather than guessed —
        // the first version of this test used 40 at strength 1 and moved 0.0066% of
        // pixels, because the brush raises by `influence / heightScale` per apply
        // and that is 1.3 world units on a 120-unit scale. Numerically obvious (the
        // parity test resolves 1e-4), optically nothing.
        //
        //   influence = min(1, weight * strength * dt) = min(1, 1 * 5 / 30) = 0.167
        //   per apply = influence / heightScale        = 0.167 / 120 = 0.00139
        //   300 applies                                = 0.42 normalized
        //                                              = ~50 world units of dome
        constexpr u32 kApplies = 300;
        constexpr f32 kApplyDt = 1.0f / 30.0f;

        const glm::vec3 centre{ kWorldSize * 0.5f, 0.0f, kWorldSize * 0.5f };
        for (u32 i = 0; i < kApplies; ++i)
        {
            const auto region = brush.ApplySculpt(*terrain.m_TerrainData, settings, centre, kWorldSize,
                                                  kWorldSize, kHeightScale, kApplyDt, 0.0f);
            ASSERT_GT(region.Width, 0u) << "the brush dispatched no work on iteration " << i;
        }

        // The stroke must be big enough on the HEIGHT FIELD before any claim about
        // pixels means anything. Asserting this separately is what tells a genuine
        // "the renderer is not sampling the brushed texture" failure apart from
        // "the stroke was too small to see", which is how this test first failed.
        {
            const std::vector<f32>& heights = terrain.m_TerrainData->GetHeightData();
            const sizet centreIndex = static_cast<sizet>(kTerrainRes / 2) * kTerrainRes + (kTerrainRes / 2);
            ASSERT_LT(centreIndex, heights.size());
            ASSERT_GT(heights[centreIndex] * kHeightScale, 20.0f)
                << "the stroke raised the centre by only " << (heights[centreIndex] * kHeightScale)
                << " world units — too small to be visible, so the pixel assertions below would be "
                   "measuring nothing";
        }

        // The chunk meshes are rebuilt at stroke settle on the GPU path; do the
        // same here so the CPU-LOD fallback geometry cannot be what is being
        // measured. This is also the stroke's single readback.
        if (terrain.m_ChunkManager && terrain.m_ChunkManager->IsBuilt())
        {
            TerrainBrush::DirtyRegion full{ 0, 0, kTerrainRes, kTerrainRes };
            TerrainBrush::RebuildDirtyChunks(*terrain.m_ChunkManager, *terrain.m_TerrainData, full,
                                             kWorldSize, kWorldSize, kHeightScale);
        }

        std::vector<u8> after;
        ASSERT_NO_FATAL_FAILURE(Capture(after));
        MaybeWritePng("after", after);

        // --- the stroke is on screen ----------------------------------------
        const f32 strokeChanged = ChangedFraction(before, after, 8);
        EXPECT_GT(strokeChanged, 0.02f)
            << "only " << (strokeChanged * 100.0f)
            << "% of pixels moved after a 220-unit raise stroke — the kernel wrote a texture the "
               "renderer is not sampling, which every non-visual test in this PR would still pass";
        EXPECT_LT(strokeChanged, 0.95f)
            << (strokeChanged * 100.0f)
            << "% of pixels moved, which is a whole-frame change rather than a brush footprint — "
               "the camera moved, or the terrain was rebuilt wholesale";

        // --- undo puts it back ------------------------------------------------
        ASSERT_TRUE(undoStack.Restore(snapshot, terrain.m_TerrainData->GetGPUHeightmap()));
        terrain.m_TerrainData->MarkGPUModified();
        if (terrain.m_ChunkManager && terrain.m_ChunkManager->IsBuilt())
        {
            TerrainBrush::DirtyRegion full{ 0, 0, kTerrainRes, kTerrainRes };
            TerrainBrush::RebuildDirtyChunks(*terrain.m_ChunkManager, *terrain.m_TerrainData, full,
                                             kWorldSize, kWorldSize, kHeightScale);
        }

        std::vector<u8> undone;
        ASSERT_NO_FATAL_FAILURE(Capture(undone));
        MaybeWritePng("undone", undone);

        const f32 undoResidual = ChangedFraction(before, undone, 8);
        EXPECT_LT(undoResidual, 0.02f)
            << (undoResidual * 100.0f)
            << "% of pixels still differ from the pre-stroke frame after undo. The heights are "
               "restored by an exact texture blit, so a residual this large means the restore "
               "reached the wrong rect, the wrong texture, or nothing the renderer samples.";

        // Anti-vacuous: the undo comparison above is only meaningful if the stroke
        // it is undoing actually moved more pixels than the tolerance allows.
        EXPECT_GT(strokeChanged, undoResidual * 4.0f)
            << "the stroke barely exceeded the undo tolerance, so 'undo restored the frame' is not "
               "distinguishable from 'nothing ever changed'";
    }
} // namespace OloEngine::Tests
