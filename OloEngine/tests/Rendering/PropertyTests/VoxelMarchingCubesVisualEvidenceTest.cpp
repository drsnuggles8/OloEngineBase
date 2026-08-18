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
// problem. It is "is what drew actually SHADED": the brightest point on the
// surface must clear the background, and the surface must show a luminance
// GRADIENT. A black render passes every coverage check ever written and fails
// both of these.
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

#include <algorithm>
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
            f32 Coverage = 0.0f; // fraction of the frame that is the terrain entity
            int Background = 0;  // luma of the empty frame
            int PeakLuma = 0;    // brightest sample ON the surface
            int Spread = 0;      // p90 - p10 across the surface
        };

        // Coverage comes from the entity-ID attachment (shading-independent, so
        // it answers "is this pixel the subject" and nothing else); brightness
        // comes from the colour attachment at the same grid points.
        //
        // The shading question is answered by PEAK and SPREAD rather than by a
        // "fraction of pixels brighter than the background", which was the first
        // thing tried here and measured the wrong property: this blob falls back
        // to a stone albedo of (0.35, 0.32, 0.28), so a perfectly correct render
        // is simply DIM and most of its pixels sit near the background. Measured
        // on a known-good frame: peak 83 against a background of 25, and
        // p90 - p10 = 42.
        //
        // What the bug actually destroyed was the shading GRADIENT - every
        // normal rotated the same way, so the surface went uniformly dark. That
        // collapses both of these numbers (peak toward background, spread toward
        // zero) while a dim-but-correct render keeps them.
        [[nodiscard]] SurfaceSample SampleSurface(Framebuffer& framebuffer, const std::vector<u8>& rgba, i32 entityId)
        {
            // GL readback is bottom-up; the top-left pixel is the last row's first.
            const sizet cornerIndex = (static_cast<sizet>(kHeight - 1) * kWidth) * 4;

            SurfaceSample out;
            out.Background =
                rgba.empty() ? 0 : (rgba[cornerIndex + 0] + rgba[cornerIndex + 1] + rgba[cornerIndex + 2]) / 3;

            std::vector<int> surfaceLuma;
            surfaceLuma.reserve(static_cast<sizet>(kMaskCols) * kMaskRows);

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

                    const sizet i = (static_cast<sizet>(y) * kWidth + x) * 4;
                    if (i + 2 < rgba.size())
                    {
                        surfaceLuma.push_back((rgba[i + 0] + rgba[i + 1] + rgba[i + 2]) / 3);
                    }
                }
            }

            out.Coverage = static_cast<f32>(surfaceLuma.size()) / static_cast<f32>(kMaskCols * kMaskRows);
            if (surfaceLuma.empty())
            {
                return out;
            }

            std::ranges::sort(surfaceLuma);
            const auto at = [&surfaceLuma](f32 q)
            {
                const auto idx = static_cast<sizet>(q * static_cast<f32>(surfaceLuma.size() - 1));
                return surfaceLuma[idx];
            };
            out.PeakLuma = surfaceLuma.back();
            out.Spread = at(0.90f) - at(0.10f);
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

        // Looks UP at the blob from below and in front of it, against open sky.
        //
        // Two conventions matter here and both are easy to get backwards
        // (EditorCamera::GetOrientation): yaw 0 looks along -Z, and POSITIVE
        // pitch tilts DOWN. So the camera sits at greater z than the blob and
        // tilts up with a negative pitch.
        //
        // The upward tilt is not cosmetic: at 60 degrees FOV the lower edge of
        // the frustum then points slightly ABOVE horizontal, so the heightmap
        // surface - which shares this entity ID and would otherwise count as
        // covered - cannot enter the frame at any distance. Every covered
        // sample is the voxel blob.
        [[nodiscard]] static EditorCamera BlobCamera()
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 2000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(glm::vec3(kBlobCentre.x, 60.0f, 150.0f), 0.0f, -0.60f);
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
            std::printf("[#727] marching-cubes blob (no layers): %u tris, coverage %.3f, bg %d, peak %d, spread %d\n",
                        mcTriangles, static_cast<double>(sample.Coverage), sample.Background, sample.PeakLuma,
                        sample.Spread);

            EXPECT_GT(sample.Coverage, 0.03f) << "the blob did not render";
            EXPECT_GT(sample.PeakLuma, sample.Background + 30)
                << "the blob rendered but nothing on it is lit (peak " << sample.PeakLuma << " vs background "
                << sample.Background << "). Geometry is not the problem - this is the unbound-normal-array decode: "
                                        "black * 2 - 1 is a unit-length (-1,-1,-1) that rotates every normal away from the light.";
            EXPECT_GT(sample.Spread, 20)
                << "the blob has no shading gradient (p90-p10 = " << sample.Spread
                << "): its shading is not varying across the surface, which is what a uniformly-rotated normal "
                   "looks like.";
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
            std::printf("[#727] marching-cubes blob (layers bound): coverage %.3f, bg %d, peak %d, spread %d\n",
                        static_cast<double>(sample.Coverage), sample.Background, sample.PeakLuma, sample.Spread);

            EXPECT_GT(sample.Coverage, 0.03f) << "the blob stopped rendering once terrain layers were bound";
            EXPECT_GT(sample.PeakLuma, sample.Background + 30)
                << "the blob went dark with the normal array bound (peak " << sample.PeakLuma << " vs background "
                << sample.Background << ") - the tangent frame is producing NaN or an inverted normal";
            EXPECT_GT(sample.Spread, 20)
                << "the blob lost its shading gradient with the normal array bound (p90-p10 = " << sample.Spread
                << ") - the tangent frame is collapsing the perturbed normals";
        }
    }
} // namespace OloEngine::Tests
