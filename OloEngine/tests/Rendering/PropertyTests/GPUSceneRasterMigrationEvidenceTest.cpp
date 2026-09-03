// OLO_TEST_LAYER: integration
// =============================================================================
// GPUSceneRasterMigrationEvidenceTest.cpp
//
// Visual evidence for the raster migration (issue #994): the classic
// ordinary-mesh path rendered through the canonical GPU Scene records, captured
// from four camera angles into
//   OloEditor/assets/tests/visual/GPUSceneRaster_<pose>.png
//
// Why the PNGs alone would not be evidence
// ----------------------------------------
// The migration is expected to be pixel-identical: the record and the per-draw
// UBO are both built from the same Material, and the record's transforms are
// the same render-relative matrices the legacy path uploaded. A capture that
// looks right therefore proves nothing on its own — it would look exactly the
// same if the migration had silently fallen back to the legacy branch on every
// draw.
//
// So the test asserts the path is actually live, from the renderer's own
// telemetry: after a frame of this scene, Renderer3D must report resolved draw
// links and no unresolved ones. That plus an identical picture is the claim
// worth making — "renders through the GPU Scene, and nothing moved".
//
// The remaining failure the pictures DO catch is the one a CPU test cannot: a
// transposed or double-shifted transform. Those still render something
// plausible from one angle, which is why the captures orbit the scene instead
// of taking one shot.
//
// Classification: L8 / integration (full GL pipeline + RGBA8 readback + PNG).
// =============================================================================

#include "OloEnginePCH.h"
#include "../../TestOptions.h"

#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <array>
#include <cmath>
#include <limits>
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

        // Per-channel RMSE (0..255) over RGB between two equal-size RGBA8 buffers.
        [[nodiscard]] f64 Rgba8Rmse(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size() || a.empty())
            {
                return std::numeric_limits<f64>::max();
            }
            f64 sumSquares = 0.0;
            std::size_t count = 0;
            for (std::size_t i = 0; i + 3 < a.size(); i += 4)
            {
                for (int channel = 0; channel < 3; ++channel)
                {
                    const f64 delta = static_cast<f64>(a[i + channel]) - static_cast<f64>(b[i + channel]);
                    sumSquares += delta * delta;
                    ++count;
                }
            }
            return count ? std::sqrt(sumSquares / static_cast<f64>(count)) : 0.0;
        }

        // Generous, because this is a whole-pipeline capture rather than a tight
        // pixel test: it exists to catch a mesh that MOVED, not a shading epsilon.
        constexpr f64 kGoldenRmseThreshold = 6.0;

        // Mean luminance (0..1) of a square region centred on (cx, cy).
        [[nodiscard]] f32 MeanLuminance(const std::vector<u8>& rgba, u32 width, u32 height, u32 cx, u32 cy,
                                        u32 halfExtent)
        {
            const u32 x0 = cx > halfExtent ? cx - halfExtent : 0u;
            const u32 y0 = cy > halfExtent ? cy - halfExtent : 0u;
            const u32 x1 = std::min(cx + halfExtent, width);
            const u32 y1 = std::min(cy + halfExtent, height);
            f64 sum = 0.0;
            u64 count = 0;
            for (u32 y = y0; y < y1; ++y)
            {
                for (u32 x = x0; x < x1; ++x)
                {
                    const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4u;
                    sum += 0.2126 * rgba[i] + 0.7152 * rgba[i + 1] + 0.0722 * rgba[i + 2];
                    ++count;
                }
            }
            return count ? static_cast<f32>(sum / (count * 255.0)) : 0.0f;
        }
    } // namespace

    // A scene of ordinary MeshComponent meshes — the migrated path, and only it.
    // Deliberately no instanced field and no virtual geometry: those are named
    // legacy adapters (GPUSceneLegacyAdapters.h) and would put unresolved links
    // in the frame, blunting the "no draw fell back" assertion below.
    //
    // The colours are separated on purpose. A material record read from the
    // wrong slot shows up as one mesh wearing another's albedo, which a single
    // grey test scene would hide completely.
    class GPUSceneRasterMigrationScene : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();

            {
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.45f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 3.0f;
            }

            const auto addMesh = [&scene](const char* name, MeshPrimitive primitive, const glm::vec3& position,
                                          const glm::vec3& scale, const glm::vec4& albedo, f32 metallic,
                                          f32 roughness)
            {
                Entity entity = scene.CreateEntity(name);
                auto& transform = entity.GetComponent<TransformComponent>();
                transform.Translation = position;
                transform.Scale = scale;

                auto& mesh = entity.AddComponent<MeshComponent>();
                mesh.m_Primitive = primitive;
                Ref<Mesh> source = (primitive == MeshPrimitive::Plane) ? MeshPrimitives::CreatePlane()
                                                                       : MeshPrimitives::CreateCube();
                if (source)
                {
                    mesh.m_MeshSource = source->GetMeshSource();
                }

                // A MaterialComponent override, so the material record is an
                // EntityOverride keyed on the stable entity id — the lane a
                // wrong slot would cross between two of these meshes.
                auto& material = entity.AddComponent<MaterialComponent>();
                material.m_Material.SetBaseColorFactor(albedo);
                material.m_Material.SetMetallicFactor(metallic);
                material.m_Material.SetRoughnessFactor(roughness);
            };

            // Ground, so the meshes have somewhere to sit and cast onto.
            addMesh("Ground", MeshPrimitive::Plane, { 0.0f, -1.0f, 0.0f }, { 24.0f, 1.0f, 24.0f },
                    { 0.45f, 0.45f, 0.48f, 1.0f }, 0.0f, 0.9f);
            // Three strongly separated albedos at known world positions. Their
            // left-to-right order in the frame is what the pose assertions read.
            addMesh("RedCube", MeshPrimitive::Cube, { -4.0f, 0.0f, 0.0f }, { 1.6f, 1.6f, 1.6f },
                    { 0.85f, 0.06f, 0.06f, 1.0f }, 0.0f, 0.45f);
            addMesh("GreenCube", MeshPrimitive::Cube, { 0.0f, 0.0f, 0.0f }, { 1.6f, 1.6f, 1.6f },
                    { 0.06f, 0.80f, 0.10f, 1.0f }, 0.0f, 0.35f);
            addMesh("BlueCube", MeshPrimitive::Cube, { 4.0f, 0.0f, 0.0f }, { 1.6f, 1.6f, 1.6f },
                    { 0.08f, 0.12f, 0.90f, 1.0f }, 0.0f, 0.25f);
            // A tall pillar off to one side: an obvious silhouette, so a
            // transposed transform (which usually shears or flattens) shows up
            // as a shape change rather than a subtle shading change.
            addMesh("Pillar", MeshPrimitive::Cube, { 0.0f, 2.0f, -6.0f }, { 0.7f, 5.0f, 0.7f },
                    { 0.9f, 0.85f, 0.4f, 1.0f }, 0.2f, 0.5f);

            EnableRendering(kWidth, kHeight);
        }

        void Capture(const std::string& poseName, const glm::vec3& position, f32 yaw, f32 pitch,
                     std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 500.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, yaw, pitch);

            // Two ticks: the first fills the GPU Scene with fresh records (every
            // instance starts static), the second is the steady-state frame the
            // capture wants — and the one where a previous-transform mistake
            // would smear velocity.
            RunEditorFrames(camera, 2);

            u32 width = 0;
            u32 height = 0;
            ASSERT_TRUE(ReadbackComposite(outPixels, width, height)) << "no composite framebuffer for '" << poseName << "'";
            ASSERT_EQ(width, kWidth);
            ASSERT_EQ(height, kHeight);

            // Golden model, as WaterVisualEvidenceTest uses it: a normal run
            // COMPARES against the committed PNG and writes nothing, so a passing
            // run leaves the tracked file untouched; --olo-golden-rebase rewrites
            // it after a deliberate visual change. The comparison is the actual
            // no-regression check — the migration is supposed to be
            // pixel-identical, so any real divergence lands here.
            const fs::path directory = fs::path("assets") / "tests" / "visual";
            const std::string path = (directory / ("GPUSceneRaster_" + poseName + ".png")).string();
            if (Options().GoldenRebase)
            {
                std::error_code ec;
                fs::create_directories(directory, ec);
                ASSERT_FALSE(ec) << "failed to create '" << directory.string() << "': " << ec.message();
                const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight),
                                                   4, outPixels.data(), static_cast<int>(kWidth) * 4);
                ASSERT_NE(wrote, 0) << "failed to write golden '" << path << "'";
                return;
            }

            int goldenWidth = 0;
            int goldenHeight = 0;
            int goldenChannels = 0;
            stbi_uc* golden = ::stbi_load(path.c_str(), &goldenWidth, &goldenHeight, &goldenChannels, 4);
            ASSERT_NE(golden, nullptr) << "missing golden '" << path << "' — rerun with --olo-golden-rebase to create it";
            const bool sizeMatches =
                goldenWidth == static_cast<int>(kWidth) && goldenHeight == static_cast<int>(kHeight);
            std::vector<u8> goldenPixels;
            if (sizeMatches)
            {
                goldenPixels.assign(golden, golden + static_cast<std::size_t>(kWidth) * kHeight * 4u);
            }
            ::stbi_image_free(golden);
            ASSERT_TRUE(sizeMatches) << "golden '" << path << "' is " << goldenWidth << "x" << goldenHeight;

            const f64 rmse = Rgba8Rmse(outPixels, goldenPixels);
            EXPECT_LE(rmse, kGoldenRmseThreshold)
                << "pose '" << poseName << "' diverged from its golden (RMSE " << rmse << "). If this is an intended "
                << "visual change, rerun with --olo-golden-rebase to update " << path;
        }
    };

    TEST_F(GPUSceneRasterMigrationScene, ClassicMeshPathRendersThroughTheRecordsFromEveryAngle)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // The migrated raster path is the classic mesh path on Deferred, where
        // PBR_GBuffer.glsl reads the canonical material record.
        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        struct Pose
        {
            const char* Name;
            glm::vec3 Position;
            f32 Yaw;
            f32 Pitch;
        };

        // Four angles around the same geometry. A transform that is transposed
        // or shifted by the render origin twice survives one of these and not
        // four: the meshes move relative to each other, or leave the frame.
        // EditorCamera::GetOrientation builds quat(-pitch, -yaw, 0), so the
        // forward vector is (sin(yaw)cos(pitch), -sin(pitch), -cos(yaw)cos(pitch)):
        // POSITIVE yaw swings the view toward +X and positive pitch tilts it
        // down. The two side poses therefore take yaw = atan2(-eye.x, eye.z) to
        // look back at the origin — the sign that makes them see the scene at
        // all, which the first run of this test got backwards.
        const std::array<Pose, 4> poses = { {
            { "Front", { 0.0f, 2.5f, 14.0f }, 0.0f, 0.14f },
            { "Left", { -12.0f, 3.0f, 9.0f }, 0.93f, 0.20f },
            { "Right", { 12.0f, 3.0f, 9.0f }, -0.93f, 0.20f },
            { "Above", { 0.0f, 12.0f, 8.0f }, 0.0f, 0.90f },
        } };

        std::vector<u8> frontPixels;
        for (const Pose& pose : poses)
        {
            std::vector<u8> pixels;
            Capture(pose.Name, pose.Position, pose.Yaw, pose.Pitch, pixels);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }

            const f32 frame = MeanLuminance(pixels, kWidth, kHeight, kWidth / 2u, kHeight / 2u, kHeight / 3u);
            EXPECT_GT(frame, 0.02f) << "pose '" << pose.Name << "' rendered (near-)black";

            // The claim the pictures cannot make: draws in this frame actually
            // READ the records. Without it a total fallback — every draw on the
            // legacy branch — would produce byte-identical captures and a green
            // test. It has to be the dispatcher's count, not the extraction
            // one: extraction resolving a link says nothing about whether
            // anything consumed it, so asserting on that would stay green if
            // the link were never passed to DrawMesh at all.
            EXPECT_GT(CommandDispatch::GetGPUSceneConsumedDrawCount(), 0u)
                << "pose '" << pose.Name
                << "': no draw consumed a canonical record, so the classic mesh path did not render through the "
                   "GPU Scene at all";
            EXPECT_EQ(CommandDispatch::GetGPUSceneFallbackDrawCount(), 0u)
                << "pose '" << pose.Name
                << "': a classic-mesh draw carried a link and fell back to its own copy. Every mesh in this scene "
                   "is an ordinary MeshComponent, so every one of them should resolve and be consumed.";
            // And extraction agrees: every link staged this frame resolved.
            EXPECT_GT(Renderer3D::GetGPUSceneLinkedDrawCount(), 0u) << "pose '" << pose.Name << "'";
            EXPECT_EQ(Renderer3D::GetGPUSceneUnlinkedDrawCount(), 0u) << "pose '" << pose.Name << "'";

            if (std::string(pose.Name) == "Front")
            {
                frontPixels = pixels;
            }
        }

        ASSERT_FALSE(frontPixels.empty());

        // Material identity: each cube must wear its own albedo. A material
        // record read from the wrong slot — the failure a generation check
        // exists to prevent — swaps two of these while leaving the frame
        // perfectly plausible.
        const auto meanChannel = [&](u32 cx, u32 cy, int channel)
        {
            f64 sum = 0.0;
            u64 count = 0;
            for (u32 y = cy - 12u; y < cy + 12u; ++y)
            {
                for (u32 x = cx - 12u; x < cx + 12u; ++x)
                {
                    sum += frontPixels[(static_cast<std::size_t>(y) * kWidth + x) * 4u + channel];
                    ++count;
                }
            }
            return static_cast<f32>(sum / (count * 255.0));
        };

        // The three cubes sit at world x = -4, 0, +4 in front of a camera on
        // +Z looking down -Z. These pixel centres are read off the committed
        // GPUSceneRaster_Front.png; a 12-pixel half-extent stays inside each
        // cube's face, clear of the silhouette and of the editor grid lines
        // that cross the upper part of it.
        constexpr u32 kRowY = 245u;
        const std::array<u32, 3> columns{ 345u, 480u, 613u };
        const std::array<const char*, 3> names{ "RedCube", "GreenCube", "BlueCube" };
        for (int cube = 0; cube < 3; ++cube)
        {
            const f32 red = meanChannel(columns[static_cast<std::size_t>(cube)], kRowY, 0);
            const f32 green = meanChannel(columns[static_cast<std::size_t>(cube)], kRowY, 1);
            const f32 blue = meanChannel(columns[static_cast<std::size_t>(cube)], kRowY, 2);
            const f32 dominant = (cube == 0) ? red : (cube == 1) ? green
                                                                 : blue;
            const f32 other0 = (cube == 0) ? green : (cube == 1) ? red
                                                                 : red;
            const f32 other1 = (cube == 0) ? blue : (cube == 1) ? blue
                                                                : green;
            EXPECT_GT(dominant, other0)
                << names[static_cast<std::size_t>(cube)]
                << " is not wearing its own albedo — a material record resolved to the wrong slot";
            EXPECT_GT(dominant, other1) << names[static_cast<std::size_t>(cube)] << " albedo channel is not dominant";
        }
    }
} // namespace OloEngine::Tests
