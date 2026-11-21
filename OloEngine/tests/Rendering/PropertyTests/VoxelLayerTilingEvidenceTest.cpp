// OLO_TEST_LAYER: L8
// =============================================================================
// VoxelLayerTilingEvidenceTest.cpp — a voxel surface shows a layer at the scale
// the tile gives it, per axis (issue #1721).
//
// A layer's TilingScale is repeats across the terrain TILE. On a tile that is
// not square one repeat is a different number of metres along X than along Z,
// and the heightfield surface draws it so. A voxel surface samples a world
// position in metres; it was handed one number, taken from the X size, so on a
// 64 x 128 m tile its texture was square where the ground beside it was 1:2.
//
// THE SCENE. A level terrain 64 m along X and 128 m along Z, seeded into a
// greedy-meshed voxel volume, so the frame is the voxel volume's flat top.
// Every layer is a 2 x 2 checker repeating 8 times across the tile: one cell
// is 4 m along X and 8 m along Z. The camera looks straight down.
//
// WHAT IS ASSERTED, on Forward (Terrain_VoxelGreedy.glsl) and on Deferred
// (Terrain_VoxelGreedy_GBuffer.glsl), in this process's backend: the checker's
// cells measure 4 m along X and 8 m along Z. With the single X-derived scale
// they measure 4 m along both, which the ratio bound rejects.
//
// The side projections and the two marching-cubes shaders use the same helper
// the same way; TerrainLayerTextureContract.AVoxelWallTakesTheTilesScalePerAxis
// counts them in the source.
//
// Evidence: OloEditor/assets/tests/visual/VoxelLayerTiling_<Backend>_<Path>.png.
//
// Classification: L8 (full pipeline through the real Scene render path,
// readback + PNG). SKIPs cleanly without a GPU context.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "TestRenderBackend.h"
#include "../../TestTempDir.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Terrain/Voxel/VoxelGreedyMeshBuilder.h"
#include "OloEngine/Terrain/Voxel/VoxelOverride.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 960;
        constexpr u32 kHeight = 540;
        constexpr f32 kTileX = 64.0f;
        constexpr f32 kTileZ = 128.0f;
        constexpr f32 kHeightScale = 40.0f;
        constexpr u8 kLevel = 128; // the terrain's height, of 255
        constexpr f32 kVoxelSize = 2.0f;
        constexpr f32 kRepeats = 8.0f;                   // TilingScale: repeats across the tile
        constexpr f32 kCellX = kTileX / kRepeats / 2.0f; // a checker cell is half a repeat: 4 m
        constexpr f32 kCellZ = kTileZ / kRepeats / 2.0f; // 8 m
        constexpr u32 kCheckerSize = 64;
        constexpr f32 kCameraHeight = 120.0f;
        constexpr f32 kFovDegrees = 60.0f;

        [[nodiscard]] const char* PathName(RenderingPath path)
        {
            switch (path)
            {
                case RenderingPath::Forward:
                    return "Forward";
                case RenderingPath::ForwardPlus:
                    return "ForwardPlus";
                case RenderingPath::Deferred:
                    return "Deferred";
                default:
                    return "Other";
            }
        }

        void WritePng(const fs::path& file, u32 width, u32 height, int channels, const std::vector<u8>& pixels)
        {
            // The write-side flip is process-wide and has no getter; state it.
            ::stbi_flip_vertically_on_write(0);
            ASSERT_NE(::stbi_write_png(file.string().c_str(), static_cast<int>(width), static_cast<int>(height), channels,
                                       pixels.data(), static_cast<int>(width) * channels),
                      0)
                << "could not write " << file.string();
        }

        // The median length, in samples, of the runs of `values` on one side of
        // the middle of their range, leaving out the cut-off run at each end.
        // Zero when the row holds fewer than three whole runs.
        [[nodiscard]] f64 MedianRun(const std::vector<f64>& values)
        {
            const auto [lo, hi] = std::minmax_element(values.begin(), values.end());
            if (*hi - *lo < 30.0)
                return 0.0; // no checker in this row
            const f64 mid = 0.5 * (*lo + *hi);
            std::vector<f64> runs;
            sizet runStart = 0;
            for (sizet i = 1; i <= values.size(); ++i)
            {
                if (i == values.size() || (values[i] > mid) != (values[runStart] > mid))
                {
                    runs.push_back(static_cast<f64>(i - runStart));
                    runStart = i;
                }
            }
            if (runs.size() < 5)
                return 0.0;
            runs.erase(runs.begin());
            runs.pop_back();
            std::sort(runs.begin(), runs.end());
            return runs[runs.size() / 2];
        }
    } // namespace

    class VoxelLayerTilingEvidence : public RendererAttachedTest, public ::testing::WithParamInterface<RenderingPath>
    {
      protected:
        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return true;
        }

        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);
            scene.SetGridVisible(false);

            auto& rs = Renderer3D::GetRendererSettings();
            rs.Path = GetParam();
            rs.ShowGrid = false;
            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.AutoExposureEnabled = false;
            pp.Exposure = 1.0f;
            pp.Tonemap = TonemapOperator::None;
            Renderer3D::ApplyRendererSettings();

            const fs::path directory = TempDir("voxel-layer-tiling");
            const fs::path heightmap = directory / "level.png";
            const fs::path checker = directory / "checker.png";
            ASSERT_NO_FATAL_FAILURE(WritePng(heightmap, 33, 33, 1, std::vector<u8>(33u * 33u, kLevel)));
            {
                std::vector<u8> pixels(static_cast<sizet>(kCheckerSize) * kCheckerSize * 4u);
                const u32 cell = kCheckerSize / 2u;
                for (u32 y = 0; y < kCheckerSize; ++y)
                {
                    for (u32 x = 0; x < kCheckerSize; ++x)
                    {
                        const u8 value = (((x / cell) + (y / cell)) % 2u) == 0u ? 30 : 230;
                        const sizet at = (static_cast<sizet>(y) * kCheckerSize + x) * 4u;
                        pixels[at] = pixels[at + 1] = pixels[at + 2] = value;
                        pixels[at + 3] = 255;
                    }
                }
                ASSERT_NO_FATAL_FAILURE(WritePng(checker, kCheckerSize, kCheckerSize, 4, pixels));
            }

            {
                // Nearly overhead, so the level top is evenly lit.
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(0.15f, -1.0f, 0.1f));
                dl.m_Color = glm::vec3(1.0f);
                dl.m_Intensity = 3.0f;
                dl.m_CastShadows = false;
            }

            m_Terrain = scene.CreateEntity("VoxelTerrain");
            auto& terrain = m_Terrain.AddComponent<TerrainComponent>();
            terrain.m_HeightmapPath = heightmap.generic_string();
            terrain.m_WorldSizeX = kTileX;
            terrain.m_WorldSizeZ = kTileZ;
            terrain.m_HeightScale = kHeightScale;
            terrain.m_TessellationEnabled = false;
            terrain.m_CollisionEnabled = false;
            terrain.m_Material = Ref<TerrainMaterial>::Create();
            // Whichever layer a voxel's material index names, it is this checker
            // at this tiling.
            for (u32 i = 0; i < MAX_TERRAIN_LAYERS; ++i)
            {
                TerrainLayer layer;
                layer.Name = "Checker " + std::to_string(i);
                layer.AlbedoPath = checker.generic_string();
                layer.TilingScale = kRepeats;
                layer.Roughness = 1.0f;
                layer.Metallic = 0.0f;
                terrain.m_Material->AddLayer(layer);
            }
            terrain.m_Material->SetLayerResolution(kCheckerSize);
            terrain.m_MaterialNeedsRebuild = true;

            terrain.m_VoxelEnabled = true;
            terrain.m_VoxelSize = kVoxelSize;
            terrain.m_VoxelMesher = VoxelMesherKind::GreedyCubic;
        }

        Entity m_Terrain;
    };

    TEST_P(VoxelLayerTilingEvidence, ACheckerCellIsTheTilesOwnSizeAlongEachAxis)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Straight down over the tile's centre (87.7 degrees: the editor camera
        // has no defined heading at exactly 90). Yaw pi looks along +Z, so image
        // columns run along world X and image rows along world Z.
        EditorCamera camera(kFovDegrees, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 3000.0f);
        camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        camera.SetPose({ kTileX * 0.5f, kCameraHeight, kTileZ * 0.5f }, 3.14159265f, 1.53f);

        // The height field, the seeded volume, its greedy meshes and the layer
        // arrays all arrive over the first frames; meshing is asynchronous, so
        // it is flushed rather than waited for.
        RunEditorFrames(camera, 4);
        {
            auto& terrain = m_Terrain.GetComponent<TerrainComponent>();
            ASSERT_TRUE(terrain.m_VoxelOverride) << "the voxel override was never created";
            ASSERT_GT(terrain.m_VoxelOverride->GetChunkCount(), 0u) << "the volume was never seeded from the height field";
            ASSERT_TRUE(terrain.m_VoxelQuadMeshes) << "the greedy mesh builder was never created";
            terrain.m_VoxelQuadMeshes->FlushPending(*terrain.m_VoxelOverride);
        }
        RunEditorFrames(camera, 4);

        std::vector<u8> rgba;
        u32 w = 0;
        u32 h = 0;
        ASSERT_TRUE(ReadbackComposite(rgba, w, h)) << "no composited frame to read back";
        ASSERT_EQ(w, kWidth);
        ASSERT_EQ(h, kHeight);
        {
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string name = TestRenderBackend::EvidenceName("VoxelLayerTiling") + PathName(GetParam()) + ".png";
            std::vector<u8> topDown(rgba.size());
            const sizet rowBytes = static_cast<sizet>(kWidth) * 4u;
            for (u32 y = 0; y < kHeight; ++y)
                std::copy_n(rgba.data() + static_cast<sizet>(kHeight - 1u - y) * rowBytes, rowBytes,
                            topDown.data() + static_cast<sizet>(y) * rowBytes);
            ASSERT_NO_FATAL_FAILURE(WritePng(dir / name, kWidth, kHeight, 4, topDown));
        }

        const auto luma = [&rgba](const u32 x, const u32 y)
        {
            const sizet at = (static_cast<sizet>(y) * kWidth + x) * 4u;
            return 0.2126 * rgba[at] + 0.7152 * rgba[at + 1] + 0.0722 * rgba[at + 2];
        };

        // The volume's top is the terrain's level rounded to a voxel.
        const f32 top = std::round(static_cast<f32>(kLevel) / 255.0f * kHeightScale / kVoxelSize) * kVoxelSize;
        const f64 metresPerPixel = 2.0 * (kCameraHeight - top) * std::tan(glm::radians(kFovDegrees) * 0.5) / kHeight;

        // Along X: the middle row, 25 m either side of the centre (the tile is
        // 64 m wide). Along Z: the middle column, 50 m either side.
        const u32 halfX = static_cast<u32>(25.0 / metresPerPixel);
        const u32 halfZ = static_cast<u32>(50.0 / metresPerPixel);
        ASSERT_LT(halfZ, kHeight / 2u);
        std::vector<f64> alongX;
        for (u32 x = kWidth / 2u - halfX; x < kWidth / 2u + halfX; ++x)
            alongX.push_back(luma(x, kHeight / 2u));
        std::vector<f64> alongZ;
        for (u32 y = kHeight / 2u - halfZ; y < kHeight / 2u + halfZ; ++y)
            alongZ.push_back(luma(kWidth / 2u, y));

        const f64 cellX = MedianRun(alongX) * metresPerPixel;
        const f64 cellZ = MedianRun(alongZ) * metresPerPixel;
        std::cout << "[VoxelLayerTilingEvidence] " << PathName(GetParam()) << ": checker cell " << cellX << " m along X (authored "
                  << kCellX << "), " << cellZ << " m along Z (authored " << kCellZ << ")\n";

        ASSERT_GT(cellX, 0.0) << "no checker along X: the voxel top is not textured";
        ASSERT_GT(cellZ, 0.0) << "no checker along Z: the voxel top is not textured";
        EXPECT_NEAR(cellX, kCellX, kCellX * 0.15) << "a cell is not a sixteenth of the tile's X size";
        EXPECT_NEAR(cellZ, kCellZ, kCellZ * 0.15) << "a cell is not a sixteenth of the tile's Z size";
        EXPECT_NEAR(cellZ / cellX, kCellZ / kCellX, 0.3)
            << "the voxel surface used one scale for both axes of a tile that is not square";
    }

    INSTANTIATE_TEST_SUITE_P(BothShaders, VoxelLayerTilingEvidence, ::testing::Values(RenderingPath::Forward, RenderingPath::Deferred),
                             [](const ::testing::TestParamInfo<RenderingPath>& info)
                             { return std::string(PathName(info.param)); });
} // namespace OloEngine::Tests
