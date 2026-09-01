// OLO_TEST_LAYER: L3
//
// GPU terrain authoring brush, undo ring, and the CPU/GPU height sync point
// (issue #716). Drives the REAL kernels on a live GL 4.6 context and SKIPs
// cleanly without one.
//
// The oracle is the CPU brush that shipped before this change. That is the point
// of the exercise: moving a per-texel loop into a compute kernel is a
// transliteration, and the way transliterations fail is not "it does nothing" —
// it is that the two halves agree on the FUNCTION and disagree about which SPACE
// its arguments live in (docs/agent-rules/cpu-gpu-surface-parity.md). A brush
// radius measured in texels instead of world units, or a falloff distance taken
// before the world-size scale, produces a perfectly plausible round dab that is
// the wrong size — and on a square terrain with equal world sizes it is even the
// RIGHT size, which is why the parity cases below deliberately use a NON-SQUARE
// world (WorldSizeX != WorldSizeZ). That asymmetry is the only thing separating a
// correct implementation from the most likely wrong one.
//
// The sync-point tests cover the other half of the issue: with the GPU
// authoritative, a CPU consumer that reads a stale mirror gets a terrain that
// looks right and gameplay that disagrees with it. Those failures are silent, so
// they are asserted directly rather than inferred from a rendered frame.

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Terrain/Editor/TerrainBrush.h"
#include "OloEngine/Terrain/Editor/TerrainErosion.h"
#include "OloEngine/Terrain/Editor/TerrainGPUBrush.h"
#include "OloEngine/Terrain/Editor/TerrainPaintBrush.h"
#include "OloEngine/Terrain/Editor/TerrainTextureUndoStack.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainLayer.h"
#include "OloEngine/Terrain/TerrainMaterial.h"

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Tests;

namespace
{
    constexpr u32 kResolution = 129;

    // Deliberately NOT square. A brush is a circle in WORLD space, so on this
    // terrain it is an ellipse in texel space; an implementation that measured the
    // radius in texels would produce a circle here and pass on a square terrain.
    constexpr f32 kWorldSizeX = 256.0f;
    constexpr f32 kWorldSizeZ = 160.0f;
    constexpr f32 kHeightScale = 64.0f;

    // A field with structure in both axes, so a Smooth pass has something to
    // average and a transposed index would not cancel out.
    std::vector<f32> MakeTestField()
    {
        std::vector<f32> heights(static_cast<sizet>(kResolution) * kResolution);
        for (u32 z = 0; z < kResolution; ++z)
        {
            for (u32 x = 0; x < kResolution; ++x)
            {
                const f32 fx = static_cast<f32>(x) / static_cast<f32>(kResolution - 1);
                const f32 fz = static_cast<f32>(z) / static_cast<f32>(kResolution - 1);
                const f32 v = 0.35f + 0.2f * std::sin(fx * 11.0f) * std::cos(fz * 7.0f) + 0.15f * fz;
                heights[static_cast<sizet>(z) * kResolution + x] = std::clamp(v, 0.0f, 1.0f);
            }
        }
        return heights;
    }

    Ref<TerrainData> MakeTerrain()
    {
        Ref<TerrainData> data = Ref<TerrainData>::Create();
        data->SetHeights(kResolution, MakeTestField());
        return data;
    }

    // Largest absolute difference between two height fields, and where it is.
    struct FieldDiff
    {
        f32 MaxAbs = 0.0f;
        sizet Index = 0;
    };

    FieldDiff CompareFields(const std::vector<f32>& a, const std::vector<f32>& b)
    {
        FieldDiff diff;
        const sizet n = std::min(a.size(), b.size());
        for (sizet i = 0; i < n; ++i)
        {
            const f32 d = std::abs(a[i] - b[i]);
            if (d > diff.MaxAbs)
            {
                diff.MaxAbs = d;
                diff.Index = i;
            }
        }
        return diff;
    }

    // Number of texels that actually moved, so a "parity" pass cannot be satisfied
    // by two implementations that both did nothing.
    u32 CountChanged(const std::vector<f32>& before, const std::vector<f32>& after)
    {
        u32 changed = 0;
        const sizet n = std::min(before.size(), after.size());
        for (sizet i = 0; i < n; ++i)
        {
            if (std::abs(after[i] - before[i]) > 1e-6f)
                ++changed;
        }
        return changed;
    }

    Ref<TerrainMaterial> MakeMaterial(u32 layerCount)
    {
        Ref<TerrainMaterial> material = Ref<TerrainMaterial>::Create();
        for (u32 i = 0; i < layerCount; ++i)
        {
            TerrainLayer layer;
            layer.Name = "Layer" + std::to_string(i);
            material->AddLayer(layer);
        }
        material->InitializeCPUSplatmaps(kResolution);
        return material;
    }
} // namespace

// -----------------------------------------------------------------------------
// Brush parity — the GPU kernel against the CPU brush it replaces
// -----------------------------------------------------------------------------

class TerrainGPUBrushParity : public ::testing::TestWithParam<TerrainBrushTool>
{
};

TEST_P(TerrainGPUBrushParity, MatchesTheCpuBrushOnANonSquareTerrain)
{
    OLO_ENSURE_GPU_OR_SKIP();

    TerrainGPUBrush gpuBrush;
    if (!gpuBrush.IsSculptReady())
    {
        GTEST_SKIP() << "Terrain_SculptBrush.comp did not compile in this environment";
    }

    TerrainBrushSettings settings;
    settings.Tool = GetParam();
    settings.Radius = 37.0f;
    settings.Strength = 0.8f;
    settings.Falloff = 0.6f;

    // Off-centre and off-grid: a centre that lands exactly on a texel hides an
    // off-by-half-texel sampling-convention error, which is the other classic
    // CPU/GPU disagreement in this area.
    const glm::vec3 worldPos{ 101.3f, 0.0f, 63.7f };
    constexpr f32 kDeltaTime = 1.0f / 60.0f;

    // --- CPU reference -------------------------------------------------------
    Ref<TerrainData> cpuTerrain = MakeTerrain();
    const std::vector<f32> original = cpuTerrain->GetHeightData();
    const f32 targetHeight = cpuTerrain->GetHeightAt(worldPos.x / kWorldSizeX, worldPos.z / kWorldSizeZ);

    const TerrainBrush::DirtyRegion cpuRegion =
        TerrainBrush::Apply(*cpuTerrain, settings, worldPos, kWorldSizeX, kWorldSizeZ, kHeightScale, kDeltaTime);
    const std::vector<f32> cpuResult = cpuTerrain->GetHeightData();

    ASSERT_GT(cpuRegion.Width, 0u) << "The CPU reference brush touched nothing — the test inputs are wrong, "
                                      "not the GPU kernel";
    const u32 cpuChanged = CountChanged(original, cpuResult);
    ASSERT_GT(cpuChanged, 100u) << "The CPU reference barely moved the field; parity against it would be "
                                   "vacuous";

    // --- GPU under test ------------------------------------------------------
    Ref<TerrainData> gpuTerrain = MakeTerrain();
    const TerrainBrush::DirtyRegion gpuRegion =
        gpuBrush.ApplySculpt(*gpuTerrain, settings, worldPos, kWorldSizeX, kWorldSizeZ, kHeightScale,
                             kDeltaTime, targetHeight);

    // Same rect: both go through TerrainBrushUtils::ComputeBrushRect, and this is
    // what pins that they really do.
    EXPECT_EQ(gpuRegion.X, cpuRegion.X);
    EXPECT_EQ(gpuRegion.Y, cpuRegion.Y);
    EXPECT_EQ(gpuRegion.Width, cpuRegion.Width);
    EXPECT_EQ(gpuRegion.Height, cpuRegion.Height);

    // The dispatch marks the mirror stale; reading it is what performs the single
    // sync, and asserting the flag first proves the brush did not read back itself.
    EXPECT_TRUE(gpuTerrain->IsCPUMirrorStale())
        << "ApplySculpt left the CPU mirror marked fresh — either it read the heightmap back (the cost "
           "this issue removes) or it forgot to call MarkGPUModified (so every CPU consumer now reads a "
           "pre-stroke surface, silently)";

    const std::vector<f32> gpuResult = gpuTerrain->GetHeightData();
    EXPECT_FALSE(gpuTerrain->IsCPUMirrorStale()) << "Reading the mirror did not clear the stale flag";

    ASSERT_EQ(gpuResult.size(), cpuResult.size());
    EXPECT_GT(CountChanged(original, gpuResult), 100u) << "The GPU kernel dispatched but changed nothing";

    // Tolerance: both sides run the identical float formula, but the GPU evaluates
    // cos() and the divides at its own precision, and the R32F round-trip through
    // the texture costs nothing extra (it is the same 32-bit type). 1e-4 of the
    // normalized [0,1] height range is ~6 mm at this height scale — far below any
    // visible or physical difference, and far below what a space mismatch produces
    // (a wrong-radius brush differs by whole tenths at the rim).
    const FieldDiff diff = CompareFields(cpuResult, gpuResult);
    EXPECT_LT(diff.MaxAbs, 1e-4f)
        << "GPU brush diverged from the CPU brush by " << diff.MaxAbs << " at texel index " << diff.Index
        << " (x=" << (diff.Index % kResolution) << ", z=" << (diff.Index / kResolution) << ")";
}

INSTANTIATE_TEST_SUITE_P(AllTools, TerrainGPUBrushParity,
                         ::testing::Values(TerrainBrushTool::Raise,
                                           TerrainBrushTool::Lower,
                                           TerrainBrushTool::Smooth,
                                           TerrainBrushTool::Flatten,
                                           TerrainBrushTool::Level),
                         [](const ::testing::TestParamInfo<TerrainBrushTool>& info) -> std::string
                         {
                             switch (info.param)
                             {
                                 case TerrainBrushTool::Raise:
                                     return "Raise";
                                 case TerrainBrushTool::Lower:
                                     return "Lower";
                                 case TerrainBrushTool::Smooth:
                                     return "Smooth";
                                 case TerrainBrushTool::Flatten:
                                     return "Flatten";
                                 case TerrainBrushTool::Level:
                                     return "Level";
                             }
                             return "Unknown";
                         });

// The whole point of the move: the brush's CPU-side cost must not grow with the
// radius. This asserts the SHAPE of the cost, not a wall-clock number — a timing
// threshold on a shared dev box is a flake, and the per-texel work is now the
// GPU's, so what is being defended is that no CPU loop over the rect came back.
TEST(TerrainGPUBrush, DispatchCostDoesNotScaleWithBrushArea)
{
    OLO_ENSURE_GPU_OR_SKIP();

    TerrainGPUBrush gpuBrush;
    if (!gpuBrush.IsSculptReady())
    {
        GTEST_SKIP() << "Terrain_SculptBrush.comp did not compile in this environment";
    }

    Ref<TerrainData> terrain = MakeTerrain();
    const glm::vec3 worldPos{ kWorldSizeX * 0.5f, 0.0f, kWorldSizeZ * 0.5f };

    TerrainBrushSettings small;
    small.Tool = TerrainBrushTool::Raise;
    small.Radius = 4.0f;

    TerrainBrushSettings large = small;
    large.Radius = 120.0f;

    const auto smallRegion = gpuBrush.ApplySculpt(*terrain, small, worldPos, kWorldSizeX, kWorldSizeZ,
                                                  kHeightScale, 1.0f / 60.0f, 0.0f);
    const auto largeRegion = gpuBrush.ApplySculpt(*terrain, large, worldPos, kWorldSizeX, kWorldSizeZ,
                                                  kHeightScale, 1.0f / 60.0f, 0.0f);

    // Asserted here, before the timing below: the drained measurement reads the
    // mirror deliberately (that is how it forces the GPU to finish), which clears
    // this flag. Checking it afterwards tests the measurement harness, not the
    // brush — which is exactly what it did on the first run of this file.
    EXPECT_TRUE(terrain->IsCPUMirrorStale())
        << "A brush dispatch synced the CPU mirror — that is a full-map GPU->CPU transfer per stroke frame";

    // The rects differ by a large factor — so if any CPU work were proportional to
    // the rect, these two calls would not be comparable.
    const u64 smallArea = static_cast<u64>(smallRegion.Width) * smallRegion.Height;
    const u64 largeArea = static_cast<u64>(largeRegion.Width) * largeRegion.Height;
    ASSERT_GT(smallArea, 0u);
    ASSERT_GT(largeArea, smallArea * 50) << "The two radii did not produce meaningfully different areas; "
                                            "this test would not discriminate";

    // ---- reported measurement (not asserted) --------------------------------
    // The acceptance criterion is "stroke frame time is flat with respect to
    // brush radius". What a stroke frame actually costs the MAIN THREAD is what
    // is timed here — the GPU does the per-texel work asynchronously, so a
    // GPU-side timer would answer a different question than the one the issue
    // asks. The CPU brush is timed on the same radii as the baseline.
    constexpr u32 kIterations = 200;
    const auto timeCalls = [&](auto&& fn)
    {
        // One untimed call so shader binding / scratch allocation is not charged
        // to the first sample.
        fn();
        const auto start = std::chrono::steady_clock::now();
        for (u32 i = 0; i < kIterations; ++i)
            fn();
        const auto elapsed = std::chrono::steady_clock::now() - start;
        return std::chrono::duration<double, std::micro>(elapsed).count() / kIterations;
    };

    const double gpuSmallUs = timeCalls([&]
                                        { gpuBrush.ApplySculpt(*terrain, small, worldPos, kWorldSizeX, kWorldSizeZ, kHeightScale, 0.0f, 0.0f); });
    const double gpuLargeUs = timeCalls([&]
                                        { gpuBrush.ApplySculpt(*terrain, large, worldPos, kWorldSizeX, kWorldSizeZ, kHeightScale, 0.0f, 0.0f); });

    // A dispatch RETURNS before the GPU has run it, so the figures above are the
    // main-thread cost and nothing else. That is the number the "flat frame time"
    // criterion is about — but on its own it cannot distinguish a cheap dispatch
    // from a cheap dispatch that queues expensive work. So measure again with the
    // pipeline drained: submit the batch, then force completion by reading the
    // heightmap back, and charge the whole thing to the batch.
    const auto timeDrained = [&](const TerrainBrushSettings& settings)
    {
        gpuBrush.ApplySculpt(*terrain, settings, worldPos, kWorldSizeX, kWorldSizeZ, kHeightScale, 0.0f, 0.0f);
        (void)terrain->GetHeightData(); // warm-up drain, untimed
        const auto start = std::chrono::steady_clock::now();
        for (u32 i = 0; i < kIterations; ++i)
            gpuBrush.ApplySculpt(*terrain, settings, worldPos, kWorldSizeX, kWorldSizeZ, kHeightScale, 0.0f,
                                 0.0f);
        (void)terrain->GetHeightData(); // drains every queued dispatch
        const auto elapsed = std::chrono::steady_clock::now() - start;
        return std::chrono::duration<double, std::micro>(elapsed).count() / kIterations;
    };

    const double gpuSmallDrainedUs = timeDrained(small);
    const double gpuLargeDrainedUs = timeDrained(large);

    Ref<TerrainData> cpuTerrain = MakeTerrain();
    const double cpuSmallUs = timeCalls([&]
                                        { TerrainBrush::Apply(*cpuTerrain, small, worldPos, kWorldSizeX, kWorldSizeZ, kHeightScale, 0.0f); });
    const double cpuLargeUs = timeCalls([&]
                                        { TerrainBrush::Apply(*cpuTerrain, large, worldPos, kWorldSizeX, kWorldSizeZ, kHeightScale, 0.0f); });

    std::cout << "\n[ MEASURED ] main-thread cost per stroke frame, " << kIterations
              << " calls averaged, terrain " << kResolution << "^2\n"
              << "             brush rect r=" << small.Radius << ": " << smallRegion.Width << "x"
              << smallRegion.Height << " texels (" << smallArea << ")\n"
              << "             brush rect r=" << large.Radius << ": " << largeRegion.Width << "x"
              << largeRegion.Height << " texels (" << largeArea << ")\n"
              << "             area ratio large/small: "
              << (static_cast<double>(largeArea) / static_cast<double>(smallArea)) << "x\n"
              << "           main-thread only (dispatch returns before the GPU runs it):\n"
              << "             GPU  r=" << small.Radius << ": " << gpuSmallUs << " us\n"
              << "             GPU  r=" << large.Radius << ": " << gpuLargeUs << " us   (ratio "
              << (gpuLargeUs / gpuSmallUs) << "x)\n"
              << "           GPU work included (batch drained by a readback, cost per call):\n"
              << "             GPU  r=" << small.Radius << ": " << gpuSmallDrainedUs << " us\n"
              << "             GPU  r=" << large.Radius << ": " << gpuLargeDrainedUs << " us   (ratio "
              << (gpuLargeDrainedUs / gpuSmallDrainedUs) << "x)\n"
              << "           CPU brush baseline (all work is main-thread):\n"
              << "             CPU  r=" << small.Radius << ": " << cpuSmallUs << " us\n"
              << "             CPU  r=" << large.Radius << ": " << cpuLargeUs << " us   (ratio "
              << (cpuLargeUs / cpuSmallUs) << "x)\n";
}

// What removing the erosion readback actually bought, in microseconds. The old
// TerrainErosion::Apply did a full-heightmap GetData after EVERY iteration; that
// call still exists, once, as TerrainData::SyncFromGPU. Timing the two separately
// says how much of an iteration was dispatch and how much was the stall — which is
// the difference between erosion you can watch converge and erosion you wait for.
//
// Reported, not asserted: see the comment in DispatchCostDoesNotScaleWithBrushArea.
TEST(TerrainErosionInteractivity, ReportsDispatchCostAgainstTheRemovedReadback)
{
    OLO_ENSURE_GPU_OR_SKIP();

    TerrainErosion erosion;
    if (!erosion.IsReady())
    {
        GTEST_SKIP() << "Terrain_Erosion.comp did not compile in this environment";
    }

    Ref<TerrainData> terrain = MakeTerrain();

    ErosionSettings settings;
    settings.DropletCount = 70000; // the panel default

    constexpr u32 kIterations = 20;

    // Both shapes are timed end to end, with the pipeline drained inside the timed
    // region, because a dispatch RETURNS before the GPU has run it. Timing the
    // dispatch alone and calling the difference "the readback" would credit this
    // change with the entire cost of the erosion work itself — the first version of
    // this measurement did exactly that and reported a 573 ms "readback" for a
    // 66 KB texture, which is the GPU draining 20 queued iterations, not a transfer.

    // OLD shape: every iteration read the whole heightmap back before the next one.
    erosion.Apply(*terrain, settings);
    (void)terrain->GetHeightData(); // warm-up, untimed
    const auto oldStart = std::chrono::steady_clock::now();
    for (u32 i = 0; i < kIterations; ++i)
    {
        erosion.Apply(*terrain, settings);
        (void)terrain->GetHeightData(); // what TerrainErosion::Apply used to do inline
    }
    const double oldTotalMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - oldStart).count();

    // NEW shape: N dispatches, then ONE sync when a CPU consumer next asks.
    const auto newStart = std::chrono::steady_clock::now();
    for (u32 i = 0; i < kIterations; ++i)
        erosion.Apply(*terrain, settings);

    // N iterations leave exactly one pending sync, however many ran.
    ASSERT_TRUE(terrain->IsCPUMirrorStale());
    (void)terrain->GetHeightData();
    const double newTotalMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - newStart).count();

    EXPECT_FALSE(terrain->IsCPUMirrorStale());

    // What the editor's continuous-erosion mode actually pays per frame: it
    // dispatches and returns, and the GPU erodes while the frame carries on. This
    // is the figure the "interactive" criterion is about — the drained numbers
    // above are a throughput comparison, and deliberately stall in a way the
    // editor never does.
    erosion.Apply(*terrain, settings); // warm-up, untimed
    const auto mainThreadStart = std::chrono::steady_clock::now();
    for (u32 i = 0; i < kIterations; ++i)
        erosion.Apply(*terrain, settings);
    const double mainThreadUs =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - mainThreadStart).count() /
        kIterations;
    (void)terrain->GetHeightData(); // drain, so the next test does not inherit the queue

    std::cout << "\n[ MEASURED ] erosion, terrain " << kResolution << "^2, "
              << settings.DropletCount << " droplets/iteration, " << kIterations << " iterations\n"
              << "           throughput, GPU drained inside the timed region:\n"
              << "             before (sync every iteration): " << oldTotalMs << " ms total, "
              << (oldTotalMs / kIterations) << " ms/iteration\n"
              << "             after  (one sync at the end):  " << newTotalMs << " ms total, "
              << (newTotalMs / kIterations) << " ms/iteration\n"
              << "             speedup: " << (oldTotalMs / newTotalMs) << "x\n"
              << "           main-thread cost per continuous-erosion frame (what the editor pays):\n"
              << "             " << mainThreadUs << " us/iteration — the GPU work overlaps the frame\n"
              << "           NOTE " << (newTotalMs / kIterations) << " ms of GPU work per iteration at "
              << settings.DropletCount << " droplets is the erosion itself, not the readback;\n"
              << "                the droplet count is what sets how fast it converges per frame.\n";
}

// -----------------------------------------------------------------------------
// Paint parity
// -----------------------------------------------------------------------------

TEST(TerrainGPUPaintBrush, MatchesTheCpuPaintBrushAcrossBothSplatmaps)
{
    OLO_ENSURE_GPU_OR_SKIP();

    TerrainGPUBrush gpuBrush;
    if (!gpuBrush.IsPaintReady())
    {
        GTEST_SKIP() << "Terrain_PaintBrush.comp did not compile in this environment";
    }

    // Six layers, so the second splatmap participates in the re-normalisation —
    // the case where painting one layer must rewrite the OTHER texture too.
    constexpr u32 kLayerCount = 6;
    TerrainPaintSettings settings;
    settings.TargetLayer = 4; // lives in splatmap 1
    settings.Radius = 29.0f;
    settings.Strength = 0.9f;
    settings.Falloff = 0.4f;

    const glm::vec3 worldPos{ 88.5f, 0.0f, 71.25f };
    constexpr f32 kDeltaTime = 1.0f / 60.0f;

    Ref<TerrainMaterial> cpuMaterial = MakeMaterial(kLayerCount);
    const TerrainPaintBrush::DirtyRegion cpuRegion =
        TerrainPaintBrush::Apply(*cpuMaterial, settings, worldPos, kWorldSizeX, kWorldSizeZ, kDeltaTime);
    ASSERT_GT(cpuRegion.Width, 0u);
    const std::vector<u8> cpuSplat0 = cpuMaterial->GetSplatmapData(0);
    const std::vector<u8> cpuSplat1 = cpuMaterial->GetSplatmapData(1);

    Ref<TerrainMaterial> gpuMaterial = MakeMaterial(kLayerCount);
    const TerrainPaintBrush::DirtyRegion gpuRegion =
        gpuBrush.ApplyPaint(*gpuMaterial, settings, worldPos, kWorldSizeX, kWorldSizeZ, kDeltaTime);

    EXPECT_EQ(gpuRegion.X, cpuRegion.X);
    EXPECT_EQ(gpuRegion.Y, cpuRegion.Y);
    EXPECT_EQ(gpuRegion.Width, cpuRegion.Width);
    EXPECT_EQ(gpuRegion.Height, cpuRegion.Height);

    EXPECT_TRUE(gpuMaterial->AreCPUSplatmapsStale())
        << "ApplyPaint did not mark the splatmap mirror stale — every CPU consumer (foliage density masks, "
           "save) would keep reading the pre-stroke splatmap";

    const std::vector<u8> gpuSplat0 = gpuMaterial->GetSplatmapData(0);
    const std::vector<u8> gpuSplat1 = gpuMaterial->GetSplatmapData(1);
    ASSERT_EQ(gpuSplat0.size(), cpuSplat0.size());
    ASSERT_EQ(gpuSplat1.size(), cpuSplat1.size());

    // Tolerance of one level, not zero: both sides quantise to u8 at every
    // assignment, and the GPU reaches those bytes through an RGBA8 unorm image
    // whose float round-trip can land either side of a .5 boundary the CPU's
    // truncation resolves the other way. A real divergence — a wrong channel, a
    // missing normalisation, a mis-scaled radius — moves whole tens of levels.
    i32 worst0 = 0;
    i32 worst1 = 0;
    sizet worstIndex = 0;
    for (sizet i = 0; i < cpuSplat0.size(); ++i)
    {
        const i32 d0 = std::abs(static_cast<i32>(cpuSplat0[i]) - static_cast<i32>(gpuSplat0[i]));
        const i32 d1 = std::abs(static_cast<i32>(cpuSplat1[i]) - static_cast<i32>(gpuSplat1[i]));
        if (d0 > worst0)
        {
            worst0 = d0;
            worstIndex = i;
        }
        worst1 = std::max(worst1, d1);
    }

    EXPECT_LE(worst0, 1) << "Splatmap 0 diverged by " << worst0 << " levels at byte " << worstIndex
                         << " — splatmap 0 is not the painted one, so this is the re-normalisation "
                            "disagreeing, not the deposit";
    EXPECT_LE(worst1, 1) << "Splatmap 1 (the painted one) diverged by " << worst1 << " levels";

    // Guard against a vacuous pass: the stroke must actually have painted.
    const u32 layerChannel = settings.TargetLayer % 4;
    u32 painted = 0;
    for (sizet px = layerChannel; px < gpuSplat1.size(); px += 4)
    {
        if (gpuSplat1[px] > 0)
            ++painted;
    }
    EXPECT_GT(painted, 100u) << "The GPU paint kernel dispatched but deposited nothing";
}

// -----------------------------------------------------------------------------
// The sync point
// -----------------------------------------------------------------------------

TEST(TerrainDataSyncPoint, GpuEditsReachEveryCpuConsumerThroughOneSync)
{
    OLO_ENSURE_GPU_OR_SKIP();

    TerrainGPUBrush gpuBrush;
    if (!gpuBrush.IsSculptReady())
    {
        GTEST_SKIP() << "Terrain_SculptBrush.comp did not compile in this environment";
    }

    Ref<TerrainData> terrain = MakeTerrain();
    const std::vector<f32> before = terrain->GetHeightData();
    EXPECT_FALSE(terrain->IsCPUMirrorStale());

    TerrainBrushSettings settings;
    settings.Tool = TerrainBrushTool::Raise;
    settings.Radius = 40.0f;
    settings.Strength = 1.0f;

    const glm::vec3 worldPos{ kWorldSizeX * 0.5f, 0.0f, kWorldSizeZ * 0.5f };
    const auto region = gpuBrush.ApplySculpt(*terrain, settings, worldPos, kWorldSizeX, kWorldSizeZ,
                                             kHeightScale, 1.0f / 30.0f, 0.0f);
    ASSERT_GT(region.Width, 0u);

    // GetHeightAt is the query the physics height field and gameplay both reach
    // through. It must see the GPU edit — this is the assertion that separates
    // "the terrain looks right" from "gameplay agrees with it".
    const f32 sampled = terrain->GetHeightAt(0.5f, 0.5f);
    const sizet centreIndex = static_cast<sizet>(kResolution / 2) * kResolution + (kResolution / 2);
    EXPECT_GT(sampled, before[centreIndex])
        << "GetHeightAt returned the PRE-stroke height after a GPU sculpt. The CPU mirror is stale and "
           "nothing synced it — the physics height field, height queries and save would all silently "
           "disagree with what is on screen.";

    // And the raw mirror agrees with the query.
    const std::vector<f32> after = terrain->GetHeightData();
    EXPECT_GT(after[centreIndex], before[centreIndex]);
    EXPECT_GT(CountChanged(before, after), 100u);
}

TEST(TerrainDataSyncPoint, UploadingTheMirrorDischargesThePendingSync)
{
    OLO_ENSURE_GPU_OR_SKIP();

    Ref<TerrainData> terrain = MakeTerrain();
    terrain->MarkGPUModified();
    ASSERT_TRUE(terrain->IsCPUMirrorStale());

    // A CPU-side write pushed to the GPU makes the two agree again. If the flag
    // survived, the very next read would pull the freshly uploaded data back —
    // harmless but pointless — and if the flag were checked BEFORE the upload the
    // read would clobber the write. Both orderings look fine in a screenshot.
    terrain->UploadToGPU();
    EXPECT_FALSE(terrain->IsCPUMirrorStale());
}

// -----------------------------------------------------------------------------
// The undo ring
// -----------------------------------------------------------------------------

TEST(TerrainTextureUndoStack, SnapshotAndRestoreRoundTripARegion)
{
    OLO_ENSURE_GPU_OR_SKIP();

    TerrainGPUBrush gpuBrush;
    if (!gpuBrush.IsSculptReady())
    {
        GTEST_SKIP() << "Terrain_SculptBrush.comp did not compile in this environment";
    }

    Ref<TerrainData> terrain = MakeTerrain();
    const std::vector<f32> original = terrain->GetHeightData();

    TerrainTextureUndoStack stack;
    const auto snapshot = stack.Capture(terrain->GetGPUHeightmap(), 0, 0, kResolution, kResolution);
    ASSERT_NE(snapshot, TerrainTextureUndoStack::kInvalidSnapshot);

    TerrainBrushSettings settings;
    settings.Tool = TerrainBrushTool::Raise;
    settings.Radius = 45.0f;
    settings.Strength = 1.0f;
    const glm::vec3 worldPos{ kWorldSizeX * 0.5f, 0.0f, kWorldSizeZ * 0.5f };
    ASSERT_GT(gpuBrush.ApplySculpt(*terrain, settings, worldPos, kWorldSizeX, kWorldSizeZ, kHeightScale,
                                   1.0f / 30.0f, 0.0f)
                  .Width,
              0u);

    const std::vector<f32> edited = terrain->GetHeightData();
    ASSERT_GT(CountChanged(original, edited), 100u) << "The stroke changed nothing; the restore below "
                                                       "would pass trivially";

    ASSERT_TRUE(stack.Restore(snapshot, terrain->GetGPUHeightmap()));
    terrain->MarkGPUModified();

    const std::vector<f32> restored = terrain->GetHeightData();
    const FieldDiff diff = CompareFields(original, restored);
    EXPECT_LE(diff.MaxAbs, 0.0f) << "Undo did not restore the pre-stroke field exactly (worst diff "
                                 << diff.MaxAbs << " at index " << diff.Index
                                 << "). A blit-based undo is a byte copy — any difference at all means the "
                                    "rect or the offsets are wrong, not that precision was lost.";
}

TEST(TerrainTextureUndoStack, EvictsOldestOnceOverBudgetAndRefusesTheEvictedStep)
{
    OLO_ENSURE_GPU_OR_SKIP();

    Ref<TerrainData> terrain = MakeTerrain();
    Ref<Texture2D> heightmap = terrain->GetGPUHeightmap();
    ASSERT_TRUE(heightmap);

    // Room for three entries and no more. The entry cap and the byte cap are
    // separate bounds; this exercises the entry one, which is the bound a session
    // of many small dabs hits first.
    TerrainTextureUndoStack stack(3, 64ull * 1024 * 1024);

    std::vector<TerrainTextureUndoStack::SnapshotId> ids;
    for (u32 i = 0; i < 5; ++i)
    {
        const auto id = stack.Capture(heightmap, 0, 0, 16, 16);
        ASSERT_NE(id, TerrainTextureUndoStack::kInvalidSnapshot);
        ids.push_back(id);
    }

    EXPECT_EQ(stack.GetEntryCount(), 3u);
    EXPECT_EQ(stack.GetEvictionCount(), 2u)
        << "Entries were dropped without being counted, or were not dropped at all — a ring that never "
           "evicts is a VRAM leak that only shows up after a long editing session";

    // The two oldest are gone and must REPORT that, not silently restore something.
    EXPECT_FALSE(stack.Contains(ids[0]));
    EXPECT_FALSE(stack.Contains(ids[1]));
    EXPECT_FALSE(stack.Restore(ids[0], heightmap));
    EXPECT_TRUE(stack.Contains(ids[4]));
    EXPECT_TRUE(stack.Restore(ids[4], heightmap));

    // Releasing gives the budget back, so a discarded redo branch does not
    // permanently shrink the usable history.
    const sizet bytesBefore = stack.GetBytesUsed();
    ASSERT_GT(bytesBefore, 0u);
    stack.Release(ids[4]);
    EXPECT_LT(stack.GetBytesUsed(), bytesBefore);
    EXPECT_EQ(stack.GetEntryCount(), 2u);
}

TEST(TerrainTextureUndoStack, ByteBudgetBoundsASingleOversizedCapture)
{
    OLO_ENSURE_GPU_OR_SKIP();

    Ref<TerrainData> terrain = MakeTerrain();
    Ref<Texture2D> heightmap = terrain->GetGPUHeightmap();
    ASSERT_TRUE(heightmap);

    // A budget smaller than one full-map snapshot. The newest entry is evicted
    // along with everything else rather than being allowed to sit outside the
    // budget — a "keep at least the latest" exemption is how a bounded cache
    // quietly becomes unbounded when every entry is oversized.
    const sizet oneSnapshot = static_cast<sizet>(kResolution) * kResolution * sizeof(f32);
    TerrainTextureUndoStack stack(64, oneSnapshot / 2);

    // Capture must REFUSE, not hand back an id it has already dropped: a caller
    // that got a plausible id would build an undo command whose first press
    // silently does nothing.
    const auto id = stack.Capture(heightmap, 0, 0, kResolution, kResolution);
    EXPECT_EQ(id, TerrainTextureUndoStack::kInvalidSnapshot)
        << "Capture returned an id for a snapshot that its own budget check evicted";

    EXPECT_LE(stack.GetBytesUsed(), stack.GetMaxBytes());
    EXPECT_FALSE(stack.Contains(id));
    EXPECT_FALSE(stack.Restore(id, heightmap))
        << "An over-budget snapshot stayed usable — the byte bound is not actually bounding anything";
}

TEST(TerrainTextureUndoStack, FullCopyReusesItsSlotAcrossStrokes)
{
    OLO_ENSURE_GPU_OR_SKIP();

    Ref<TerrainData> terrain = MakeTerrain();
    Ref<Texture2D> heightmap = terrain->GetGPUHeightmap();
    ASSERT_TRUE(heightmap);

    Ref<Texture2D> slot;
    ASSERT_TRUE(TerrainTextureUndoStack::EnsureFullCopy(slot, heightmap));
    ASSERT_TRUE(slot);
    EXPECT_EQ(slot->GetWidth(), kResolution);
    EXPECT_EQ(slot->GetHeight(), kResolution);

    // A second stroke must not allocate again — this copy happens on every mouse
    // press, so reallocating each time would be a full-map allocation per stroke.
    Texture2D* firstAddress = slot.Raw();
    ASSERT_TRUE(TerrainTextureUndoStack::EnsureFullCopy(slot, heightmap));
    EXPECT_EQ(slot.Raw(), firstAddress);

    // A terrain regenerated at a different resolution must force a new slot rather
    // than reusing one the copy cannot legally target.
    Ref<TerrainData> smaller = Ref<TerrainData>::Create();
    smaller->CreateFlat(64, 0.25f);
    ASSERT_TRUE(TerrainTextureUndoStack::EnsureFullCopy(slot, smaller->GetGPUHeightmap()));
    EXPECT_EQ(slot->GetWidth(), 64u);
}
