// OLO_TEST_LAYER: L8
// =============================================================================
// TerrainShadowCasterEvidenceTest.cpp — a terrain's shadow does not move when
// its casters coarsen (issue #1721).
//
// A terrain casts its sun shadow from meshes that follow the main view's detail
// (TerrainShadowLod): each chunk from the coarsest of its four index sets whose
// quads are no larger than the triangles the main view draws that ground with,
// and a terrain whose every chunk would cast from the coarsest set from ONE mesh
// of the whole tile instead. TerrainShadowLodTest pins the selection rule as a
// pure function; this pins what the rule may not change: where the shadow falls.
//
// THE SCENE. A 64 m terrain from a 513 px height map the test writes (9 x 9
// chunks, ceil(513 / 64) a side): a plain, a cliff rising 24 m between x = 40 and 48 m, a plateau, a
// fall back to zero at the far edge. The sun stands 45 degrees up behind the
// plateau, so the plateau's rim at x = 48 m throws its shadow 24 m across the
// plain: the shadow's edge is at x = 24 m. The camera looks straight down.
//
// THREE ARMS:
//   Chunks        0.5 px triangles: every chunk casts from its finest set,
//   TileMesh       24 px triangles: the whole terrain casts from the tile mesh,
//   Untessellated  24 px triangles, tessellation off: the terrain is drawn at
//                  full density at any distance, so it must NOT cast from the
//                  tile mesh, whose heights are filtered to its spacing.
// Each arm checks, with the engine's own rule on this camera and the chunk
// manager's own quad size, the level it expects, and then the number of
// terrain casters the shadow pass was handed: one for the tile mesh, one per
// chunk otherwise. That count is what says which path cast.
//
// WHAT IS ASSERTED, per render path, in this process's backend: the plain is
// lit on the sunny side of x = 24 m and shadowed between there and the cliff,
// and the edge between them is within 1.5 m of 24 m (0.6 m for the two arms
// whose casters carry exact heights).
//
// WHAT THIS FOUND. Written on a 129 px map (3 x 3 chunks), the TileMesh arm
// put the edge at 26.9 m on every path: a chunk's coarsest mesh was 2.7 m
// quads there, the main view's root node 1 m quads, and a caster that much
// blunter than the ground drawn rounds a rim off by the height its filter
// loses. The rule now stops at the root node's quad
// (TerrainShadowLod::CoarsestReceiverQuad), and on that map the tile mesh is
// never chosen. And the review of this file found that a terrain with
// tessellation off still took the tile mesh; the third arm is that case.
//
// Evidence: OloEditor/assets/tests/visual/TerrainShadowCasters<Arm>_<Backend>_<Path>.png.
// Runs in the --olo-rhi=vulkan process too.
//
// Classification: L8 (full pipeline through the real Scene render path,
// readback + PNG). SKIPs cleanly without a GPU context.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "TestRenderBackend.h"
#include "../../TestTempDir.h"

#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Terrain/TerrainChunk.h"
#include "OloEngine/Terrain/TerrainChunkManager.h"
#include "OloEngine/Terrain/TerrainShadowLod.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
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

        constexpr u32 kFrame = 256;
        constexpr u32 kSamples = 513; // 8 x 8 chunks of 64 quads, an eighth of a metre each
        constexpr f32 kWorldSize = 64.0f;
        constexpr f32 kHeightScale = 24.0f;
        constexpr f32 kCliffStart = 40.0f;
        constexpr f32 kCliffEnd = 48.0f; // the rim that casts
        constexpr f32 kPlateauEnd = 56.0f;
        constexpr f32 kShadowEdge = kCliffEnd - kHeightScale; // a 45 degree sun: 24 m

        constexpr f32 kCameraHeight = 110.0f;
        constexpr f32 kCameraFovDegrees = 40.0f;

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

        // The image column that looks at world x on the plain (y = 0), for the
        // camera below: straight down over the tile's centre, image right +X.
        [[nodiscard]] f32 ColumnOf(const f32 x)
        {
            const f32 halfExtent = kCameraHeight * std::tan(glm::radians(kCameraFovDegrees) * 0.5f);
            return (static_cast<f32>(kFrame) * 0.5f) +
                   ((x - (kWorldSize * 0.5f)) / (2.0f * halfExtent)) * static_cast<f32>(kFrame);
        }
    } // namespace

    class TerrainShadowCasterEvidence : public RendererAttachedTest, public ::testing::WithParamInterface<RenderingPath>
    {
      protected:
        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return true;
        }

        // The arm: what the main view's triangles are sized to, in pixels.
        [[nodiscard]] virtual f32 TargetTrianglePixels() const = 0;
        [[nodiscard]] virtual u32 ExpectedLevel() const = 0;
        [[nodiscard]] virtual const char* ArmName() const = 0;
        [[nodiscard]] virtual bool Tessellated() const
        {
            return true;
        }
        // Whether the whole terrain is expected to cast as ONE caster.
        [[nodiscard]] virtual bool ExpectsTheTileMesh() const = 0;
        // How close to the predicted edge this arm's casters put it.
        [[nodiscard]] virtual f32 EdgeTolerance() const
        {
            return 1.5f;
        }

        void BuildScene() override
        {
            EnableRendering(kFrame, kFrame);

            auto& rs = Renderer3D::GetRendererSettings();
            rs.Path = GetParam();
            rs.ShowGrid = false;
            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.AutoExposureEnabled = false;
            pp.Exposure = 1.0f;
            pp.Tonemap = TonemapOperator::None;
            Renderer3D::ApplyRendererSettings();

            const fs::path heightmap = TempDir("terrain-shadow-casters") / "height.png";
            std::vector<u8> pixels(static_cast<sizet>(kSamples) * kSamples);
            for (u32 z = 0; z < kSamples; ++z)
            {
                for (u32 x = 0; x < kSamples; ++x)
                {
                    // Sample x sits at x / (N - 1) of the tile.
                    const f32 metres = static_cast<f32>(x) / static_cast<f32>(kSamples - 1u) * kWorldSize;
                    const f32 rise = std::clamp((metres - kCliffStart) / (kCliffEnd - kCliffStart), 0.0f, 1.0f);
                    const f32 fall = std::clamp((kWorldSize - metres) / (kWorldSize - kPlateauEnd), 0.0f, 1.0f);
                    pixels[static_cast<sizet>(z) * kSamples + x] = static_cast<u8>(std::lround(std::min(rise, fall) * 255.0f));
                }
            }
            // The write-side flip is process-wide and has no getter; state it.
            ::stbi_flip_vertically_on_write(0);
            ASSERT_NE(::stbi_write_png(heightmap.string().c_str(), static_cast<int>(kSamples), static_cast<int>(kSamples), 1,
                                       pixels.data(), static_cast<int>(kSamples)),
                      0)
                << "could not write " << heightmap.string();

            m_Camera = GetScene().CreateEntity("Camera");
            auto& cam = m_Camera.AddComponent<CameraComponent>();
            cam.Primary = true;
            cam.Camera.SetViewportSize(kFrame, kFrame);
            auto& transform = m_Camera.GetComponent<TransformComponent>();
            transform.Translation = { kWorldSize * 0.5f, kCameraHeight, kWorldSize * 0.5f };
            transform.SetRotationEuler({ -glm::half_pi<f32>(), 0.0f, 0.0f });
            cam.Camera.SetPerspective(glm::radians(kCameraFovDegrees), 0.5f, 500.0f);

            // Travelling toward -X and down at 45 degrees: from behind the
            // plateau, over its rim, onto the plain.
            Entity sun = GetScene().CreateEntity("Sun");
            auto& dl = sun.AddComponent<DirectionalLightComponent>();
            dl.m_Direction = glm::normalize(glm::vec3(-1.0f, -1.0f, 0.0f));
            dl.m_Color = glm::vec3(1.0f);
            dl.m_Intensity = 2.0f;
            dl.m_CastShadows = true;
            dl.m_MaxShadowDistance = 300.0f;

            m_Terrain = GetScene().CreateEntity("Terrain");
            auto& terrain = m_Terrain.AddComponent<TerrainComponent>();
            terrain.m_HeightmapPath = heightmap.generic_string();
            terrain.m_WorldSizeX = kWorldSize;
            terrain.m_WorldSizeZ = kWorldSize;
            terrain.m_HeightScale = kHeightScale;
            terrain.m_CollisionEnabled = false;
            terrain.m_TargetTriangleSize = TargetTrianglePixels();
            terrain.m_TessellationEnabled = Tessellated();
        }

        // The level the engine's rule gives this camera for the terrain's
        // NEAREST point, which is what decides between chunks and the tile mesh.
        [[nodiscard]] u32 LevelAtTheNearestPoint()
        {
            const glm::mat4 world = m_Camera.GetComponent<TransformComponent>().GetTransform();
            const glm::mat4 viewProjection = m_Camera.GetComponent<CameraComponent>().Camera.GetProjection() * glm::inverse(world);
            const f32 pixelSizePerMetre = TerrainShadowLod::PixelSizePerMetre(viewProjection, static_cast<f32>(kFrame));
            // The engine's quad: the tile over its chunks over a chunk's quads.
            // Not kWorldSize / (kSamples - 1): a 513 px map is 9 chunks a side.
            const auto& terrain = m_Terrain.GetComponent<TerrainComponent>();
            if (!terrain.m_ChunkManager || terrain.m_ChunkManager->GetNumChunksX() == 0)
                return ~0u;
            const f32 quadSize = kWorldSize / static_cast<f32>(terrain.m_ChunkManager->GetNumChunksX()) /
                                 static_cast<f32>(TerrainChunk::CHUNK_RESOLUTION);
            return TerrainShadowLod::Select(kCameraHeight - kHeightScale, quadSize, pixelSizePerMetre, TargetTrianglePixels(),
                                            TerrainShadowLod::CoarsestReceiverQuad(kWorldSize, kWorldSize));
        }

        void MeasureAndAssert()
        {
            RunFrames(8);
            ASSERT_EQ(LevelAtTheNearestPoint(), ExpectedLevel())
                << "this arm no longer casts from the meshes it is named for; move its triangle size";
            // Which path cast: the tile mesh is one caster for the whole
            // terrain, the chunk path one per chunk in a cascade.
            const u32 casters = Renderer3D::GetTerrainShadowCasterCountLastFrame();
            const u32 chunks = m_Terrain.GetComponent<TerrainComponent>().m_ChunkManager->GetNumChunksX() *
                               m_Terrain.GetComponent<TerrainComponent>().m_ChunkManager->GetNumChunksZ();
            if (ExpectsTheTileMesh())
                ASSERT_EQ(casters, 1u) << "the terrain did not cast from its tile mesh";
            else
            {
                // One per chunk the shadow range reaches: more than one, and
                // never more than the terrain has.
                ASSERT_GT(casters, 1u) << "the terrain cast from one mesh, not from its chunks";
                ASSERT_LE(casters, chunks);
            }
            std::vector<u8> rgba;
            u32 w = 0;
            u32 h = 0;
            ASSERT_TRUE(ReadbackComposite(rgba, w, h)) << "no composited frame to read back";
            ASSERT_EQ(w, kFrame);
            ASSERT_EQ(h, kFrame);
            WriteEvidence(rgba);

            // Luma per column over a 6 m band of the plain around the middle
            // row (the scene is constant along Z).
            const u32 rowMin = kFrame / 2u - 10u;
            const u32 rowMax = kFrame / 2u + 10u;
            const auto columnLuma = [&](const u32 column)
            {
                f64 sum = 0.0;
                for (u32 y = rowMin; y < rowMax; ++y)
                {
                    const sizet at = (static_cast<sizet>(y) * kFrame + column) * 4u;
                    sum += 0.2126 * rgba[at] + 0.7152 * rgba[at + 1] + 0.0722 * rgba[at + 2];
                }
                return sum / static_cast<f64>(rowMax - rowMin);
            };
            const auto meanOver = [&](const f32 fromMetres, const f32 toMetres)
            {
                const u32 first = static_cast<u32>(std::lround(ColumnOf(fromMetres)));
                const u32 last = static_cast<u32>(std::lround(ColumnOf(toMetres)));
                f64 sum = 0.0;
                for (u32 column = first; column <= last; ++column)
                    sum += columnLuma(column);
                return sum / static_cast<f64>(last - first + 1u);
            };

            const f64 lit = meanOver(6.0f, 18.0f);
            const f64 shadowed = meanOver(29.0f, 37.0f);
            // The first column, walking from the sunny side, darker than halfway
            // between the two, refined between it and the column before.
            const f64 half = 0.5 * (lit + shadowed);
            f32 edgeMetres = -1.0f;
            const u32 scanFirst = static_cast<u32>(std::lround(ColumnOf(8.0f)));
            const u32 scanLast = static_cast<u32>(std::lround(ColumnOf(38.0f)));
            const f32 metresPerColumn = 1.0f / (ColumnOf(1.0f) - ColumnOf(0.0f));
            for (u32 column = scanFirst + 1u; column <= scanLast; ++column)
            {
                const f64 here = columnLuma(column);
                if (here >= half)
                    continue;
                const f64 before = columnLuma(column - 1u);
                const f64 t = before > here ? (before - half) / (before - here) : 0.0;
                const f32 columnAtEdge = static_cast<f32>(column - 1u) + static_cast<f32>(std::clamp(t, 0.0, 1.0));
                edgeMetres = (kWorldSize * 0.5f) + (columnAtEdge + 0.5f - static_cast<f32>(kFrame) * 0.5f) * metresPerColumn;
                break;
            }
            std::cout << "[TerrainShadowCasterEvidence] " << ArmName() << " " << PathName(GetParam()) << ": lit plain " << lit
                      << ", shadowed plain " << shadowed << ", edge at x = " << edgeMetres << " m (predicted " << kShadowEdge
                      << " m)\n";

            EXPECT_GT(lit, shadowed * 1.5) << "the plateau casts no shadow on the plain in front of it";
            ASSERT_GE(edgeMetres, 0.0f) << "no shadow edge on the plain";
            EXPECT_NEAR(edgeMetres, kShadowEdge, EdgeTolerance()) << "the shadow's edge is not where the plateau's rim puts it";
        }

      private:
        // Rows arrive bottom-up; the PNG is written top-down.
        void WriteEvidence(const std::vector<u8>& rgba) const
        {
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string name =
                TestRenderBackend::EvidenceName((std::string("TerrainShadowCasters") + ArmName()).c_str()) + PathName(GetParam());
            const sizet rowBytes = static_cast<sizet>(kFrame) * 4u;
            std::vector<u8> topDown(rgba.size());
            for (u32 y = 0; y < kFrame; ++y)
                std::copy_n(rgba.data() + static_cast<sizet>(kFrame - 1u - y) * rowBytes, rowBytes,
                            topDown.data() + static_cast<sizet>(y) * rowBytes);
            ::stbi_flip_vertically_on_write(0);
            EXPECT_NE(::stbi_write_png((dir / (name + ".png")).string().c_str(), static_cast<int>(kFrame),
                                       static_cast<int>(kFrame), 4, topDown.data(), static_cast<int>(rowBytes)),
                      0)
                << "could not write " << name;
        }

      protected:
        Entity m_Camera;
        Entity m_Terrain;
    };

    class TerrainShadowFromChunks : public TerrainShadowCasterEvidence
    {
      protected:
        [[nodiscard]] f32 TargetTrianglePixels() const override
        {
            return 0.5f;
        }
        [[nodiscard]] u32 ExpectedLevel() const override
        {
            return 0u;
        }
        [[nodiscard]] const char* ArmName() const override
        {
            return "Chunks";
        }
        [[nodiscard]] bool ExpectsTheTileMesh() const override
        {
            return false;
        }
        [[nodiscard]] f32 EdgeTolerance() const override
        {
            return 0.6f;
        }
    };

    class TerrainShadowFromTheTileMesh : public TerrainShadowCasterEvidence
    {
      protected:
        [[nodiscard]] f32 TargetTrianglePixels() const override
        {
            return 24.0f;
        }
        [[nodiscard]] u32 ExpectedLevel() const override
        {
            return TerrainShadowLod::kLevelCount - 1u;
        }
        [[nodiscard]] const char* ArmName() const override
        {
            return "TileMesh";
        }
        [[nodiscard]] bool ExpectsTheTileMesh() const override
        {
            return true;
        }
    };

    // The same triangle size with tessellation off. The main view then draws
    // every chunk at full density with its vertices' own heights, so the caster
    // may thin its indices with distance (the coarsest level here) but must
    // keep those heights: chunks, not the tile mesh.
    class TerrainShadowUntessellated : public TerrainShadowCasterEvidence
    {
      protected:
        [[nodiscard]] f32 TargetTrianglePixels() const override
        {
            return 24.0f;
        }
        [[nodiscard]] u32 ExpectedLevel() const override
        {
            return TerrainShadowLod::kLevelCount - 1u;
        }
        [[nodiscard]] const char* ArmName() const override
        {
            return "Untessellated";
        }
        [[nodiscard]] bool Tessellated() const override
        {
            return false;
        }
        [[nodiscard]] bool ExpectsTheTileMesh() const override
        {
            return false;
        }
        [[nodiscard]] f32 EdgeTolerance() const override
        {
            return 0.6f;
        }
    };

    TEST_P(TerrainShadowFromChunks, ThePlateauShadowsThePlainWhereItsRimPutsIt)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        MeasureAndAssert();
    }

    TEST_P(TerrainShadowFromTheTileMesh, ThePlateauShadowsThePlainWhereItsRimPutsIt)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        MeasureAndAssert();
    }

    TEST_P(TerrainShadowUntessellated, ThePlateauShadowsThePlainWhereItsRimPutsIt)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        MeasureAndAssert();
    }

    INSTANTIATE_TEST_SUITE_P(AllPaths, TerrainShadowFromChunks,
                             ::testing::Values(RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred),
                             [](const ::testing::TestParamInfo<RenderingPath>& info)
                             { return std::string(PathName(info.param)); });
    INSTANTIATE_TEST_SUITE_P(AllPaths, TerrainShadowFromTheTileMesh,
                             ::testing::Values(RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred),
                             [](const ::testing::TestParamInfo<RenderingPath>& info)
                             { return std::string(PathName(info.param)); });
    INSTANTIATE_TEST_SUITE_P(AllPaths, TerrainShadowUntessellated,
                             ::testing::Values(RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred),
                             [](const ::testing::TestParamInfo<RenderingPath>& info)
                             { return std::string(PathName(info.param)); });
} // namespace OloEngine::Tests
