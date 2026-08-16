// OLO_TEST_LAYER: L1
// =============================================================================
// TerrainGPUQuadtreeTest.cpp
//
// Pins the GPU terrain LOD quadtree (issue #714) against the CPU quadtree it
// replaces, and against the property the whole feature exists to preserve:
// **adjacent patches must not crack**.
//
// Three things are transcribed from GLSL into C++ here — the level descent
// (`TerrainNodeSelect.comp`), the split-map walk (`TerrainLODMap.comp`) and the
// seam packing (`TerrainSeamMap.comp`) — plus the vertex stage's edge snapping
// from `include/TerrainQuadtreeCommon.glsl`. Each transcription is line-for-line
// with its shader; a divergence on either side is the bug this file catches.
//
// Why a transcription rather than a GPU test: the failure mode is a *silent
// visual* one. A seam that only breaks at a specific neighbour-level delta will
// not show up in an average-pixel comparison, and would need a camera parked at
// exactly the wrong boundary to appear in a screenshot. Enumerating every
// boundary in a selected set and checking the two edges' vertices coincide
// exactly is the only form of this check that is complete. The screenshots in
// `TerrainGPUQuadtreeVisualEvidenceTest` cover the other half — that what the
// math proves is actually what reaches the screen.
//
// Classification: L1 (pure CPU, no GL context).
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Terrain/TerrainGPUQuadtree.h"
#include "OloEngine/Terrain/TerrainQuadtree.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kK = TerrainGPUQuadtree::kPatchGridResolution;
        constexpr u32 kMaxSeamDelta = TerrainGPUQuadtree::kMaxSeamDelta;

        struct GpuNodeCoord
        {
            u32 Level = 0;
            u32 X = 0;
            u32 Y = 0;

            auto operator<=>(const GpuNodeCoord&) const = default;
        };

        // --------------------------------------------------------------------
        // Transcription of include/TerrainQuadtreeCommon.glsl
        // --------------------------------------------------------------------
        u32 NodeIndex_GLSLEquivalent(u32 level, u32 nx, u32 ny)
        {
            const u32 levelOffset = ((1u << (2u * level)) - 1u) / 3u;
            return levelOffset + (ny << level) + nx;
        }

        i32 SnapEdgeIndex_GLSLEquivalent(i32 index, u32 delta)
        {
            return (index >> static_cast<i32>(delta)) << static_cast<i32>(delta);
        }

        // --------------------------------------------------------------------
        // Transcription of TerrainNodeSelect.comp, run level by level exactly as
        // the CPU dispatch loop drives it.
        // --------------------------------------------------------------------
        struct DescentResult
        {
            std::vector<GpuNodeCoord> Visible;
            std::vector<u8> SplitMap; // one entry per node, level-major
        };

        DescentResult RunDescent_GLSLEquivalent(const std::vector<glm::vec2>& nodeMinMaxY,
                                                u32 maxDepth,
                                                f32 worldSizeX, f32 worldSizeZ,
                                                const std::array<glm::vec4, 6>& planes,
                                                const glm::vec3& cameraPos,
                                                f32 projScale,
                                                f32 targetTriangleSize)
        {
            DescentResult result;
            result.SplitMap.assign(TerrainGPUQuadtree::TotalNodeCount(maxDepth), 0);

            std::vector<GpuNodeCoord> pending{ GpuNodeCoord{ 0, 0, 0 } };
            for (u32 level = 0; level <= maxDepth && !pending.empty(); ++level)
            {
                std::vector<GpuNodeCoord> next;
                for (const GpuNodeCoord& node : pending)
                {
                    const f32 span = 1.0f / static_cast<f32>(1u << node.Level);
                    const f32 minX = static_cast<f32>(node.X) * span * worldSizeX;
                    const f32 maxX = static_cast<f32>(node.X + 1u) * span * worldSizeX;
                    const f32 minZ = static_cast<f32>(node.Y) * span * worldSizeZ;
                    const f32 maxZ = static_cast<f32>(node.Y + 1u) * span * worldSizeZ;

                    const glm::vec2 heightRange = nodeMinMaxY[NodeIndex_GLSLEquivalent(node.Level, node.X, node.Y)];
                    const glm::vec3 boundsMin{ minX, heightRange.x, minZ };
                    const glm::vec3 boundsMax{ maxX, heightRange.y, maxZ };

                    bool culled = false;
                    for (const glm::vec4& plane : planes)
                    {
                        glm::vec3 p = boundsMin;
                        if (plane.x >= 0.0f)
                            p.x = boundsMax.x;
                        if (plane.y >= 0.0f)
                            p.y = boundsMax.y;
                        if (plane.z >= 0.0f)
                            p.z = boundsMax.z;
                        if (glm::dot(glm::vec3(plane), p) + plane.w < 0.0f)
                        {
                            culled = true;
                            break;
                        }
                    }
                    if (culled)
                        continue;

                    if (node.Level < maxDepth)
                    {
                        const f32 geometricError = std::max(maxX - minX, maxZ - minZ);
                        const glm::vec3 nodeCenter = (boundsMin + boundsMax) * 0.5f;
                        const f32 distance = std::max(glm::length(cameraPos - nodeCenter), 0.001f);
                        const f32 screenError = (geometricError * projScale) / distance;
                        if (screenError >= targetTriangleSize)
                        {
                            result.SplitMap[NodeIndex_GLSLEquivalent(node.Level, node.X, node.Y)] = 1;
                            const u32 childLevel = node.Level + 1u;
                            next.push_back({ childLevel, node.X * 2u, node.Y * 2u });
                            next.push_back({ childLevel, node.X * 2u + 1u, node.Y * 2u });
                            next.push_back({ childLevel, node.X * 2u, node.Y * 2u + 1u });
                            next.push_back({ childLevel, node.X * 2u + 1u, node.Y * 2u + 1u });
                            continue;
                        }
                    }
                    result.Visible.push_back(node);
                }
                pending = std::move(next);
            }
            return result;
        }

        // --------------------------------------------------------------------
        // Transcription of TerrainLODMap.comp
        // --------------------------------------------------------------------
        std::vector<u32> BuildLODMap_GLSLEquivalent(const std::vector<u8>& splitMap, u32 maxDepth)
        {
            const u32 resolution = 1u << maxDepth;
            std::vector<u32> lodMap(static_cast<sizet>(resolution) * resolution, maxDepth);
            for (u32 ty = 0; ty < resolution; ++ty)
            {
                for (u32 tx = 0; tx < resolution; ++tx)
                {
                    u32 selected = maxDepth;
                    for (u32 level = 0; level < maxDepth; ++level)
                    {
                        const u32 shift = maxDepth - level;
                        if (splitMap[NodeIndex_GLSLEquivalent(level, tx >> shift, ty >> shift)] == 0)
                        {
                            selected = level;
                            break;
                        }
                    }
                    lodMap[static_cast<sizet>(ty) * resolution + tx] = selected;
                }
            }
            return lodMap;
        }

        // --------------------------------------------------------------------
        // Transcription of TerrainSeamMap.comp
        // --------------------------------------------------------------------
        struct SeamDeltas
        {
            u32 PlusX = 0;
            u32 MinusX = 0;
            u32 PlusZ = 0;
            u32 MinusZ = 0;
        };

        u32 SeamDeltaAt_GLSLEquivalent(u32 myLevel, glm::ivec2 texel, u32 resolution,
                                       const std::vector<u32>& lodMap)
        {
            if (texel.x < 0 || texel.y < 0 || texel.x >= static_cast<i32>(resolution) ||
                texel.y >= static_cast<i32>(resolution))
            {
                return 0;
            }
            const u32 neighbourLevel = lodMap[static_cast<sizet>(texel.y) * resolution + static_cast<sizet>(texel.x)];
            const u32 delta = (myLevel > neighbourLevel) ? (myLevel - neighbourLevel) : 0u;
            return std::min(delta, kMaxSeamDelta);
        }

        SeamDeltas ComputeSeams_GLSLEquivalent(const GpuNodeCoord& node, u32 maxDepth,
                                               const std::vector<u32>& lodMap)
        {
            const u32 resolution = 1u << maxDepth;
            const u32 step = 1u << (maxDepth - node.Level);
            const glm::ivec2 origin{ static_cast<i32>(node.X * step), static_cast<i32>(node.Y * step) };
            const i32 mid = static_cast<i32>(step >> 1u);

            SeamDeltas seams;
            seams.PlusX = SeamDeltaAt_GLSLEquivalent(node.Level, origin + glm::ivec2(static_cast<i32>(step), mid), resolution, lodMap);
            seams.MinusX = SeamDeltaAt_GLSLEquivalent(node.Level, origin + glm::ivec2(-1, mid), resolution, lodMap);
            seams.PlusZ = SeamDeltaAt_GLSLEquivalent(node.Level, origin + glm::ivec2(mid, static_cast<i32>(step)), resolution, lodMap);
            seams.MinusZ = SeamDeltaAt_GLSLEquivalent(node.Level, origin + glm::ivec2(mid, -1), resolution, lodMap);
            return seams;
        }

        // --------------------------------------------------------------------
        // Transcription of the GPU-driven branch of the terrain vertex stages.
        // Returns the normalized terrain-space UV a patch vertex lands on.
        // --------------------------------------------------------------------
        glm::dvec2 PatchVertexUV_GLSLEquivalent(const GpuNodeCoord& node, const SeamDeltas& seams,
                                                i32 gx, i32 gz)
        {
            glm::ivec2 gi{ gx, gz };
            if (gi.x == 0)
                gi.y = SnapEdgeIndex_GLSLEquivalent(gi.y, seams.MinusX);
            else if (gi.x == static_cast<i32>(kK))
                gi.y = SnapEdgeIndex_GLSLEquivalent(gi.y, seams.PlusX);
            if (gi.y == 0)
                gi.x = SnapEdgeIndex_GLSLEquivalent(gi.x, seams.MinusZ);
            else if (gi.y == static_cast<i32>(kK))
                gi.x = SnapEdgeIndex_GLSLEquivalent(gi.x, seams.PlusZ);

            // Done in double here purely so the SET COMPARISON below is exact.
            // The shader works in float; the positions it produces are dyadic
            // rationals (integer / 2^n), which float represents without error at
            // these magnitudes, so the two agree bit for bit.
            const f64 span = 1.0 / static_cast<f64>(1u << node.Level);
            const f64 u = (static_cast<f64>(node.X) + static_cast<f64>(gi.x) / static_cast<f64>(kK)) * span;
            const f64 v = (static_cast<f64>(node.Y) + static_cast<f64>(gi.y) / static_cast<f64>(kK)) * span;
            return { u, v };
        }

        std::array<glm::vec4, 6> ExtractPlanes(const Frustum& frustum)
        {
            using enum Frustum::Planes;
            constexpr std::array<Frustum::Planes, 6> kOrder{ Near, Far, Left, Right, Top, Bottom };
            std::array<glm::vec4, 6> planes{};
            for (sizet i = 0; i < kOrder.size(); ++i)
            {
                const Plane& p = frustum.GetPlane(kOrder[i]);
                planes[i] = glm::vec4(p.Normal, p.Distance);
            }
            return planes;
        }

        // Deterministic non-flat heightfield, built as a plain array. Nothing
        // here touches TerrainData, and that is the point: populating a
        // TerrainData uploads a GPU heightmap texture, so an L1 test that used
        // one would fault on a null GL entry point whenever it happened to run
        // without a suite that brings a context up first. The crack, seam and
        // LOD-map proofs below are pure arithmetic and must not depend on that.
        std::vector<f32> MakeHeightField(u32 resolution)
        {
            std::vector<f32> heights(static_cast<sizet>(resolution) * resolution);
            for (u32 z = 0; z < resolution; ++z)
            {
                for (u32 x = 0; x < resolution; ++x)
                {
                    const f32 u = static_cast<f32>(x) / static_cast<f32>(resolution);
                    const f32 v = static_cast<f32>(z) / static_cast<f32>(resolution);
                    // Ridged sum of a few octaves — non-flat in both axes, so a
                    // node's AABB genuinely differs from its neighbours' and a
                    // pyramid mistake cannot hide behind uniform heights.
                    const f32 h = 0.5f + 0.30f * std::sin(u * 11.0f) * std::cos(v * 7.0f) +
                                  0.12f * std::sin(u * 29.0f + v * 17.0f) +
                                  0.05f * std::cos(u * 53.0f - v * 41.0f);
                    heights[static_cast<sizet>(z) * resolution + x] = std::clamp(h, 0.0f, 1.0f);
                }
            }
            return heights;
        }

        constexpr u32 kTestResolution = 256;
        constexpr f32 kWorldSizeX = 512.0f;
        constexpr f32 kWorldSizeZ = 512.0f;
        constexpr f32 kHeightScale = 60.0f;
        constexpr u32 kTestDepth = 5;
        constexpr f32 kViewportHeight = 1080.0f;

        std::vector<glm::vec2> MakeTestPyramid(u32 maxDepth = kTestDepth)
        {
            const std::vector<f32> heights = MakeHeightField(kTestResolution);
            return TerrainQuadtree::BuildHeightPyramid(heights, kTestResolution, kHeightScale, maxDepth);
        }

        // Split thresholds swept alongside the camera poses. The shipped default
        // of 8 px splits everything to leaves at these distances, so a sweep that
        // only used it would never produce two ADJACENT VISIBLE nodes at
        // different levels — and the crack proof would pass vacuously. The looser
        // values are what actually exercise the seam snapping.
        constexpr std::array<f32, 4> kTargetTriangleSizes{ 8.0f, 120.0f, 600.0f, 2400.0f };

        // The camera poses the parity / crack sweeps run over. Deliberately
        // includes a ground-level pose looking across the terrain (the case that
        // produces the widest spread of LOD levels in one frame, and therefore
        // the most boundaries) and a top-down one (the case the issue's
        // acceptance criteria name).
        std::vector<std::pair<glm::vec3, glm::mat4>> MakeCameraPoses(f32 worldSizeX, f32 worldSizeZ)
        {
            std::vector<std::pair<glm::vec3, glm::mat4>> poses;
            const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 5000.0f);
            const glm::vec3 center{ worldSizeX * 0.5f, 0.0f, worldSizeZ * 0.5f };

            // Ground level, sweeping the yaw — a grazing view across the whole
            // terrain, which is where LOD levels fan out the most.
            for (i32 step = 0; step < 8; ++step)
            {
                const f32 yaw = glm::radians(static_cast<f32>(step) * 45.0f);
                const glm::vec3 eye{ worldSizeX * 0.5f + std::cos(yaw) * 20.0f, 30.0f,
                                     worldSizeZ * 0.5f + std::sin(yaw) * 20.0f };
                const glm::vec3 target = eye + glm::vec3(std::cos(yaw + 1.0f), -0.1f, std::sin(yaw + 1.0f)) * 100.0f;
                poses.emplace_back(eye, proj * glm::lookAt(eye, target, glm::vec3(0, 1, 0)));
            }

            // Directly above, at three heights.
            for (f32 height : { 150.0f, 400.0f, 1200.0f })
            {
                const glm::vec3 eye{ center.x, height, center.z };
                poses.emplace_back(eye, proj * glm::lookAt(eye, center, glm::vec3(0, 0, 1)));
            }

            // Oblique, from outside the terrain looking in.
            for (i32 step = 0; step < 4; ++step)
            {
                const f32 yaw = glm::radians(static_cast<f32>(step) * 90.0f + 30.0f);
                const glm::vec3 eye{ center.x + std::cos(yaw) * worldSizeX, 250.0f,
                                     center.z + std::sin(yaw) * worldSizeZ };
                poses.emplace_back(eye, proj * glm::lookAt(eye, center, glm::vec3(0, 1, 0)));
            }
            return poses;
        }
    } // namespace

    // =========================================================================
    // Node addressing
    // =========================================================================

    TEST(TerrainGPUQuadtree, LevelMajorNodeIndexingIsDenseAndCollisionFree)
    {
        constexpr u32 kDepth = 6;
        const u32 total = TerrainGPUQuadtree::TotalNodeCount(kDepth);
        EXPECT_EQ(total, (((1u << (2u * (kDepth + 1u))) - 1u) / 3u));

        std::vector<u32> hits(total, 0);
        for (u32 level = 0; level <= kDepth; ++level)
        {
            const u32 span = 1u << level;
            EXPECT_EQ(TerrainGPUQuadtree::LevelOffset(level), NodeIndex_GLSLEquivalent(level, 0, 0))
                << "C++ LevelOffset and the GLSL level offset disagree at level " << level;
            for (u32 y = 0; y < span; ++y)
            {
                for (u32 x = 0; x < span; ++x)
                {
                    const u32 index = NodeIndex_GLSLEquivalent(level, x, y);
                    ASSERT_LT(index, total) << "node (" << level << "," << x << "," << y << ") indexes past the pyramid";
                    ++hits[index];
                }
            }
        }
        EXPECT_EQ(std::ranges::count(hits, 1u), static_cast<std::ptrdiff_t>(total))
            << "every pyramid slot must be claimed by exactly one node";
    }

    TEST(TerrainGPUQuadtree, PackedNodeCoordRoundTrips)
    {
        for (u32 level = 0; level <= TerrainGPUQuadtree::kMaxDepth; ++level)
        {
            const u32 span = 1u << level;
            for (u32 y : { 0u, span / 2u, span - 1u })
            {
                for (u32 x : { 0u, span / 2u, span - 1u })
                {
                    const u32 packed = TerrainGPUQuadtree::PackNode(level, x, y);
                    EXPECT_EQ(packed >> 28u, level);
                    EXPECT_EQ(packed & 0x3FFFu, x);
                    EXPECT_EQ((packed >> 14u) & 0x3FFFu, y);
                }
            }
        }
    }

    // =========================================================================
    // The height pyramid both paths read
    // =========================================================================

    TEST(TerrainGPUQuadtree, HeightPyramidBoundsEveryChildsRange)
    {
        const std::vector<glm::vec2> pyramid = MakeTestPyramid();
        ASSERT_EQ(pyramid.size(), TerrainGPUQuadtree::TotalNodeCount(kTestDepth));

        f32 widestRange = 0.0f;
        f32 narrowestRange = std::numeric_limits<f32>::max();
        for (u32 level = 0; level < kTestDepth; ++level)
        {
            const u32 span = 1u << level;
            for (u32 y = 0; y < span; ++y)
            {
                for (u32 x = 0; x < span; ++x)
                {
                    const glm::vec2 parent = pyramid[NodeIndex_GLSLEquivalent(level, x, y)];
                    widestRange = std::max(widestRange, parent.y - parent.x);
                    narrowestRange = std::min(narrowestRange, parent.y - parent.x);
                    for (u32 cy = 0; cy < 2; ++cy)
                    {
                        for (u32 cx = 0; cx < 2; ++cx)
                        {
                            const glm::vec2 child = pyramid[NodeIndex_GLSLEquivalent(level + 1, x * 2 + cx, y * 2 + cy)];
                            EXPECT_LE(parent.x, child.x) << "parent min must not exceed a child's min";
                            EXPECT_GE(parent.y, child.y) << "parent max must not fall below a child's max";
                        }
                    }
                }
            }
        }

        // A flat field would satisfy the containment above trivially.
        EXPECT_GT(widestRange, narrowestRange)
            << "the test heightfield is flat — the containment check would hold vacuously";
    }

    // =========================================================================
    // Parity with the CPU descent this replaces
    // =========================================================================

    TEST(TerrainGPUQuadtree, GpuDescentSelectsTheSameNodesAsTheCpuQuadtree)
    {
        const std::vector<glm::vec2> pyramid = MakeTestPyramid();

        u32 posesWithSplit = 0;
        u32 comparisons = 0;
        for (const auto& [eye, viewProjection] : MakeCameraPoses(kWorldSizeX, kWorldSizeZ))
        {
            for (const f32 target : kTargetTriangleSizes)
            {
                TerrainQuadtree cpuTree;
                cpuTree.BuildFromPyramid(pyramid, kWorldSizeX, kWorldSizeZ, kTestDepth);
                cpuTree.GetConfig().TargetTriangleSize = target;
                ASSERT_EQ(cpuTree.GetMaxDepth(), kTestDepth);

                const Frustum frustum(viewProjection);
                cpuTree.SelectLOD(frustum, eye, viewProjection, kViewportHeight);

                std::set<GpuNodeCoord> cpuSelected;
                for (const TerrainQuadNode* node : cpuTree.GetSelectedNodes())
                {
                    const u32 span = 1u << node->Depth;
                    cpuSelected.insert({ node->Depth,
                                         static_cast<u32>(std::lround(node->MinX * static_cast<f32>(span))),
                                         static_cast<u32>(std::lround(node->MinZ * static_cast<f32>(span))) });
                }

                const f32 projScale = viewProjection[1][1] * kViewportHeight * 0.5f;
                const DescentResult gpu = RunDescent_GLSLEquivalent(
                    pyramid, kTestDepth, kWorldSizeX, kWorldSizeZ,
                    ExtractPlanes(frustum), eye, projScale, target);

                const std::set<GpuNodeCoord> gpuSelected(gpu.Visible.begin(), gpu.Visible.end());
                ASSERT_EQ(gpuSelected, cpuSelected)
                    << "GPU descent and TerrainQuadtree::SelectNode disagree at target=" << target
                    << " camera " << eye.x << "," << eye.y << "," << eye.z;
                ++comparisons;

                if (std::ranges::any_of(gpu.Visible, [](const GpuNodeCoord& n)
                                        { return n.Level != kTestDepth; }))
                    ++posesWithSplit;
            }
        }

        EXPECT_GT(comparisons, 0u);
        // Guard against a vacuous pass: if every case selected only leaves, the
        // comparison would be satisfied by any descent that always recurses.
        EXPECT_GT(posesWithSplit, 0u) << "no case produced a partially-split tree — the sweep proves nothing";
    }

    // =========================================================================
    // Seam resolution
    // =========================================================================

    TEST(TerrainGPUQuadtree, LodMapReportsTheLevelOfTheFirstUnsplitAncestor)
    {
        const std::vector<glm::vec2> pyramid = MakeTestPyramid();
        const auto poses = MakeCameraPoses(kWorldSizeX, kWorldSizeZ);
        const auto& [eye, viewProjection] = poses.front();
        const Frustum frustum(viewProjection);

        const DescentResult gpu = RunDescent_GLSLEquivalent(
            pyramid, kTestDepth, kWorldSizeX, kWorldSizeZ, ExtractPlanes(frustum), eye,
            viewProjection[1][1] * kViewportHeight * 0.5f, 120.0f);
        const std::vector<u32> lodMap = BuildLODMap_GLSLEquivalent(gpu.SplitMap, kTestDepth);

        // Every VISIBLE node's own footprint must report that node's level — the
        // property the seam kernel relies on when it samples just past an edge.
        const u32 resolution = 1u << kTestDepth;
        for (const GpuNodeCoord& node : gpu.Visible)
        {
            const u32 step = 1u << (kTestDepth - node.Level);
            for (u32 dy = 0; dy < step; ++dy)
            {
                for (u32 dx = 0; dx < step; ++dx)
                {
                    const u32 tx = node.X * step + dx;
                    const u32 ty = node.Y * step + dy;
                    ASSERT_EQ(lodMap[static_cast<sizet>(ty) * resolution + tx], node.Level)
                        << "LOD map texel (" << tx << "," << ty << ") should report level " << node.Level;
                }
            }
        }
    }

    TEST(TerrainGPUQuadtree, SeamDeltasStayWithinWhatAPatchEdgeCanExpress)
    {
        const std::vector<glm::vec2> pyramid = MakeTestPyramid();

        u32 nonZeroDeltas = 0;
        for (const auto& [eye, viewProjection] : MakeCameraPoses(kWorldSizeX, kWorldSizeZ))
        {
            for (const f32 target : kTargetTriangleSizes)
            {
                const Frustum frustum(viewProjection);
                const DescentResult gpu = RunDescent_GLSLEquivalent(
                    pyramid, kTestDepth, kWorldSizeX, kWorldSizeZ, ExtractPlanes(frustum), eye,
                    viewProjection[1][1] * kViewportHeight * 0.5f, target);
                const std::vector<u32> lodMap = BuildLODMap_GLSLEquivalent(gpu.SplitMap, kTestDepth);

                for (const GpuNodeCoord& node : gpu.Visible)
                {
                    const SeamDeltas s = ComputeSeams_GLSLEquivalent(node, kTestDepth, lodMap);
                    for (const u32 delta : { s.PlusX, s.MinusX, s.PlusZ, s.MinusZ })
                    {
                        ASSERT_LE(delta, kMaxSeamDelta) << "a seam delta must never exceed log2(patch grid)";
                        if (delta > 0)
                            ++nonZeroDeltas;
                    }
                }
            }
        }
        EXPECT_GT(nonZeroDeltas, 0u) << "no seam delta was ever non-zero — the packing was never exercised";
    }

    // =========================================================================
    // The crack-freedom contract
    // =========================================================================

    TEST(TerrainGPUQuadtree, AdjacentPatchEdgesShareExactlyTheSameVertexPositions)
    {
        const std::vector<glm::vec2> pyramid = MakeTestPyramid();

        u32 boundariesChecked = 0;
        u32 mismatchedLevelBoundaries = 0;
        for (const auto& [eye, viewProjection] : MakeCameraPoses(kWorldSizeX, kWorldSizeZ))
        {
            for (const f32 target : kTargetTriangleSizes)
            {
                const Frustum frustum(viewProjection);
                const DescentResult gpu = RunDescent_GLSLEquivalent(
                    pyramid, kTestDepth, kWorldSizeX, kWorldSizeZ, ExtractPlanes(frustum), eye,
                    viewProjection[1][1] * kViewportHeight * 0.5f, target);
                const std::vector<u32> lodMap = BuildLODMap_GLSLEquivalent(gpu.SplitMap, kTestDepth);

                std::map<GpuNodeCoord, SeamDeltas> seams;
                std::map<std::pair<u32, u32>, GpuNodeCoord> owner;
                for (const GpuNodeCoord& node : gpu.Visible)
                {
                    seams[node] = ComputeSeams_GLSLEquivalent(node, kTestDepth, lodMap);
                    const u32 step = 1u << (kTestDepth - node.Level);
                    for (u32 dy = 0; dy < step; ++dy)
                        for (u32 dx = 0; dx < step; ++dx)
                            owner[{ node.X * step + dx, node.Y * step + dy }] = node;
                }

                const u32 resolution = 1u << kTestDepth;
                for (const GpuNodeCoord& node : gpu.Visible)
                {
                    const u32 step = 1u << (kTestDepth - node.Level);

                    // Walk every finest texel along the +X edge so a coarse node
                    // bordering several finer ones is covered once per neighbour.
                    std::set<GpuNodeCoord> neighbours;
                    for (u32 d = 0; d < step; ++d)
                    {
                        const u32 tx = node.X * step + step;
                        const u32 ty = node.Y * step + d;
                        if (tx >= resolution)
                            continue;
                        if (auto it = owner.find({ tx, ty }); it != owner.end())
                            neighbours.insert(it->second);
                    }

                    for (const GpuNodeCoord& neighbour : neighbours)
                    {
                        ++boundariesChecked;
                        if (neighbour.Level != node.Level)
                            ++mismatchedLevelBoundaries;

                        std::set<f64> mine;
                        for (u32 g = 0; g <= kK; ++g)
                            mine.insert(PatchVertexUV_GLSLEquivalent(node, seams[node], static_cast<i32>(kK), static_cast<i32>(g)).y);

                        std::set<f64> theirs;
                        for (u32 g = 0; g <= kK; ++g)
                            theirs.insert(PatchVertexUV_GLSLEquivalent(neighbour, seams[neighbour], 0, static_cast<i32>(g)).y);

                        // The shared span is the overlap of the two extents.
                        const f64 mySpan = 1.0 / static_cast<f64>(1u << node.Level);
                        const f64 theirSpan = 1.0 / static_cast<f64>(1u << neighbour.Level);
                        const f64 lo = std::max(static_cast<f64>(node.Y) * mySpan, static_cast<f64>(neighbour.Y) * theirSpan);
                        const f64 hi = std::min(static_cast<f64>(node.Y + 1) * mySpan, static_cast<f64>(neighbour.Y + 1) * theirSpan);

                        std::set<f64> mineInSpan;
                        std::ranges::copy_if(mine, std::inserter(mineInSpan, mineInSpan.end()),
                                             [&](f64 v)
                                             { return v >= lo - 1e-12 && v <= hi + 1e-12; });
                        std::set<f64> theirsInSpan;
                        std::ranges::copy_if(theirs, std::inserter(theirsInSpan, theirsInSpan.end()),
                                             [&](f64 v)
                                             { return v >= lo - 1e-12 && v <= hi + 1e-12; });

                        // Exact equality, not a tolerance: both sides are integer
                        // multiples of the same dyadic step, so any difference at
                        // all is a real crack, not a rounding artefact.
                        ASSERT_EQ(mineInSpan, theirsInSpan)
                            << "+X edge of node (L" << node.Level << "," << node.X << "," << node.Y
                            << ") does not match -X edge of node (L" << neighbour.Level << ","
                            << neighbour.X << "," << neighbour.Y << ") at target=" << target << " — this is a crack";
                    }
                }
            }
        }

        EXPECT_GT(boundariesChecked, 0u) << "no +X boundaries were examined";
        // The load-bearing guard: if every boundary joined two nodes at the SAME
        // level the snapping was never exercised and the equality above is
        // trivially true. This fired for real on the first run, when every pose
        // used the shipped 8 px target and the descent always reached leaves.
        EXPECT_GT(mismatchedLevelBoundaries, 0u)
            << "every boundary joined two nodes at the same level — the seam snapping was never exercised";
    }

    TEST(TerrainGPUQuadtree, EdgeSnappingLeavesCornersAndTheUnsnappedInteriorAlone)
    {
        // Corners sit at grid index 0 and K, both multiples of every 2^delta up
        // to K, so no delta may move them — that is what keeps a patch's four
        // corners welded to its neighbours' regardless of the seam pattern.
        for (u32 delta = 0; delta <= kMaxSeamDelta; ++delta)
        {
            EXPECT_EQ(SnapEdgeIndex_GLSLEquivalent(0, delta), 0);
            EXPECT_EQ(SnapEdgeIndex_GLSLEquivalent(static_cast<i32>(kK), delta), static_cast<i32>(kK));
        }

        // A zero delta is the identity everywhere, so an interior patch pays
        // nothing for the mechanism.
        for (i32 g = 0; g <= static_cast<i32>(kK); ++g)
            EXPECT_EQ(SnapEdgeIndex_GLSLEquivalent(g, 0), g);

        // A delta of d keeps exactly K/2^d + 1 distinct positions on the edge —
        // the same count the neighbour d levels coarser has over that span.
        for (u32 delta = 1; delta <= kMaxSeamDelta; ++delta)
        {
            std::set<i32> distinct;
            for (i32 g = 0; g <= static_cast<i32>(kK); ++g)
                distinct.insert(SnapEdgeIndex_GLSLEquivalent(g, delta));
            EXPECT_EQ(distinct.size(), static_cast<sizet>(kK >> delta) + 1u)
                << "delta " << delta << " should leave K/2^delta + 1 distinct edge vertices";
        }
    }
} // namespace OloEngine::Tests
