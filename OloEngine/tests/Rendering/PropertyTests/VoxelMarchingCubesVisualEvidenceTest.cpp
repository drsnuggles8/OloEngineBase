// =============================================================================
// VoxelMarchingCubesVisualEvidenceTest.cpp
//
// Visual evidence for the MARCHING-CUBES voxel path (Terrain_Voxel.glsl).
//
// Why this exists: nothing rendered that path. No test and no sandbox scene
// enabled a voxel volume before issue #727, so a shading bug in it could not be
// seen by the suite — and one was there. Both voxel shaders decoded the
// triplanar normal map BEFORE checking whether the terrain normal array was
// bound, and an unbound sampler reads solid black, which `black * 2 - 1` turns
// into a UNIT-LENGTH (-1,-1,-1). That is indistinguishable from a real
// perturbation, so it fed the TBN, rotated the surface normal ~55 degrees off
// true, and rendered the whole volume black while the geometry, the materials
// and the lighting were all provably fine. It was found by looking at a PNG.
//
// The subject is a floating sphere added through VoxelOverride::AddSphere —
// which is what this path is actually FOR (caves, overhangs, carved blobs on
// top of a heightmap terrain). A sphere is the right shape here for three
// reasons: marching cubes renders it smooth, so it cannot be confused with the
// cubic path; it silhouettes against empty sky, so the coverage mask is
// unambiguous; and its normals sweep every direction, including the exactly
// +/-Z ones where `normalize(cross(N, vec3(0,0,1)))` is NaN — the second defect
// fixed alongside the first.
//
// The load-bearing assertion is NOT "did it draw". Geometry was never the
// problem. It is "is what drew actually SHADED" — the fraction of surface
// pixels meaningfully brighter than the background. A black render passes every
// coverage check ever written and fails this one.
//
// Shot twice: once with no terrain layers (the arrays are unbound — the case
// that was broken), then again with the default layer set bound, which is the
// only configuration that reaches the tangent-frame branch at all.
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
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Terrain/Voxel/VoxelOverride.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 960;
        constexpr u32 kHeight = 540;

        constexpr f32 kTerrainExtent = 192.0f;
        constexpr f32 kHeightScale = 40.0f;
        constexpr f32 kVoxelSize = 2.0f;

        // Centred in chunk (1,1,1) — world [64,128) per axis at this voxel size —
        // and small enough to stay inside it: marching cubes meshes each chunk
        // over [0, SIZE-1], so a blob spanning a chunk border shows that path's
        // own seam, which is not what this test is about.
        const glm::vec3 kBlobCentre{ 96.0f, 96.0f, 96.0f };
        constexpr f32 kBlobRadius = 25.0f;

        // Sampling grid for the entity-ID mask. Framebuffer::ReadPixel is a
        // one-pixel synchronous readback, so a full-resolution mask would be
        // tens of thousands of GPU round-trips.
        constexpr u32 kMaskCols = 96;
        constexpr u32 kMaskRows = 54;

        [[nodiscard]] bool GoldenRebaseRequested()
        {
            return OloEngine::Tests::Options().GoldenRebase;
        }

        struct SurfaceSample
        {
            f32 Coverage = 0.0f;    // fraction of the frame that is the terrain entity
            f32 LitFraction = 0.0f; // of THAT, the fraction visibly brighter than the background
        };

        // Coverage comes from the entity-ID attachment (shading-independent, so
        // it answers "is this pixel the subject" and nothing else); brightness
        // comes from the colour attachment at the same grid points. Keeping the
        // two separate is the whole point: it lets the test say "the geometry is
        // there AND it is black", which is exactly the bug this file guards.
        [[nodiscard]] SurfaceSample SampleSurface(Framebuffer& framebuffer, const std::vector<u8>& rgba, i32 entityId)
        {
            // GL readback is bottom-up; the top-left pixel is the last row's first.
            const sizet cornerIndex = (static_cast<sizet>(kHeight - 1) * kWidth) * 4;
            const int backgroundLuma =
                rgba.empty() ? 0 : (rgba[cornerIndex + 0] + rgba[cornerIndex + 1] + rgba[cornerIndex + 2]) / 3;

            sizet covered = 0;
            sizet lit = 0;
            for (u32 row = 0; row < kMaskRows; ++row)
            {
                const u32 y = (row * kHeight) / kMaskRows;
                for (u32 col = 0; col < kMaskCols; ++col)
                {
                    const u32 x = (col * kWidth) / kMaskCols;
                    if (framebuffer.ReadPixel(1, static_cast<int>(x), static_cast<int>(y)) != entityId)
                    {
                        continue;
                    }
                    ++covered;

                    const sizet i = (static_cast<sizet>(y) * kWidth + x) * 4;
                    if (i + 2 >= rgba.size())
                    {
                        continue;
                    }
                    const int luma = (rgba[i + 0] + rgba[i + 1] + rgba[i + 2]) / 3;
                    if (luma > backgroundLuma + 12)
                    {
                        ++lit;
                    }
                }
            }

            SurfaceSample out;
            const auto total = static_cast<f32>(kMaskCols * kMaskRows);
            out.Coverage = static_cast<f32>(covered) / total;
            out.LitFraction = (covered == 0) ? 0.0f : static_cast<f32>(lit) / static_cast<f32>(covered);
            return out;
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

    class VoxelMarchingCubesVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            // The editor grid is an infinite plane that would sit behind the
            // blob and muddy both the background reference and the coverage.
            scene.SetGridVisible(false);

            {
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.35f, -0.85f, -0.4f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 3.5f;
            }
            {
                // Fill from below-and-behind so the blob's lower hemisphere is
                // lit too: under one top-down sun most of a sphere sits at or
                // past grazing incidence, and a legitimately dark surface would
                // then be indistinguishable from the bug this test guards.
                Entity fill = scene.CreateEntity("Fill");
                auto& dl = fill.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(0.45f, 0.8f, 0.4f));
                dl.m_Color = glm::vec3(0.8f, 0.85f, 1.0f);
                dl.m_Intensity = 3.2f;
                dl.m_CastShadows = false;
            }

            m_TerrainEntity = scene.CreateEntity("VoxelTerrain");
            {
                auto& terrain = m_TerrainEntity.AddComponent<TerrainComponent>();
                terrain.m_ProceduralEnabled = true;
                terrain.m_ProceduralSeed = 4242;
                terrain.m_ProceduralResolution = 128;
                terrain.m_WorldSizeX = kTerrainExtent;
                terrain.m_WorldSizeZ = kTerrainExtent;
                terrain.m_HeightScale = kHeightScale;
                terrain.m_TessellationEnabled = false;
                terrain.m_CollisionEnabled = false;
                terrain.m_Material = Ref<TerrainMaterial>::Create();

                terrain.m_VoxelEnabled = true;
                terrain.m_VoxelSize = kVoxelSize;
                terrain.m_VoxelMesher = VoxelMesherKind::MarchingCubes;
            }
        }

        // Frames the blob head-on. The terrain surface is ~56 units below and
        // outside this framing, so every covered sample is the voxel blob.
        [[nodiscard]] static EditorCamera BlobCamera()
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 2000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(glm::vec3(kBlobCentre.x, kBlobCentre.y, 40.0f), 0.0f, 0.0f);
            return camera;
        }

        void CaptureBlob(std::vector<u8>& outPixels, SurfaceSample& outSample)
        {
            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No SceneColor framebuffer";
            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<sizet>(kWidth) * kHeight * 4u);
            outSample = SampleSurface(*fb, outPixels,
                                      static_cast<i32>(std::to_underlying(static_cast<entt::entity>(m_TerrainEntity))));
        }

        Entity m_TerrainEntity;
    };

    TEST_F(VoxelMarchingCubesVisualEvidenceTest, CarvedBlobMeshesAndIsActuallyShaded)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        EditorCamera camera = BlobCamera();

        // Tick once so Scene creates the VoxelOverride, then add the blob and
        // let the marching-cubes rebuild pick it up.
        RunEditorFrames(camera, 1);

        auto& terrain = m_TerrainEntity.GetComponent<TerrainComponent>();
        ASSERT_TRUE(terrain.m_VoxelOverride) << "the voxel override was never created";
        terrain.m_VoxelOverride->AddSphere(kBlobCentre, kBlobRadius);

        RunEditorFrames(camera, 3);

        // (1) The marching-cubes path produced geometry at all.
        ASSERT_FALSE(terrain.m_VoxelMeshes.empty())
            << "AddSphere marked chunks dirty but marching cubes meshed nothing";
        u32 mcTriangles = 0;
        for (const auto& [coord, mesh] : terrain.m_VoxelMeshes)
        {
            mcTriangles += mesh.IndexCount / 3u;
        }
        EXPECT_GT(mcTriangles, 0u);

        // (2) Unbound terrain arrays — the configuration that used to render the
        //     whole volume black.
        {
            std::vector<u8> frame;
            SurfaceSample sample;
            CaptureBlob(frame, sample);
            if (GoldenRebaseRequested())
            {
                WritePng("VoxelMarchingCubes_blob_untextured.png", frame);
            }
            std::printf("[#727] marching-cubes blob (no layers): %u tris, coverage %.3f, lit %.3f\n",
                        mcTriangles, static_cast<double>(sample.Coverage), static_cast<double>(sample.LitFraction));

            EXPECT_GT(sample.Coverage, 0.03f) << "the blob did not render";
            EXPECT_GT(sample.LitFraction, 0.80f)
                << "the blob rendered but is BLACK (" << sample.LitFraction
                << " of its pixels are brighter than the background). Geometry is not the problem — this is the "
                   "unbound-normal-array decode: black * 2 - 1 is a unit-length (-1,-1,-1) that rotates every "
                   "normal away from the light.";
        }

        // (3) Bound terrain arrays. This is the only configuration that reaches
        //     the tangent-frame branch, where an exactly +/-Z normal used to make
        //     `normalize(cross(N, vec3(0,0,1)))` NaN — and a NaN cannot be caught
        //     by `length(T) < 0.001`, because every comparison against NaN is
        //     false. A sphere has such normals.
        {
            for (const auto& layer : TerrainGenerator::MakeDefaultLayers())
            {
                terrain.m_Material->AddLayer(layer);
            }
            terrain.m_MaterialNeedsRebuild = true;
            RunEditorFrames(camera, 3);

            std::vector<u8> frame;
            SurfaceSample sample;
            CaptureBlob(frame, sample);
            if (GoldenRebaseRequested())
            {
                WritePng("VoxelMarchingCubes_blob_textured.png", frame);
            }
            std::printf("[#727] marching-cubes blob (layers bound): coverage %.3f, lit %.3f\n",
                        static_cast<double>(sample.Coverage), static_cast<double>(sample.LitFraction));

            EXPECT_GT(sample.Coverage, 0.03f) << "the blob stopped rendering once terrain layers were bound";
            EXPECT_GT(sample.LitFraction, 0.80f)
                << "the blob went dark with the normal array bound (" << sample.LitFraction
                << " lit) — the tangent frame is producing NaN or an inverted normal";
        }
    }
} // namespace OloEngine::Tests
