// =============================================================================
// VoxelGreedyMeshVisualEvidenceTest.cpp
//
// Visual + measured evidence for the binary greedy mesher / packed-quad voxel
// path (issue #727). The CPU contract tests in
// tests/Terrain/VoxelGreedyMesherTest.cpp prove the FACE SET is right; they
// cannot prove the GPU rebuilds those faces in the right place, the right way
// round, or at all. That is this file's job.
//
// What it checks:
//
//   1. Acceptance criterion 1 — the same voxel volume meshed by both paths, with
//      both triangle counts MEASURED and reported. The greedy path must come out
//      substantially cheaper. Nothing is forecast; the numbers are printed so
//      they can be quoted.
//
//   2. Acceptance criterion 2 — four camera angles, one of them looking straight
//      ALONG a chunk border and one from underneath. Each frame must show real
//      terrain coverage. A seam that dropped a boundary quad, or a whole
//      direction rendered inside-out, collapses coverage from that angle while
//      leaving the others intact — which is exactly why one angle is not enough.
//
//   3. The cubic world sits where the height field it was seeded from says it
//      does: coverage masks from the same pose are compared and the overlap
//      reported. A wholesale offset (a chunk transform applied twice, a wrong
//      chunk origin) shows up here even though every CPU test still passes.
//
// Screenshots land in OloEditor/assets/tests/visual/ under --olo-golden-rebase,
// following the FoliageGenerationEvidenceTest convention (CI stays clean; a
// human runs the flag and looks at the PNGs).
//
// Runs in the normal suite and SKIPs cleanly when there is no GL 4.6 context.
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
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Terrain/Voxel/MarchingCubes.h"
#include "OloEngine/Terrain/Voxel/VoxelGreedyMeshBuilder.h"
#include "OloEngine/Terrain/Voxel/VoxelGreedyMesher.h"
#include "OloEngine/Terrain/Voxel/VoxelOverride.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <utility>
#include <cstdio>
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

        // 2 world units per voxel keeps the seeded volume to a handful of chunks
        // while staying coarse enough that the blockiness is unmistakable in the
        // screenshots. Chunk world size is CHUNK_SIZE * this = 64.
        constexpr f32 kVoxelSize = 2.0f;
        constexpr f32 kChunkWorldSize = static_cast<f32>(VoxelChunk::CHUNK_SIZE) * kVoxelSize;

        constexpr f32 kTerrainExtent = 192.0f;
        constexpr f32 kHeightScale = 40.0f;

        [[nodiscard]] bool GoldenRebaseRequested()
        {
            return OloEngine::Tests::Options().GoldenRebase;
        }

        struct CameraPose
        {
            const char* Name;
            glm::vec3 Position;
            f32 Yaw;
            f32 Pitch;
        };

        // EditorCamera convention: POSITIVE pitch tilts the view DOWN, and
        // yaw 0 looks along -Z (see EditorCamera::GetOrientation). Every pose
        // below also keeps sky in the top-left corner, which is where
        // CoverageMask samples its background reference.
        //
        // 'chunk_border' sits exactly on the x = kChunkWorldSize plane and looks
        // straight along it, so a dropped or double-drawn boundary quad is
        // edge-on and runs the depth of the frame instead of hiding a few pixels
        // wide. 'from_below' looks UP at the volume's underside from outside its
        // footprint: -Y faces exist nowhere else in a heightfield volume, and a
        // face direction rendered inside-out is invisible from every angle
        // except the one that needs it.
        const CameraPose kPoses[] = {
            { "overview", glm::vec3(-40.0f, 90.0f, -40.0f), 2.356f, 0.35f },
            { "grazing", glm::vec3(20.0f, 34.0f, -10.0f), 2.489f, 0.15f },
            { "chunk_border", glm::vec3(kChunkWorldSize, 55.0f, -30.0f), 3.1416f, 0.19f },
            { "from_below", glm::vec3(-30.0f, -55.0f, -30.0f), 2.356f, -0.42f },
        };

        // Coverage is sampled from the SceneColor ENTITY-ID attachment, not
        // from pixel colour.
        //
        // A colour-difference mask measures BRIGHTNESS, not geometry, and the
        // two meshers shade differently: measured this way the marching-cubes
        // reference reads as "no terrain" wherever its surface happens to shade
        // dark, and the silhouette comparison collapsed to 0.50 IoU with both
        // paths rendering the same footprint perfectly. The entity ID is written
        // by every one of these shaders and is invariant to shading, so it
        // answers the only question being asked: is this pixel the terrain?
        //
        // Sampled on a coarse grid because Framebuffer::ReadPixel is a
        // one-pixel synchronous readback; a full-resolution mask would be tens
        // of thousands of GPU round-trips.
        constexpr u32 kMaskCols = 96;
        constexpr u32 kMaskRows = 54;

        [[nodiscard]] std::vector<u8> EntityCoverageMask(Framebuffer& framebuffer, i32 entityId)
        {
            std::vector<u8> mask(static_cast<sizet>(kMaskCols) * kMaskRows, 0);
            for (u32 row = 0; row < kMaskRows; ++row)
            {
                const int y = static_cast<int>((row * kHeight) / kMaskRows);
                for (u32 col = 0; col < kMaskCols; ++col)
                {
                    const int x = static_cast<int>((col * kWidth) / kMaskCols);
                    mask[static_cast<sizet>(row) * kMaskCols + col] =
                        (framebuffer.ReadPixel(1, x, y) == entityId) ? u8{ 1 } : u8{ 0 };
                }
            }
            return mask;
        }

        [[nodiscard]] f32 CoverageFraction(const std::vector<u8>& mask)
        {
            sizet covered = 0;
            for (u8 m : mask)
            {
                covered += m;
            }
            return mask.empty() ? 0.0f : static_cast<f32>(covered) / static_cast<f32>(mask.size());
        }

        [[nodiscard]] f32 IntersectionOverUnion(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            sizet intersection = 0;
            sizet unionCount = 0;
            for (sizet i = 0; i < a.size() && i < b.size(); ++i)
            {
                intersection += (a[i] != 0 && b[i] != 0) ? 1 : 0;
                unionCount += (a[i] != 0 || b[i] != 0) ? 1 : 0;
            }
            return unionCount == 0 ? 0.0f : static_cast<f32>(intersection) / static_cast<f32>(unionCount);
        }

        void WritePng(const std::string& name, const std::vector<u8>& rgba)
        {
            std::vector<u8> flipped(rgba); // GL readback is bottom-up
            const sizet rowBytes = static_cast<sizet>(kWidth) * 4u;
            for (u32 y = 0; y < kHeight / 2u; ++y)
            {
                u8* a = flipped.data() + static_cast<sizet>(y) * rowBytes;
                u8* b = flipped.data() + static_cast<sizet>(kHeight - 1u - y) * rowBytes;
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
    } // namespace

    class VoxelGreedyMeshVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            // The editor grid is an infinite plane at y = 0 that would sit
            // between the camera and the volume's underside, and — worse —
            // count as "covered" in every mask below. A coverage assertion that
            // passes on the GRID rather than on the terrain is exactly the kind
            // of test that reports success while the feature is broken.
            scene.SetGridVisible(false);

            {
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.35f, -0.85f, -0.4f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 3.5f;
            }

            // Fill light from below-and-behind. Not decoration: with a single
            // top-down sun, +Y faces are bright and the other five orientations
            // sit at or past grazing incidence, so they render black — and a
            // black face is indistinguishable from a MISSING one. Lighting every
            // orientation is what makes "this angle drew nothing" mean
            // something.
            {
                Entity fill = scene.CreateEntity("Fill");
                auto& dl = fill.AddComponent<DirectionalLightComponent>();
                // Travels up-and-outward, so between the two lights every one of
                // the six face directions has a positive lambert term: the sun
                // covers +X/+Y/+Z, this covers -X/-Y/-Z. Bright enough that the
                // lit result clears the coverage threshold — at a timid
                // intensity the underside stays within noise of the background
                // and the assertion silently stops testing anything.
                dl.m_Direction = glm::normalize(glm::vec3(0.45f, 0.8f, 0.4f));
                dl.m_Color = glm::vec3(0.8f, 0.85f, 1.0f);
                dl.m_Intensity = 3.2f;
                dl.m_CastShadows = false;
            }

            m_TerrainEntity = scene.CreateEntity("VoxelTerrain");
            {
                auto& terrain = m_TerrainEntity.AddComponent<TerrainComponent>();
                terrain.m_ProceduralEnabled = true;
                terrain.m_ProceduralSeed = 1337;
                terrain.m_ProceduralResolution = 128;
                terrain.m_ProceduralOctaves = 4;
                terrain.m_ProceduralFrequency = 2.5f;
                terrain.m_WorldSizeX = kTerrainExtent;
                terrain.m_WorldSizeZ = kTerrainExtent;
                terrain.m_HeightScale = kHeightScale;
                terrain.m_TessellationEnabled = false;
                terrain.m_CollisionEnabled = false;
                terrain.m_Material = Ref<TerrainMaterial>::Create();

                terrain.m_VoxelEnabled = true;
                terrain.m_VoxelSize = kVoxelSize;
                terrain.m_VoxelMesher = VoxelMesherKind::GreedyCubic;
            }
        }

        // Ticks until the terrain height field exists and every greedy mesh job
        // has been collected. Meshing is asynchronous by design, so a fixed frame
        // count would be a flake; FlushPending makes it deterministic.
        void SettleVoxelMeshes(const EditorCamera& camera)
        {
            RunEditorFrames(camera, 3);

            auto& terrain = m_TerrainEntity.GetComponent<TerrainComponent>();
            if (terrain.m_VoxelQuadMeshes && terrain.m_VoxelOverride)
            {
                terrain.m_VoxelQuadMeshes->FlushPending(*terrain.m_VoxelOverride);
            }

            RunEditorFrames(camera, 2);
        }

        [[nodiscard]] static EditorCamera MakeCamera(const CameraPose& pose)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 3000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(pose.Position, pose.Yaw, pose.Pitch);
            return camera;
        }

        // Colour readback (for the human-readable PNGs) plus an entity-ID mask
        // (for every assertion) from the same rendered frame.
        void CaptureSceneColor(std::vector<u8>& outPixels, std::vector<u8>& outMask)
        {
            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No SceneColor framebuffer";
            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<sizet>(kWidth) * kHeight * 4u);
            outMask = EntityCoverageMask(*fb, static_cast<i32>(std::to_underlying(static_cast<entt::entity>(m_TerrainEntity))));
        }

        Entity m_TerrainEntity;
    };

    TEST_F(VoxelGreedyMeshVisualEvidenceTest, PackedQuadsRenderFromEveryAngleAndCostFarLess)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        EditorCamera settleCamera = MakeCamera(kPoses[0]);
        SettleVoxelMeshes(settleCamera);

        auto& terrain = m_TerrainEntity.GetComponent<TerrainComponent>();
        ASSERT_TRUE(terrain.m_VoxelOverride) << "the voxel override was never created";
        ASSERT_GT(terrain.m_VoxelOverride->GetChunkCount(), 0u)
            << "the cubic path seeded no voxel chunks from the height field — nothing to mesh";
        ASSERT_TRUE(terrain.m_VoxelQuadMeshes) << "the greedy mesh builder was never created";

        const u32 greedyQuads = terrain.m_VoxelQuadMeshes->GetQuadCount();
        const u32 greedyTriangles = terrain.m_VoxelQuadMeshes->GetTriangleCount();
        ASSERT_GT(greedyQuads, 0u) << "the greedy mesher produced no quads for a seeded volume";

        // -- Criterion 1: measured cost, same volume, both meshers --
        //
        // Two figures, because they behave very differently:
        //
        //   * TRIANGLES depend entirely on how mergeable the surface is. A
        //     high-frequency noise field at coarse voxels gives almost every
        //     column its own height, so top faces cannot merge and the mesh is
        //     dominated by one-voxel-wide cliff strips - close to the WORST case
        //     for greedy meshing. The terraced measurement below is the shape a
        //     blocky world actually has.
        //   * GEOMETRY BYTES do not: an 8-byte packed quad against marching
        //     cubes' 24-byte position+normal vertices (unwelded, three per
        //     triangle) plus a 4-byte index each. That ratio is the packed-quad
        //     design's actual claim and it holds whatever the terrain looks like.
        auto measureMarchingCubes = [](VoxelOverride& volume, u32& outTriangles, u64& outBytes)
        {
            for (auto& [coord, chunk] : volume.GetChunks())
            {
                chunk.Dirty = true;
            }
            std::unordered_map<VoxelCoord, VoxelMesh, VoxelCoordHash> mcMeshes;
            MarchingCubes::RebuildDirtyMeshes(volume, mcMeshes);

            outTriangles = 0;
            outBytes = 0;
            for (const auto& [coord, mesh] : mcMeshes)
            {
                outTriangles += mesh.IndexCount / 3u;
                // MarchingCubes emits three unwelded vertices per triangle, so
                // the vertex count equals the index count.
                outBytes += static_cast<u64>(mesh.IndexCount) * (sizeof(VoxelVertex) + sizeof(u32));
            }
        };

        // Measured on an INDEPENDENT volume seeded from the same height field,
        // never on the live one. measureMarchingCubes marks every chunk dirty,
        // and the live volume is still being rendered by the pose loop below:
        // dirtying it there would hand VoxelGreedyMeshBuilder::Update a pile of
        // async re-mesh jobs that the loop's fixed 2 frames per pose have no
        // reason to have finished. That is exactly the fixed-frame-count flake
        // SettleVoxelMeshes exists to avoid, reintroduced from the other end.
        u32 marchingCubesTriangles = 0;
        u64 marchingCubesBytes = 0;
        {
            auto reference = Ref<VoxelOverride>::Create();
            reference->Initialize(kTerrainExtent, kTerrainExtent, kHeightScale, kVoxelSize);
            ASSERT_TRUE(terrain.m_TerrainData);
            reference->SeedFromHeightmap(*terrain.m_TerrainData, kTerrainExtent, kTerrainExtent, kHeightScale);
            ASSERT_EQ(reference->GetChunkCount(), terrain.m_VoxelOverride->GetChunkCount())
                << "the reference volume must be the same seed as the live one for the comparison to mean anything";
            measureMarchingCubes(*reference, marchingCubesTriangles, marchingCubesBytes);
        }

        ASSERT_GT(marchingCubesTriangles, 0u) << "marching cubes produced nothing for the same volume - "
                                                 "the comparison would be vacuous";

        // Denominators, asserted here rather than inferred from the quad-count
        // assert above: both ratios divide by these.
        ASSERT_GT(greedyTriangles, 0u);
        const u64 packedBytes = static_cast<u64>(greedyQuads) * sizeof(PackedQuad);
        ASSERT_GT(packedBytes, 0u);
        const f32 triangleRatio = static_cast<f32>(marchingCubesTriangles) / static_cast<f32>(greedyTriangles);
        const f32 byteRatio = static_cast<f32>(marchingCubesBytes) / static_cast<f32>(packedBytes);

        std::printf("[#727] rugged noise terrain - greedy: %u quads / %u tris / %llu B | "
                    "marching cubes: %u tris / %llu B | %.2fx fewer tris, %.1fx less geometry\n",
                    greedyQuads, greedyTriangles, static_cast<unsigned long long>(packedBytes),
                    marchingCubesTriangles, static_cast<unsigned long long>(marchingCubesBytes),
                    static_cast<double>(triangleRatio), static_cast<double>(byteRatio));

        // A floor under the worst case, not a typical figure. Deliberately well
        // below the measured value so terrain-generator tuning cannot make this
        // test lie in either direction.
        EXPECT_GT(triangleRatio, 1.4f)
            << "greedy produced " << greedyTriangles << " triangles vs marching cubes' " << marchingCubesTriangles;
        EXPECT_GT(byteRatio, 10.0f)
            << "packed quads cost " << packedBytes << " B vs marching cubes' " << marchingCubesBytes << " B";

        // -- The same comparison on a TERRACED volume --
        // What a cubic voxel world actually looks like, and where merging can do
        // its job. Meshed straight off a second volume - no rendering - so it
        // costs a fraction of a second on top of the frames above.
        {
            auto terraced = Ref<VoxelOverride>::Create();
            terraced->Initialize(kTerrainExtent, kTerrainExtent, kHeightScale, kVoxelSize);

            auto terracedData = Ref<TerrainData>::Create();
            TerrainGenerator::HeightParams params;
            params.Resolution = 128;
            params.Seed = 1337;
            params.Octaves = 3;
            params.Frequency = 1.5f;
            params.Shaping.TerraceSteps = 6; // the blocky-world shape
            params.Shaping.TerraceSharpness = 0.9f;
            TerrainGenerator::GenerateHeightmap(*terracedData, params);

            terraced->SeedFromHeightmap(*terracedData, kTerrainExtent, kTerrainExtent, kHeightScale);
            ASSERT_GT(terraced->GetChunkCount(), 0u);

            VoxelGreedyMesher mesher;
            u32 terracedQuads = 0;
            for (const auto& [coord, chunk] : terraced->GetChunks())
            {
                VoxelNeighbourhood neighbourhood;
                VoxelGreedyMesher::Gather(*terraced, coord, neighbourhood);
                std::vector<PackedQuad> quads;
                mesher.Mesh(neighbourhood, quads);
                terracedQuads += static_cast<u32>(quads.size());
            }
            ASSERT_GT(terracedQuads, 0u);

            u32 terracedMcTriangles = 0;
            u64 terracedMcBytes = 0;
            measureMarchingCubes(*terraced, terracedMcTriangles, terracedMcBytes);
            ASSERT_GT(terracedMcTriangles, 0u);

            const u32 terracedTriangles = terracedQuads * 2u;
            const u64 terracedPackedBytes = static_cast<u64>(terracedQuads) * sizeof(PackedQuad);
            const f32 terracedTriangleRatio =
                static_cast<f32>(terracedMcTriangles) / static_cast<f32>(terracedTriangles);
            const f32 terracedByteRatio =
                static_cast<f32>(terracedMcBytes) / static_cast<f32>(terracedPackedBytes);

            std::printf("[#727] terraced terrain    - greedy: %u quads / %u tris / %llu B | "
                        "marching cubes: %u tris / %llu B | %.2fx fewer tris, %.1fx less geometry\n",
                        terracedQuads, terracedTriangles,
                        static_cast<unsigned long long>(terracedPackedBytes),
                        terracedMcTriangles, static_cast<unsigned long long>(terracedMcBytes),
                        static_cast<double>(terracedTriangleRatio), static_cast<double>(terracedByteRatio));

            EXPECT_GT(terracedTriangleRatio, 1.4f)
                << "a terraced world should merge at least as well as a rugged one";
        }

        // ── Criterion 2: it actually draws, from every angle ──
        std::vector<std::vector<u8>> masks;
        for (const auto& pose : kPoses)
        {
            EditorCamera camera = MakeCamera(pose);
            RunEditorFrames(camera, 2);

            std::vector<u8> frame;
            std::vector<u8> mask;
            CaptureSceneColor(frame, mask);
            ASSERT_FALSE(frame.empty());
            ASSERT_FALSE(mask.empty());

            if (GoldenRebaseRequested())
            {
                WritePng(std::string("VoxelGreedy_") + pose.Name + ".png", frame);
            }

            const f32 coverage = CoverageFraction(mask);
            std::printf("[#727] pose %-13s coverage %.3f\n", pose.Name, static_cast<double>(coverage));

            // A whole face direction rendered inside-out, or a chunk that never
            // uploaded, drops the frame it dominates to near-nothing while the
            // other angles stay fine. 5% is far below what a filled landscape
            // gives and far above what an empty frame gives.
            EXPECT_GT(coverage, 0.05f) << "pose '" << pose.Name << "' rendered almost nothing";
            masks.push_back(std::move(mask));
        }

        // -- The cubic world sits where the height field says it does --
        //
        // The reference here is the HEIGHTMAP SURFACE, not a marching-cubes
        // isosurface, and the name says so deliberately.
        //
        // Switching the mesher back drops the auto-seeded volume (Scene.cpp:
        // leaving it would z-fight the returning heightmap surface), so
        // MarchingCubes has no chunks left to mesh and the reference leg draws
        // the height field alone. That is asserted below rather than assumed,
        // because the earlier version of this block was NAMED after marching
        // cubes while rendering exactly this - a label nothing checked.
        //
        // It is also the better reference: the voxels were seeded FROM this
        // height field, so it is the ground truth for "is the blocky world in
        // the same place". The marching-cubes COST comparison above is genuine
        // marching cubes, measured on its own volume.
        {
            terrain.m_VoxelMesher = VoxelMesherKind::MarchingCubes;
            for (auto& [coord, chunk] : terrain.m_VoxelOverride->GetChunks())
            {
                chunk.Dirty = true;
            }

            // The OVERVIEW pose, deliberately, not a grazing one. Seen from
            // above both cover the same terrain footprint; seen from the side
            // the cubic volume is a solid SLAB with vertical outer walls while
            // the reference is a thin surface, so a big chunk of the frame
            // differs for a reason that has nothing to do with correctness.
            EditorCamera camera = MakeCamera(kPoses[0]);
            RunEditorFrames(camera, 3);

            EXPECT_TRUE(terrain.m_VoxelMeshes.empty())
                << "the reference leg is supposed to render the bare height field: the auto-seeded "
                   "volume should have been dropped on the mesher switch, leaving marching cubes nothing to mesh";

            std::vector<u8> frame;
            std::vector<u8> mcMask;
            CaptureSceneColor(frame, mcMask);
            if (GoldenRebaseRequested())
            {
                WritePng("VoxelGreedy_heightfield_reference.png", frame);
            }
            const f32 iou = IntersectionOverUnion(masks[0], mcMask);
            std::printf("[#727] silhouette overlap greedy vs source height field: %.3f IoU\n", static_cast<double>(iou));

            // Blocky and smooth differ along every stair-step, so this is not a
            // pixel-identity claim - it is "the same terrain, in the same place".
            // A doubled chunk transform or a one-voxel origin error tanks it.
            // Measured on the entity-ID mask, so it is unaffected by the two
            // shaders producing different colours for the same geometry.
            // Floor, not a target. The measured value is ~0.77 with everything
            // correct, and it is not ~1.0 for two structural reasons visible in
            // the PNGs: the cubic volume is a solid slab whose vertical outer
            // walls project to pixels the thin marching-cubes surface has no
            // counterpart for, and the blocky surface is quantised to whole
            // voxels so its outline staircases against a smooth one. What this
            // DOES catch is gross misplacement - a chunk transform applied
            // twice, or a wrong chunk origin, drops the overlap to near zero.
            // It is not sensitive enough to catch a sub-voxel offset; the CPU
            // contract tests own that.
            EXPECT_GT(iou, 0.70f) << "the cubic world is not where the height field it was seeded from says it is (IoU " << iou << ")";
        }
    }
} // namespace OloEngine::Tests
