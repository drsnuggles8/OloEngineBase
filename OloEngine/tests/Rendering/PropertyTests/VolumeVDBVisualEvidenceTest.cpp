// OLO_TEST_LAYER: L8
// =============================================================================
// VolumeVDBVisualEvidenceTest.cpp
//
// Visual evidence (PNG) for the OpenVDB volumetric import render path (#724):
// FogVolumeComponent::m_Shape == Texture3D sampling a VolumeAsset's Texture3D
// through VolumetricFogPass / FroxelFogScatter.comp's evaluateFogVolumesAtPointVDB.
//
// A hand-built dense density grid (a soft sphere, so the shape reads
// unambiguously from any angle) is written through the REAL .olovol codec
// (VolumeSerializer::SerializeToFile / TryLoadData — the same path
// OloEngine-VolumeCook's OpenVDB importer feeds), registered as a memory-only
// VolumeAsset, and referenced by a FogVolumeComponent on an entity placed in
// front of a lit floor. The scene is rendered from three angles — side,
// three-quarter, and top-down (per the task's verification requirement) —
// once with the volume OFF and once ON, and every frame is written to
//   OloEditor/assets/tests/visual/VolumeVDB_<angle>_<state>.png
//
// The contract is GOLDEN-FREE and differential (robust across GPUs, no
// committed reference image), mirroring FogVisualEvidenceTest:
//   1. Every frame renders non-black.
//   2. The region the volume occupies on-screen changes measurably between
//      OFF and ON, from EVERY camera angle — proving the imported density
//      grid actually reaches the frame (not just "the pass ran").
//   3. A region clearly outside the volume's screen footprint stays
//      approximately unchanged — the volume does not flood the whole frame
//      (the same class of bug CLAUDE.md warns fog passes are prone to).
//
// The grid -> texture transform math (translation/voxel-size preservation,
// OpenVDB import, downsampling) is pinned by VolumeImportTest.cpp; the
// .olovol binary codec's round-trip is pinned there too. This test is the
// "does it look right on screen" half CLAUDE.md's rendering rule requires.
//
// Runs in the normal suite and SKIPs (not fails) when no GL 4.6 context
// exists, matching WaterVisualEvidenceTest / FogVisualEvidenceTest.
//
// Classification: L8 (full GL pipeline + RGBA8 readback + PNG evidence).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Asset/VolumeAsset.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 1024;
        constexpr u32 kHeight = 768;
        constexpr f32 kCaptureTime = 4.0f;
        constexpr u32 kGridSize = 16;

        struct BandStats
        {
            f64 R = 0.0;
            f64 G = 0.0;
            f64 B = 0.0;
        };

        [[nodiscard]] BandStats SampleBand(const std::vector<u8>& px, f32 x0, f32 x1, f32 y0, f32 y1)
        {
            const u32 ix0 = static_cast<u32>(x0 * kWidth);
            const u32 ix1 = static_cast<u32>(x1 * kWidth);
            const u32 iy0 = static_cast<u32>(y0 * kHeight);
            const u32 iy1 = static_cast<u32>(y1 * kHeight);
            u64 sumR = 0, sumG = 0, sumB = 0, count = 0;
            for (u32 y = iy0; y < iy1; ++y)
            {
                for (u32 x = ix0; x < ix1; ++x)
                {
                    const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                    if (idx + 2 >= px.size())
                        continue;
                    sumR += px[idx + 0];
                    sumG += px[idx + 1];
                    sumB += px[idx + 2];
                    ++count;
                }
            }
            if (count == 0)
                return {};
            return { static_cast<f64>(sumR) / count, static_cast<f64>(sumG) / count,
                     static_cast<f64>(sumB) / count };
        }

        // Dense soft-sphere density grid — deliberately NOT round-tripped
        // through OpenVDB here (that path is VolumeImportTest.cpp's job);
        // this exercises the SAME .olovol codec + VolumeAsset + Texture3D
        // upload an OpenVDB-cooked file would produce.
        std::vector<f32> BuildSphereDensity()
        {
            std::vector<f32> density(static_cast<sizet>(kGridSize) * kGridSize * kGridSize);
            const f32 center = (kGridSize - 1) * 0.5f;
            const f32 radius = kGridSize * 0.5f;
            for (u32 z = 0; z < kGridSize; ++z)
            {
                for (u32 y = 0; y < kGridSize; ++y)
                {
                    for (u32 x = 0; x < kGridSize; ++x)
                    {
                        const f32 dx = static_cast<f32>(x) - center;
                        const f32 dy = static_cast<f32>(y) - center;
                        const f32 dz = static_cast<f32>(z) - center;
                        const f32 dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                        const f32 d = std::max(0.0f, 1.0f - dist / radius);
                        density[(static_cast<sizet>(z) * kGridSize + y) * kGridSize + x] = d * d; // smoother core
                    }
                }
            }
            return density;
        }
    } // namespace

    class VolumeVDBVisualEvidenceTest : public RendererAttachedTest
    {
      public:
        AssetHandle m_VolumeHandle = 0;
        Entity m_VolumeEntity;

        void BuildScene() override
        {
            // AssetManager::AddMemoryOnlyAsset / VolumeSerializer::TryLoadData
            // need an active project + asset manager — mount a throwaway temp
            // project, mirroring LightmapVisualEvidenceTest /
            // VirtualGeometryVisualEvidenceTest.
            if (!Project::GetActive() || !Project::HasAssetManager())
            {
                std::error_code ec;
                fs::path const projectDir = TempDir("project");
                fs::create_directories(projectDir / "Assets", ec);
                ASSERT_FALSE(ec) << "failed to create temp project dir";
                {
                    std::ofstream proj(projectDir / "Evidence.oloproj");
                    proj << "Project:\n"
                            "  Name: VolumeVDBEvidence\n"
                            "  StartScene: \"\"\n"
                            "  AssetDirectory: \"Assets\"\n"
                            "  ScriptModulePath: \"\"\n";
                }
                ASSERT_TRUE(Project::Load(projectDir / "Evidence.oloproj"));
                auto assetManager = Ref<EditorAssetManager>::Create();
                assetManager->Initialize(false); // no file watcher in tests
                Project::SetAssetManager(assetManager);
            }

            EnableRendering(kWidth, kHeight);
            Scene& scene = GetScene();

            // ── Cook a real .olovol through the same codec OloEngine-
            // VolumeCook writes, then load it through the real serializer —
            // exercises the actual runtime load path, not a shortcut. ──
            const fs::path olovolRelPath = fs::path("Assets") / "SphereSmoke.olovol";
            const fs::path olovolAbsPath = Project::GetProjectDirectory() / olovolRelPath;
            const std::vector<f32> density = BuildSphereDensity();
            ASSERT_TRUE(VolumeSerializer::SerializeToFile(olovolAbsPath, glm::uvec3(kGridSize), glm::vec3(1.0f),
                                                          glm::mat4(1.0f), 0.0f, density))
                << "failed to write synthetic .olovol fixture";

            AssetMetadata metadata;
            metadata.Handle = AssetHandle{};
            metadata.Type = AssetType::Volume;
            metadata.FilePath = olovolRelPath;

            Ref<Asset> loadedAsset;
            ASSERT_TRUE(VolumeSerializer().TryLoadData(metadata, loadedAsset)) << "VolumeSerializer failed to load the fixture";
            auto volumeAsset = loadedAsset.As<VolumeAsset>();
            ASSERT_TRUE(volumeAsset);
            ASSERT_TRUE(volumeAsset->IsLoaded()) << "VolumeAsset has no GPU texture after load";

            m_VolumeHandle = AssetManager::AddMemoryOnlyAsset<VolumeAsset>(volumeAsset);
            ASSERT_NE(static_cast<u64>(m_VolumeHandle), 0u) << "AddMemoryOnlyAsset returned a null handle";

            // Sun so the floor is visibly lit.
            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = glm::vec3{ 0.0f, 30.0f, 0.0f };
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.3f, -0.8f, -0.5f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 3.0f;
            }

            // Warm floor so the smoke (grey/white) reads distinctly against it
            // from every angle, including straight down.
            {
                Entity floor = scene.CreateEntity("Floor");
                auto& tc = floor.GetComponent<TransformComponent>();
                tc.Translation = glm::vec3{ 0.0f, -2.0f, 0.0f };
                tc.Scale = glm::vec3{ 40.0f, 1.0f, 40.0f };
                auto& mc = floor.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Plane;
                if (Ref<Mesh> mesh = MeshPrimitives::CreatePlane())
                    mc.m_MeshSource = mesh->GetMeshSource();
                auto& mat = floor.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.7f, 0.35f, 0.2f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(0.9f);
            }

            // The volume entity — created disabled; each test enables it for
            // the ON capture and leaves it disabled for OFF (see
            // ScopedVolumeToggle below), so both frames come from the exact
            // same scene graph.
            m_VolumeEntity = scene.CreateEntity("VDBSmoke");
            auto& tc = m_VolumeEntity.GetComponent<TransformComponent>();
            tc.Translation = glm::vec3{ 0.0f, 2.0f, 0.0f };
            auto& fv = m_VolumeEntity.AddComponent<FogVolumeComponent>();
            fv.m_Shape = FogVolumeShape::Texture3D;
            fv.m_Extents = glm::vec3{ 4.0f, 4.0f, 4.0f };
            fv.m_Color = glm::vec3{ 0.9f, 0.9f, 0.95f };
            fv.m_Density = 40.0f; // froxel fog integrates density over distance; needs to be strong to read clearly at 16^3 grid resolution
            fv.m_BlendWeight = 1.0f;
            fv.m_Priority = 0;
            fv.m_AffectTransparent = false;
            fv.m_DensityVolume = m_VolumeHandle;
            fv.m_Enabled = false; // OFF by default; RAII-toggled per capture
        }

        void Capture(const std::string& tag, const glm::vec3& position, f32 yaw, f32 pitch, std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, yaw, pitch);

            // VolumetricFogPass temporally reprojects/accumulates its scatter
            // volume (m_ScatterVolume ping-pong); 2 frames (enough for other
            // temporal effects in this suite) was not enough to flush a
            // previous angle's density out of history when toggling the
            // FogVolumeComponent off — the OFF capture still showed a faint
            // ghost of the last ON frame. More frames per capture lets it converge.
            RunEditorFrames(camera, 8);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for volume capture '" << tag << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

            const std::size_t rowBytes = static_cast<std::size_t>(kWidth) * 4u;
            std::vector<u8> tmp(rowBytes);
            for (u32 y = 0; y < kHeight / 2u; ++y)
            {
                u8* top = outPixels.data() + static_cast<std::size_t>(y) * rowBytes;
                u8* bot = outPixels.data() + static_cast<std::size_t>(kHeight - 1u - y) * rowBytes;
                std::memcpy(tmp.data(), top, rowBytes);
                std::memcpy(top, bot, rowBytes);
                std::memcpy(bot, tmp.data(), rowBytes);
            }

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ASSERT_FALSE(ec) << "Failed to create evidence dir '" << dir.generic_string() << "': " << ec.message();

            const std::string path = (dir / ("VolumeVDB_" + tag + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                                               outPixels.data(), static_cast<int>(kWidth) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";

            int w = 0, h = 0, ch = 0;
            stbi_uc* loaded = ::stbi_load(path.c_str(), &w, &h, &ch, 4);
            ASSERT_NE(loaded, nullptr) << "Failed to reload written PNG '" << path << "'";
            EXPECT_EQ(w, static_cast<int>(kWidth));
            EXPECT_EQ(h, static_cast<int>(kHeight));
            ::stbi_image_free(loaded);
        }
    };

    namespace
    {
        struct ScopedVolumeToggle
        {
            FogVolumeComponent& Fv;
            explicit ScopedVolumeToggle(FogVolumeComponent& fv) : Fv(fv)
            {
                Fv.m_Enabled = true;
            }
            ~ScopedVolumeToggle()
            {
                Fv.m_Enabled = false;
            }
        };

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
        };

        struct ScopedFogSettings
        {
            FogSettings Saved;
            ScopedFogSettings() : Saved(Renderer3D::GetFogSettings()) {}
            ~ScopedFogSettings()
            {
                Renderer3D::GetFogSettings() = Saved;
            }
        };
    } // namespace

    // One angle: OFF vs ON, asserting the volume's screen footprint changes
    // while a clearly-outside region stays roughly stable.
    struct AngleCase
    {
        const char* Tag;
        glm::vec3 Position;
        f32 Yaw;
        f32 Pitch;
        // Volume footprint band (UV fractions) for THIS pose.
        f32 VolX0, VolX1, VolY0, VolY1;
        // A corner clearly outside the volume, for the "didn't flood the frame" guard.
        f32 OutX0, OutX1, OutY0, OutY1;
    };

    void RunAngle(VolumeVDBVisualEvidenceTest& test, Entity volumeEntity, const AngleCase& angle)
    {
        std::vector<u8> offPixels, onPixels;

        test.Capture(std::string(angle.Tag) + "_Off", angle.Position, angle.Yaw, angle.Pitch, offPixels);
        if (::testing::Test::HasFatalFailure())
            return;

        {
            ScopedVolumeToggle toggle(volumeEntity.GetComponent<FogVolumeComponent>());
            test.Capture(std::string(angle.Tag) + "_On", angle.Position, angle.Yaw, angle.Pitch, onPixels);
        }
        if (::testing::Test::HasFatalFailure())
            return;

        const BandStats volOff = SampleBand(offPixels, angle.VolX0, angle.VolX1, angle.VolY0, angle.VolY1);
        const BandStats volOn = SampleBand(onPixels, angle.VolX0, angle.VolX1, angle.VolY0, angle.VolY1);
        const BandStats outOff = SampleBand(offPixels, angle.OutX0, angle.OutX1, angle.OutY0, angle.OutY1);
        const BandStats outOn = SampleBand(onPixels, angle.OutX0, angle.OutX1, angle.OutY0, angle.OutY1);

        EXPECT_GT(volOff.R + volOff.G + volOff.B, 5.0)
            << "[" << angle.Tag << "] OFF frame rendered (near-)black. See VolumeVDB_" << angle.Tag << "_Off.png";
        EXPECT_GT(volOn.R + volOn.G + volOn.B, 5.0)
            << "[" << angle.Tag << "] ON frame rendered (near-)black. See VolumeVDB_" << angle.Tag << "_On.png";

        const f64 volDelta = std::abs(volOn.R - volOff.R) + std::abs(volOn.G - volOff.G) + std::abs(volOn.B - volOff.B);
        EXPECT_GT(volDelta, 8.0) << "[" << angle.Tag << "] the volume's screen footprint did not change when enabled "
                                                        "(off=("
                                 << volOff.R << "," << volOff.G << "," << volOff.B << ") on=(" << volOn.R << ","
                                 << volOn.G << "," << volOn.B << ")) — the imported density grid is not reaching the "
                                                                 "frame. See VolumeVDB_"
                                 << angle.Tag << "_On.png";

        const f64 outDelta = std::abs(outOn.R - outOff.R) + std::abs(outOn.G - outOff.G) + std::abs(outOn.B - outOff.B);
        EXPECT_LT(outDelta, 20.0) << "[" << angle.Tag
                                  << "] a region clearly outside the volume changed too much when it was enabled "
                                     "(off=("
                                  << outOff.R << "," << outOff.G << "," << outOff.B << ") on=(" << outOn.R << ","
                                  << outOn.G << "," << outOn.B
                                  << ")) — the volume is flooding the whole frame instead of staying bounded. See "
                                     "VolumeVDB_"
                                  << angle.Tag << "_On.png";
    }

    TEST_F(VolumeVDBVisualEvidenceTest, ImportedDensityGridRendersFromMultipleAngles)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        ScopedMockTime scopedMockTime(kCaptureTime);
        ScopedFogSettings scopedFog;
        {
            auto& fog = Renderer3D::GetFogSettings();
            fog.Enabled = true;
            fog.EnableVolumetric = true; // VolumetricFogPass::m_Enabled gates on Enabled && EnableVolumetric
        }

        // Side view: camera at volume height, looking across it — the
        // sphere should sit mid-frame with clear sky/floor above/below it.
        const AngleCase side{ "Side", glm::vec3{ 12.0f, 2.0f, 0.0f }, -90.0f * (3.14159265f / 180.0f), 0.0f,
                              0.35f, 0.65f, 0.35f, 0.65f, 0.02f, 0.18f, 0.02f, 0.18f };

        // Three-quarter view: camera at (9,5,9) looking toward the volume at
        // (0,2,0) — forward = normalize((0,2,0) - (9,5,9)) = (-0.707,-0.235,-0.707)
        // horizontally, i.e. yaw = -45 deg (matches the -90 deg yaw the "Side"
        // case uses for a due -X look from (12,2,0); see EditorCamera's
        // yaw convention: forward = (sin(yaw), *, -cos(yaw))).
        const AngleCase threeQuarter{ "ThreeQuarter", glm::vec3{ 9.0f, 5.0f, 9.0f },
                                      -45.0f * (3.14159265f / 180.0f), 0.23f, 0.35f, 0.65f, 0.35f, 0.65f, 0.02f,
                                      0.18f, 0.02f, 0.18f };

        // Top-down: looking straight along -Y at the volume from above.
        const AngleCase topDown{ "TopDown", glm::vec3{ 0.0f, 16.0f, 0.01f }, 0.0f, 1.55f, 0.35f, 0.65f, 0.35f,
                                 0.65f, 0.02f, 0.18f, 0.02f, 0.18f };

        RunAngle(*this, m_VolumeEntity, side);
        if (::testing::Test::HasFatalFailure())
            return;
        RunAngle(*this, m_VolumeEntity, threeQuarter);
        if (::testing::Test::HasFatalFailure())
            return;
        RunAngle(*this, m_VolumeEntity, topDown);
    }
} // namespace OloEngine::Tests
