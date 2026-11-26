// =============================================================================
// WaterVisualEvidenceTest.cpp
//
// Visual evidence (PNG) for the water surface + underwater rendering
// (water-ocean.md §7.2). Renders a water scene (ocean + seafloor
// + a submerged cube + a pillar breaking the surface + skybox) through the FULL
// Renderer3D pipeline from several camera poses and writes each frame to
//   OloEditor/assets/tests/visual/Water_<pose>.png
//
// The point is the exact set of camera angles where the early water work
// glitched — viewed from the side, straddling the waterline, fully submerged,
// and looking down from above. Capturing PNGs lets a human (or the agent that
// wrote this) eyeball every angle without round-tripping through the editor.
//
// Beyond the PNGs, two driver-independent contracts are asserted on the
// "above, looking across the open ocean" pose, which is where the surface used
// to go see-through:
//   1. The open-ocean band is not the editor clear colour (the surface is
//      opaque — you can't see the background through it).
//   2. That band reads as water (blue/teal dominant), not a grey wash.
//
// Runs in the normal suite (issue #258)
// -------------------------------------
//   This test drives the full editor render path (OnUpdateEditor →
//   RenderScene3D → EndScene) by enabling rendering on the fixture's Scene.
//   It used to be DISABLED_: a full-pipeline render left Renderer3D global GL
//   state in a way that perturbed later GPU tests in the same process. PR #263
//   untangled that for the runtime-camera path (RendererAttachedTest::RunFrames
//   wraps each tick in a GLStateGuard); this test now goes through the matching
//   editor-camera helper RendererAttachedTest::RunEditorFrames, which guards the
//   OnUpdateEditor render the same way — so it runs alongside everything else
//   without poisoning it, and SKIPs (not fails) when no GL 4.6 context exists.
//
//   The committed Water_<pose>.png files act as golden references: a normal run
//   COMPARES (RMSE) and writes nothing; pass --olo-golden-rebase to
//   (re)write them after a deliberate visual change. Run from OloEditor/ so
//   assets resolve and any rebased PNGs land under
//   OloEditor/assets/tests/visual/. The cheaper water *math* contracts live in
//   WaterRenderingTest.cpp (fog segment math, the per-fragment waterline side
//   rule, UBO layout).
//
// Classification: L8 / integration (full GL pipeline + RGBA8 readback + PNG).
// =============================================================================

#include "OloEnginePCH.h"
#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "AttachmentFeedbackEvidence.h"
#include "RenderPropertyTest.h"
#include "TestRenderBackend.h"

#include "OloEngine/Physics3D/BoatWakeSystem.h"
#include "OloEngine/Physics3D/WaterProbe.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Ocean/OceanFFTField.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Water/WaterShoreDepthSystem.h"
#include "OloEngine/Renderer/Water/WaterWake.h"
#include "OloEngine/Renderer/Water/WaterWakeSystem.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Task/NamedThreads.h"
#include "OloEngine/Task/Scheduler.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <glad/gl.h>
#include <glm/gtc/quaternion.hpp>
#include <gtest/gtest.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <span>
#include <iterator>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;

        // Frozen wall-clock value for the captures. The water wave phase and
        // normal-map scroll come from Time::GetTime() (see Scene.cpp), so pinning
        // it makes every capture render an identical, deterministic frame — the
        // prerequisite for treating the PNGs as golden references.
        constexpr f32 kCaptureTime = 12.0f;

        // Generous per-channel RMSE pass threshold (0..255). Determinism makes
        // same-machine re-runs ~0; the slack absorbs cross-GPU float variance in
        // the high-frequency wave/foam detail. This is an on-demand integration
        // smoke check, not a tight pixel test.
        constexpr f64 kGoldenRmseThreshold = 6.0;

        // Mean RMSE over the RGB channels (alpha ignored) between two equal-size
        // RGBA8 buffers, in 0..255 units.
        [[nodiscard]] f64 Rgba8Rmse(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size() || a.empty())
                return std::numeric_limits<f64>::max();
            f64 sumSq = 0.0;
            std::size_t count = 0;
            for (std::size_t i = 0; i + 3 < a.size(); i += 4)
            {
                for (int c = 0; c < 3; ++c)
                {
                    const f64 d = static_cast<f64>(a[i + c]) - static_cast<f64>(b[i + c]);
                    sumSq += d * d;
                    ++count;
                }
            }
            return count ? std::sqrt(sumSq / static_cast<f64>(count)) : 0.0;
        }

        [[nodiscard]] bool GoldenRebaseRequested()
        {
            return OloEngine::Tests::Options().GoldenRebase;
        }

        void WriteWaterEvidence(const std::string& stem, const std::vector<u8>& pixels, u32 width, u32 height)
        {
            fs::create_directories("assets/tests/visual");
            const auto path = fs::path("assets/tests/visual") / (stem + ".png");
            const auto stride = static_cast<sizet>(width) * 4;
            std::vector<u8> topDown(pixels.size());
            for (u32 y = 0; y < height; ++y)
                std::copy_n(pixels.data() + (height - y - 1u) * stride, stride, topDown.data() + y * stride);
            ASSERT_NE(stbi_write_png(path.string().c_str(), static_cast<int>(width), static_cast<int>(height),
                                     4, topDown.data(), static_cast<int>(stride)),
                      0);
        }
    } // namespace

    class WaterVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();

            // Opt the fixture's Scene into the full 3D draw path. EnableRendering
            // turns on 3D mode, sizes the Scene cameras AND the Renderer3D
            // render-graph targets to kWidth x kHeight, and flips rendering on, so
            // the full graph (scene/water/post/tone-map/composite passes) executes
            // and produces a resolvable composited frame. The actual per-pose
            // render is driven by RunEditorFrames in Capture() below, which wraps
            // each OnUpdateEditor tick in a GLStateGuard so this full-pipeline
            // render leaves no global GL state behind for later GPU tests.
            EnableRendering(kWidth, kHeight);

            // Sun.
            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 20.0f, 0.0f };
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.5f, -0.7f, -0.5f));
                dl.m_Color = glm::vec3(1.0f, 0.96f, 0.9f);
                dl.m_Intensity = 2.0f;
            }

            // Skybox / environment so the surface has something to reflect.
            // If the cubemap asset can't be resolved (cwd) the reflection just
            // stays dark — the opacity contract below doesn't depend on it.
            {
                Entity sky = scene.CreateEntity("Skybox");
                auto& env = sky.AddComponent<EnvironmentMapComponent>();
                env.m_FilePath = "assets/textures/Skybox";
                env.m_IsCubemapFolder = true;
                env.m_EnableSkybox = true;
                env.m_EnableIBL = true;
                env.m_IBLIntensity = 0.3f;
            }

            // Ocean (200 x 200 at y = 0). Underwater-fog params mirror the editor
            // scene (WaterTest.olo) so the submerged/waterline captures match what
            // the user sees in the viewport.
            {
                Entity ocean = scene.CreateEntity("Ocean");
                auto& wc = ocean.AddComponent<WaterComponent>();
                wc.m_WorldSizeX = 200.0f;
                wc.m_WorldSizeZ = 200.0f;
                wc.m_GridResolutionX = 128;
                wc.m_GridResolutionZ = 128;
                wc.m_WaveAmplitude = 0.5f;
                wc.m_RenderFromBelow = true;
                wc.m_UnderwaterFogColor = glm::vec3(0.04f, 0.18f, 0.3f);
                wc.m_UnderwaterFogDensity = 0.08f;
            }

            // Helper: a primitive-mesh entity. Mirrors the serializer — sets the
            // renderable MeshSource from the primitive, not just the enum.
            auto addPrimitive = [&scene](const char* name, MeshPrimitive prim, const glm::vec3& pos,
                                         const glm::vec3& scale, const glm::vec3& albedo)
            {
                Entity e = scene.CreateEntity(name);
                auto& tc = e.GetComponent<TransformComponent>();
                tc.Translation = pos;
                tc.Scale = scale;
                auto& mc = e.AddComponent<MeshComponent>();
                mc.m_Primitive = prim;
                Ref<Mesh> mesh = (prim == MeshPrimitive::Plane) ? MeshPrimitives::CreatePlane()
                                                                : MeshPrimitives::CreateCube();
                if (mesh)
                    mc.m_MeshSource = mesh->GetMeshSource();
                auto& mat = e.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(albedo, 1.0f));
            };

            // Seafloor 20 m down. Bright MAGENTA so any see-through is
            // unmistakable: magenta can't arise from opaque blue/teal water, so
            // if the water region shows magenta the surface is transparent.
            addPrimitive("Seafloor", MeshPrimitive::Plane, { 0.0f, -20.0f, 0.0f }, { 60.0f, 1.0f, 60.0f },
                         { 1.0f, 0.0f, 1.0f });
            // Submerged cube (a bright object that would show through transparent water).
            addPrimitive("Submerged Cube", MeshPrimitive::Cube, { -4.0f, -5.0f, 6.0f }, { 3.0f, 3.0f, 3.0f },
                         { 0.1f, 0.85f, 0.2f });
            // Pillar breaking the surface (visible above and below the water).
            addPrimitive("Pillar", MeshPrimitive::Cube, { 8.0f, -9.0f, 4.0f }, { 1.5f, 22.0f, 1.5f },
                         { 0.6f, 0.6f, 0.62f });

            // (No CameraComponent — the captures drive an EditorCamera through
            // Scene::RenderScene3D, the editor's own render path, which runs the
            // full graph including the water + tone-map/underwater-fog passes.)
        }

        // Pose an EditorCamera at a world position looking with the given
        // yaw/pitch (radians), render the scene through the full pipeline, and
        // read back + save the final composited frame.
        void Capture(const std::string& poseName, const glm::vec3& position, f32 yaw, f32 pitch,
                     std::vector<u8>& outPixels, bool compareGolden = true)
        {
            EditorCamera camera(60.0f,
                                static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            // SetPose bakes the view matrix from an explicit eye + yaw/pitch. The
            // plain SetPosition/SetYaw/SetPitch setters DON'T rebuild the view (the
            // orbit model re-derives the eye from the focal point on the next
            // UpdateView), so every pose previously rendered from the default
            // constructor view — which is why the earlier captures all looked alike.
            camera.SetPose(position, yaw, pitch);

            // RunEditorFrames drives the editor render path (OnUpdateEditor →
            // RenderScene3D → EndScene, executing the full graph incl. water +
            // tone-map/underwater-fog) for two ticks — enough to let the Gerstner
            // waves advance off the flat t=0 state. Going through the fixture
            // helper (rather than calling OnUpdateEditor directly) wraps each
            // render in a GLStateGuard(restore), so this full-pipeline editor
            // render leaves no global GL state behind for the GPU tests that run
            // after this one in the same process (the coupling issue #258 fixed).
            RunEditorFrames(camera, 2);

            // Read the SAME image the editor viewport shows in 3D mode:
            // UICompositePass output (post-processed scene + overlays). This is
            // AFTER the ToneMap pass, so it includes tone-mapping AND the
            // underwater fog. SceneColor is the pre-tone-map water-pass output and
            // therefore never shows the fog — reading it was why the fog was
            // invisible in these captures. Mirror EditorLayer's resolve order:
            // UIComposite, then ToneMapColor, then SceneColor as a last resort.
            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for pose '" << poseName << "'";

            ReadbackRgba8(fb->GetColorAttachmentHandle(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

            // glGetTextureImage returns rows bottom-up (GL origin); stbi_write_png
            // and our band sampling both treat row 0 as the TOP. Flip vertically so
            // the saved PNG is right-side up and "lower quarter of the frame" really
            // is the foreground near the camera.
            {
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
            }

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            const std::string path = (dir / ("Water_" + poseName + ".png")).string();

            // Golden-image model: with the render frozen (kCaptureTime) the frame
            // is deterministic, so the committed PNG is a stable reference. In
            // rebase mode we (re)write it; otherwise we COMPARE the freshly
            // rendered frame against it and never write — so a passing run leaves
            // the tracked PNG untouched (no churn). Pass --olo-golden-rebase
            // to update the goldens after a deliberate visual change.
            if (!compareGolden || GoldenRebaseRequested())
            {
                std::error_code ec;
                fs::create_directories(dir, ec);
                ASSERT_FALSE(ec) << "Failed to create golden dir '" << dir.string()
                                 << "': " << ec.message();
                const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth),
                                                   static_cast<int>(kHeight), 4, outPixels.data(),
                                                   static_cast<int>(kWidth) * 4);
                ASSERT_NE(wrote, 0) << "stbi_write_png failed to write golden '" << path << "'";
                return;
            }

            int gw = 0, gh = 0, gch = 0;
            stbi_uc* golden = ::stbi_load(path.c_str(), &gw, &gh, &gch, 4);
            ASSERT_NE(golden, nullptr)
                << "Missing golden '" << path << "' — rerun with --olo-golden-rebase to create it.";
            const bool sizeMatches = (gw == static_cast<int>(kWidth) && gh == static_cast<int>(kHeight));
            std::vector<u8> goldenPixels;
            if (sizeMatches)
                goldenPixels.assign(golden, golden + static_cast<std::size_t>(kWidth) * kHeight * 4u);
            ::stbi_image_free(golden);
            ASSERT_TRUE(sizeMatches) << "Golden '" << path << "' is " << gw << "x" << gh << ", expected "
                                     << kWidth << "x" << kHeight << " — rerun with --olo-golden-rebase.";

            const f64 rmse = Rgba8Rmse(outPixels, goldenPixels);
            EXPECT_LE(rmse, kGoldenRmseThreshold)
                << "Pose '" << poseName << "' diverged from golden (RMSE " << rmse << " > "
                << kGoldenRmseThreshold << "). If this is an intended visual change, rerun with "
                << "--olo-golden-rebase to update " << path;
        }
    };

    TEST_F(WaterVisualEvidenceTest, AttachmentSnapshotsValidateAcrossPathsMsaaAndReconstruction)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        struct RestoreClock
        {
            ~RestoreClock()
            {
                Time::ClearMockTime();
            }
        } const restore;
        Time::SetMockTime(kCaptureTime);
        EditorCamera camera(60.0f, static_cast<f32>(kWidth) / kHeight, 0.05f, 1000.0f);
        camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        const glm::vec3 eye{ 0.0f, 16.0f, 38.0f };
        camera.SetPose(eye, 0.0f, 0.40f);
        Entity ocean = GetScene().FindEntityByName("Ocean");
        const WaterComponent water = ocean.GetComponent<WaterComponent>();
        for (const auto& [path, name] : std::array{
                 std::pair{ RenderingPath::Forward, "Forward" },
                 std::pair{ RenderingPath::ForwardPlus, "ForwardPlus" },
                 std::pair{ RenderingPath::Deferred, "Deferred" } })
        {
            ForEachAttachmentFeedbackCell(path, name, [&](const std::string& cell, u32 settleFrames)
                                          {
                ocean.RemoveComponent<WaterComponent>();
                RunEditorFrames(camera, settleFrames);
                std::vector<u8> control;
                Capture("AttachmentOff_GL_" + cell, eye, 0.0f, 0.40f, control, false);
                ASSERT_FALSE(HasFatalFailure());
                ocean.AddComponent<WaterComponent>(water);
                RunEditorFrames(camera, settleFrames);
                std::vector<u8> pixels;
                Capture("Attachment_GL_" + cell, eye, 0.0f, 0.40f, pixels, false);
                ASSERT_FALSE(HasFatalFailure());
                EXPECT_GT(Rgba8Rmse(control, pixels), 0.1);
                ExpectAttachmentFeedbackGraphClean("WaterPass");
                ExpectAttachmentSnapshotSamplingMatchesSource("SceneDepth", "SceneDepthSnapshot");
                ExpectRefractionExtentMatchesSceneDepth("WaterRefraction", "SceneDepthSnapshot");
                const auto waterDepth = Renderer3D::GetWaterSurfaceDepthTextureID();
                ASSERT_TRUE(waterDepth.IsValid());
                const u32 depthNative = static_cast<u32>(RHI::ResourceRegistry::Get().ResolveNativeForBackend(waterDepth));
                const auto graph = RenderGraphDebugRuntime::GetActiveGraph();
                const u32 sceneDepth = graph->ResolveTexture(graph->GetTextureHandle("SceneDepthSnapshot"));
                for (const GLenum parameter : { GL_TEXTURE_WIDTH, GL_TEXTURE_HEIGHT })
                {
                    GLint waterExtent = 0;
                    GLint sceneExtent = 0;
                    glGetTextureLevelParameteriv(depthNative, 0, parameter, &waterExtent);
                    glGetTextureLevelParameteriv(sceneDepth, 0, parameter, &sceneExtent);
                    EXPECT_EQ(waterExtent, sceneExtent) << cell << " tone-map water-depth extent";
                }
                ExpectAttachmentSnapshotSamplingMatchesSource("SceneViewNormals", "SceneViewNormalsSnapshot"); });
        }
    }

    // Runs in the normal suite (SKIPs without a GL 4.6 context — see the file
    // header). The render is frozen (kCaptureTime) so each pose is deterministic
    // and the committed PNGs act as golden references: a normal run COMPARES
    // against them (RMSE) and writes nothing; pass --olo-golden-rebase to
    // (re)write the goldens after a deliberate change.
    TEST_F(WaterVisualEvidenceTest, CaptureWaterFromMultipleAngles)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Freeze the wall clock for the whole capture run so the wave phase /
        // normal-map scroll are identical every time — the determinism that lets
        // these PNGs be golden references. RAII restores the real clock on any
        // exit path (including ASSERT early-returns) so the mock can't leak.
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
        } scopedMockTime(kCaptureTime);

        struct Pose
        {
            const char* Name;
            glm::vec3 Position;
            f32 Yaw;   // radians; 0 looks toward -Z
            f32 Pitch; // radians; negative tilts the view down
        };

        // Camera sits on +Z looking toward -Z (toward the origin). With SetPose,
        // POSITIVE pitch tilts the view DOWN (forward = (0, sin(-pitch),
        // -cos(-pitch)), so pitch>0 lowers the look direction). Heights/pitches
        // frame the water surface + scene (pillar/seafloor) the way the user did
        // in the editor when the surface looked see-through.
        const std::array<Pose, 6> poses = { {
            // High up, angled down at the surface — the pillar breaks the surface
            // and the seafloor sits 20 m below. Top-down see-through test.
            { "Overview", { 0.0f, 16.0f, 38.0f }, 0.0f, 0.40f },
            // Camera just above the surface looking across to the horizon — the
            // exact grazing angle where the user saw straight through to the floor.
            { "GrazingAcross", { 0.0f, 2.5f, 42.0f }, 0.0f, 0.045f },
            // Straddling the waterline, looking flat across.
            { "Waterline", { 0.0f, 0.3f, 22.0f }, 0.0f, 0.02f },
            // Just submerged, looking slightly down — underwater fog should tint.
            { "Submerged", { 0.0f, -4.0f, 18.0f }, 0.0f, 0.10f },
            // Steep top-down from above the water at the submerged cube + seafloor,
            // the strongest see-through test (ray punches almost straight down
            // through deep water onto the magenta floor).
            { "TopDown", { 0.0f, 15.0f, 7.0f }, 0.0f, 1.30f },
            // Camera ~1.2 m UNDER the surface looking near-horizontal/up: half the
            // frame is above-water sky, half is the submerged volume. Reproduces
            // the user's waterline straddle where above-water sky was wrongly
            // flooded with underwater fog (negative pitch looks slightly up).
            { "WaterlineStraddle", { 0.0f, -1.2f, 20.0f }, 0.0f, -0.05f },
        } };

        std::vector<u8> grazingPixels;

        for (const auto& pose : poses)
        {
            std::vector<u8> pixels;
            Capture(pose.Name, pose.Position, pose.Yaw, pose.Pitch, pixels);
            if (::testing::Test::HasFatalFailure())
                return;

            // Every pose must produce non-trivial output (catches a black /
            // failed render).
            u64 lumaSum = 0;
            for (std::size_t i = 0; i < pixels.size(); i += 4)
                lumaSum += pixels[i] + pixels[i + 1] + pixels[i + 2];
            const f64 meanChannel = static_cast<f64>(lumaSum) / (static_cast<f64>(kWidth) * kHeight * 3.0);
            EXPECT_GT(meanChannel, 5.0) << "Pose '" << pose.Name << "' rendered (near-)black";

            if (std::string(pose.Name) == "GrazingAcross")
                grazingPixels = pixels;
        }

        ASSERT_FALSE(grazingPixels.empty());

        // See-through-to-seafloor contract. Sample a band low in the GrazingAcross
        // frame: water immediately in front of and below the camera. This is the
        // exact region that used to vanish — near tessellation patches were handed
        // a tess level of 0 (because the disabled-tessellation factor 0 fell
        // through calcTessLevel's mix), discarding the patches and exposing the
        // bright MAGENTA (1,0,1) seafloor. The seafloor's signature is "red and
        // blue both high, green much lower"; opaque water is blue/teal (blue
        // dominant, green present, red low). Assert the band is water, not floor.
        const u32 bandY0 = (kHeight * 3u) / 4u; // lower quarter (foreground)
        const u32 bandY1 = (kHeight * 7u) / 8u;
        u64 sumR = 0, sumG = 0, sumB = 0, count = 0;
        for (u32 y = bandY0; y < bandY1; ++y)
        {
            for (u32 x = kWidth / 4u; x < (kWidth * 3u) / 4u; ++x)
            {
                const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                sumR += grazingPixels[idx + 0];
                sumG += grazingPixels[idx + 1];
                sumB += grazingPixels[idx + 2];
                ++count;
            }
        }
        ASSERT_GT(count, 0u);
        const f64 meanR = static_cast<f64>(sumR) / count;
        const f64 meanG = static_cast<f64>(sumG) / count;
        const f64 meanB = static_cast<f64>(sumB) / count;

        // The magenta seafloor reads as "red and blue both high, green far lower".
        // If the near water is culled away this band shows it — fail loudly.
        const bool looksLikeMagentaSeafloor =
            (meanR > 110.0) && (meanB > 110.0) && (meanG < meanR * 0.55) && (meanG < meanB * 0.55);
        EXPECT_FALSE(looksLikeMagentaSeafloor)
            << "Foreground band reads as the magenta seafloor (R=" << meanR << " G=" << meanG
            << " B=" << meanB << ") — near water is being culled / is see-through. See "
                                 "Water_GrazingAcross.png";

        // And it must read as water: blue is the dominant channel and red stays
        // low (water is blue/teal, not the red-heavy seafloor or a grey wash).
        EXPECT_GE(meanB, meanR)
            << "Foreground band is not water-blue (R=" << meanR << " G=" << meanG << " B=" << meanB
            << "). See Water_GrazingAcross.png";
        EXPECT_LT(meanR, 130.0)
            << "Foreground band is too red for water (R=" << meanR << " G=" << meanG << " B=" << meanB
            << ") — likely showing the seafloor through the surface. See Water_GrazingAcross.png";
    }

    // Issue #1486. The water UBO's ScreenParams came from a CommandDispatch
    // viewport that nothing ever set, so it was ZERO, Water.glsl's screenUV was
    // (0, 0) for every fragment, and the whole surface read the one scene-depth
    // texel in a corner. That corner is bottom-left on GL and top-left on
    // Vulkan, which is why only Vulkan Forward showed it (foam over the whole
    // surface: its corner texel sat at water height) and every other cell
    // looked plausible by luck. Two halves are pinned: the UBO now carries the
    // water pass's real target size, and the shader derives its screen UV from
    // the depth texture it indexes rather than from any uniform.
    TEST_F(WaterVisualEvidenceTest, TheWaterSamplesSceneDepthAtItsOwnPixel)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
        camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        camera.SetPose({ 0.0f, 16.0f, 38.0f }, 0.0f, 0.40f);
        RunEditorFrames(camera, 2);

        const Ref<UniformBuffer> waterUBO = Renderer3D::GetWaterUBO();
        ASSERT_TRUE(waterUBO);
        const std::span<const u8> bytes = waterUBO->GetCachedData();
        ASSERT_GE(bytes.size(), sizeof(ShaderBindingLayout::WaterUBO));
        ShaderBindingLayout::WaterUBO uploaded{};
        std::memcpy(&uploaded, bytes.data(), sizeof(uploaded));
        EXPECT_NEAR(uploaded.ScreenParams.x, static_cast<f32>(kWidth), 0.5f)
            << "the water UBO's ScreenParams is not the water pass's target width (it was 0 before #1486)";
        EXPECT_NEAR(uploaded.ScreenParams.y, static_cast<f32>(kHeight), 0.5f);
        EXPECT_NEAR(uploaded.ScreenParams.z * static_cast<f32>(kWidth), 1.0f, 1e-4f);

#ifdef OLO_TEST_EDITOR_ROOT
        std::ifstream file(std::filesystem::path(OLO_TEST_EDITOR_ROOT) / "assets" / "shaders" / "Water.glsl");
        ASSERT_TRUE(file.is_open());
        const std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const auto count = [&source](std::string_view needle)
        {
            sizet n = 0;
            for (sizet at = source.find(needle); at != std::string::npos; at = source.find(needle, at + 1))
                ++n;
            return n;
        };
        EXPECT_EQ(count("vec2 screenUV = gl_FragCoord.xy / vec2(textureSize(u_SceneDepth, 0));"), 1u);
        EXPECT_EQ(count("u_ScreenParams.zw"), 0u) << "screen UVs must not come from a uniform that can be stale";
#endif
    }

    // Water with no environment map reflects none: the handler clears the
    // environment unit rather than use what is there. It used to clear only
    // when the dispatcher's texture cache said the unit held something. That
    // cache describes one replay (issue #1330), and the skybox is drawn by
    // another, so its cubemap stayed on the unit and a sea with image lighting
    // off reflected the sky it was told it did not have.
    //
    // Two frames with image lighting off, one drawing the skybox and one not,
    // from a grazing pose where the reflection is strongest. Screen-space
    // reflection is off for both: it reflects the sky's PIXELS, which is not
    // what this is about. The water below the horizon must be the same.
    TEST_F(WaterVisualEvidenceTest, WaterWithoutAnEnvironmentMapDoesNotReflectTheSkyboxCubemap)
    {
        OLO_ENSURE_GPU_OR_SKIP();
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
        } scopedMockTime(kCaptureTime);

        Entity sky = GetScene().FindEntityByName("Skybox");
        Entity ocean = GetScene().FindEntityByName("Ocean");
        ASSERT_TRUE(sky && ocean);
        ocean.GetComponent<WaterComponent>().m_SSREnabled = false;
        sky.GetComponent<EnvironmentMapComponent>().m_EnableIBL = false;

        const std::string backend = TestRenderBackend::EvidenceBackendPrefix();
        const auto capture = [&](const bool drawSkybox, const char* name, std::vector<u8>& pixels)
        {
            sky.GetComponent<EnvironmentMapComponent>().m_EnableSkybox = drawSkybox;
            Capture(TestRenderBackend::EvidenceName(name) + "Forward", { 0.0f, 3.0f, 40.0f }, 0.0f, 0.10f, pixels, false);
        };
        std::vector<u8> withoutSkybox;
        std::vector<u8> withSkybox;
        capture(false, "EnvironmentUnitNoSkybox", withoutSkybox);
        ASSERT_FALSE(HasFatalFailure());
        capture(true, "EnvironmentUnitSkyboxDrawn", withSkybox);
        ASSERT_FALSE(HasFatalFailure());

        const auto changedRows = [&](const u32 firstRow, const u32 lastRow)
        {
            sizet changed = 0;
            for (u32 y = firstRow; y < lastRow; ++y)
                for (u32 x = 0; x < kWidth; ++x)
                {
                    const sizet at = (static_cast<sizet>(y) * kWidth + x) * 4u;
                    for (sizet channel = 0; channel < 3u; ++channel)
                        if (std::abs(static_cast<i32>(withSkybox[at + channel]) - static_cast<i32>(withoutSkybox[at + channel])) > 2)
                        {
                            ++changed;
                            break;
                        }
                }
            return changed;
        };
        // Row 0 is the top of the frame. The sky fills the top quarter in this
        // pose, so the skybox arm is not vacuous; the bottom third is water.
        const sizet skyChanged = changedRows(0u, kHeight / 4u);
        const sizet waterChanged = changedRows(kHeight * 2u / 3u, kHeight);
        std::cout << "[water-environment-unit] " << backend << ": drawing the skybox changed " << skyChanged
                  << " sky pixels and " << waterChanged << " water pixels\n";
        EXPECT_GT(skyChanged, static_cast<sizet>(kWidth) * (kHeight / 4u) / 2u)
            << "the skybox is not in the frame: the arm proves nothing";
        EXPECT_EQ(waterChanged, 0u) << "water with no environment map changed when the skybox was drawn: it reflects the cubemap "
                                    << "the skybox left on the environment unit";
    }

    // The fog on a sea is the fog of its SURFACE, not of whatever lies under it
    // (issue #1721). Water is drawn blended and writes no scene depth, so the
    // fog pass used to reconstruct the seabed behind each water pixel and fog
    // that: lightly where a floor lay close under the surface, to the limit
    // where none did. Drift's islands each stood on a straight-edged patch of
    // differently coloured water for it, the shape of the terrain tile beneath.
    //
    // Two frames in heavy fog from one pose: the fixture's seafloor in place,
    // and the same floor taken out of the world. The floor is 20 m down, far
    // past the depth at which the water is opaque, so the surface is the same
    // surface in both and so must its fog be. The floor's own reach is the
    // control: it covers the middle of the frame and not the edges, so before
    // the fix the two frames differed across exactly that middle.
    TEST_F(WaterVisualEvidenceTest, TheFogOnTheSeaDoesNotDependOnTheSeabed)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        struct ScopedMockTime
        {
            explicit ScopedMockTime(const f32 time)
            {
                Time::SetMockTime(time);
            }
            ~ScopedMockTime()
            {
                Time::ClearMockTime();
            }
        } scopedMockTime(kCaptureTime);
        struct ScopedFogSettings
        {
            FogSettings Saved;
            ScopedFogSettings() : Saved(Renderer3D::GetFogSettings()) {}
            ~ScopedFogSettings()
            {
                Renderer3D::GetFogSettings() = Saved;
            }
        } scopedFog;
        {
            auto& fog = Renderer3D::GetFogSettings();
            fog.Enabled = true;
            fog.Mode = FogMode::Exponential;
            fog.Color = glm::vec3(0.85f, 0.85f, 0.80f);
            // 50% at 35 m, the distance of the water in the middle of the frame:
            // a wrong depth is then a large change in either direction.
            fog.Density = 0.02f;
            fog.HeightFalloff = 0.0f;
            fog.HeightOffset = 0.0f;
            fog.MaxOpacity = 1.0f;
            fog.EnableScattering = false;
            fog.EnableVolumetric = false;
            fog.EnableNoise = false;
            fog.EnableLightShafts = false;
        }

        Entity floor = GetScene().FindEntityByName("Seafloor");
        ASSERT_TRUE(floor);
        const std::string backend = TestRenderBackend::EvidenceBackendPrefix();
        const auto capture = [&](const f32 floorY, const char* name, std::vector<u8>& pixels)
        {
            floor.GetComponent<TransformComponent>().Translation.y = floorY;
            Capture(TestRenderBackend::EvidenceName(name) + "Forward", { 0.0f, 16.0f, 38.0f }, 0.0f, 0.40f, pixels, false);
        };
        std::vector<u8> withFloor;
        std::vector<u8> withoutFloor;
        capture(-20.0f, "SeaFogSeabedPresent", withFloor);
        ASSERT_FALSE(HasFatalFailure());
        // Past the far plane: behind the water there is then only sky.
        capture(-5000.0f, "SeaFogSeabedAbsent", withoutFloor);
        ASSERT_FALSE(HasFatalFailure());
        ASSERT_EQ(withFloor.size(), withoutFloor.size());

        // The lower half of the frame is water in this pose (row 0 is the top).
        // The submerged cube and the pillar stand in it and are the same in both
        // frames; what may not differ is the open water round them.
        sizet water = 0;
        sizet changed = 0;
        i32 largest = 0;
        for (u32 y = kHeight / 2u; y < kHeight; ++y)
        {
            for (u32 x = 0; x < kWidth; ++x)
            {
                const sizet at = (static_cast<sizet>(y) * kWidth + x) * 4u;
                i32 delta = 0;
                for (sizet channel = 0; channel < 3u; ++channel)
                    delta = std::max(delta, std::abs(static_cast<i32>(withFloor[at + channel]) - static_cast<i32>(withoutFloor[at + channel])));
                ++water;
                changed += (delta > 6) ? 1u : 0u;
                largest = std::max(largest, delta);
            }
        }
        const f64 changedShare = static_cast<f64>(changed) / static_cast<f64>(water);
        std::cout << "[sea-fog-surface-depth] " << backend << ": removing the seabed changed " << changed << " of " << water
                  << " water pixels (" << changedShare * 100.0 << "%), largest channel delta " << largest << "\n";
        // Measured: no pixel past the threshold with the fix (largest delta 1);
        // 45.8% of the half-frame with the fog reading the scene depth alone.
        EXPECT_LT(changedShare, 0.02)
            << "the sea's fog changed when the seabed under it was removed: the fog pass is fogging the floor behind the "
            << "water instead of the surface. See " << backend << "SeaFogSeabed{Present,Absent}Forward.png";
    }

    namespace
    {
        enum class HullFoamGrid
        {
            Plain,
            Tessellated,
            Projected
        };

        struct HullFoamCell
        {
            RenderingPath Path;
            HullFoamGrid Grid;
            const char* Name;
            u32 Samples = 1;
            UpscaleMode Upscale = UpscaleMode::Off;
        };

        constexpr HullFoamCell kHullFoamCells[] = {
            { RenderingPath::Forward, HullFoamGrid::Plain, "Forward_Plain" },
            { RenderingPath::Forward, HullFoamGrid::Tessellated, "Forward_Tessellated" },
            { RenderingPath::Forward, HullFoamGrid::Projected, "Forward_Projected" },
            { RenderingPath::ForwardPlus, HullFoamGrid::Plain, "ForwardPlus_Plain" },
            { RenderingPath::ForwardPlus, HullFoamGrid::Tessellated, "ForwardPlus_Tessellated" },
            { RenderingPath::ForwardPlus, HullFoamGrid::Projected, "ForwardPlus_Projected" },
            { RenderingPath::Deferred, HullFoamGrid::Plain, "Deferred_Plain" },
            { RenderingPath::Deferred, HullFoamGrid::Tessellated, "Deferred_Tessellated" },
            { RenderingPath::Deferred, HullFoamGrid::Projected, "Deferred_Projected" },
            { RenderingPath::Deferred, HullFoamGrid::Plain, "Deferred_Plain_MSAA4x", 4 },
            { RenderingPath::Forward, HullFoamGrid::Plain, "Forward_Plain_UpscaleQuality", 1, UpscaleMode::Quality },
            { RenderingPath::ForwardPlus, HullFoamGrid::Plain, "ForwardPlus_Plain_UpscaleQuality", 1, UpscaleMode::Quality },
            { RenderingPath::Deferred, HullFoamGrid::Plain, "Deferred_Plain_UpscaleQuality", 1, UpscaleMode::Quality },
        };

        std::vector<HullFoamCell> PlainGridCells()
        {
            std::vector<HullFoamCell> cells;
            for (const auto& cell : kHullFoamCells)
                if (cell.Grid == HullFoamGrid::Plain)
                    cells.push_back(cell);
            return cells;
        }

        struct HullFoamPose
        {
            const char* Name;
            glm::vec3 Eye;
            f32 Pitch;
            bool ShoreControl = false;
        };

        constexpr HullFoamPose kHullFoamPoses[] = {
            { "Close", { 0.0f, 2.5f, 6.0f }, glm::radians(23.0f) },
            { "Above", { 0.0f, 6.0f, 2.0f }, glm::radians(71.0f) },
            { "Shore", { 12.0f, 3.0f, 5.0f }, glm::radians(30.0f), true },
        };
    } // namespace

    class HullFoamEvidenceTest : public RendererAttachedTest, public ::testing::WithParamInterface<HullFoamCell>
    {
      protected:
        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return true;
        }

        void BuildScene() override
        {
            m_Camera = GetScene().CreateEntity("Primary camera");
            m_Camera.AddComponent<CameraComponent>().Camera.SetPerspective(glm::radians(60.0f), 0.05f, 200.0f);
            EnableRendering(kCaptureWidth, kCaptureHeight);

            Entity sun = GetScene().CreateEntity("Sun");
            auto& light = sun.AddComponent<DirectionalLightComponent>();
            light.m_Direction = glm::normalize(glm::vec3(-0.5f, -0.7f, -0.5f));
            light.m_Intensity = 2.0f;

            Entity ocean = GetScene().CreateEntity("Ocean");
            auto& water = ocean.AddComponent<WaterComponent>();
            water.m_WorldSizeX = 80.0f;
            water.m_WorldSizeZ = 80.0f;
            water.m_WaveAmplitude = 0.0f;
            water.m_WaveSteepness0 = 0.0f;
            water.m_WaveSteepness1 = 0.0f;
            water.m_WakeShapeEnabled = false; // isolate the foam mask from displacement
            water.m_WakeFoamEnabled = false;
            water.m_ProjectedGridEnabled = GetParam().Grid == HullFoamGrid::Projected;
            water.m_TessellationEnabled = GetParam().Grid == HullFoamGrid::Tessellated;
            m_Ocean = ocean;

            const glm::vec3 wood{ 0.26f, 0.09f, 0.035f };
            // The flooded centre is shallow enough to confuse the depth-only
            // foam term and far enough from contact to be suppressed.
            m_Hull = Cube("Hull interior", { 0.0f, -0.30f, 0.0f }, { 2.4f, 0.1f, 5.0f }, wood);
            auto& rb = m_Hull.AddComponent<Rigidbody3DComponent>();
            rb.m_Type = BodyType3D::Dynamic;
            rb.m_DisableGravity = true;
            m_Hull.AddComponent<BoxCollider3DComponent>().m_HalfExtents = { 0.5f, 0.5f, 0.5f };
            auto& boat = m_Hull.AddComponent<BoatComponent>();
            boat.m_ThrottleInput = 0.0f;
            boat.m_SteerInput = 0.0f;

            // An almost-awash ledge pins the retained contact band inside the
            // footprint. The other rail rises above the surface.
            Cube("Waterline ledge", { -1.03f, -0.07f, 0.0f }, { 0.15f, 0.10f, 4.7f }, wood);
            Cube("Hull rail", { 1.14f, 0.0f, 0.0f }, { 0.12f, 0.65f, 5.0f }, wood);
            Cube("Deep bed", { 0.0f, -8.5f, 0.0f }, { 80.0f, 1.0f, 80.0f }, { 0.12f, 0.19f, 0.16f });
            Entity shore = Cube("Sloped shore", { 12.0f, -0.25f, -1.5f }, { 7.0f, 0.4f, 5.0f },
                                { 0.55f, 0.45f, 0.32f });
            shore.GetComponent<TransformComponent>().SetRotationEuler({ 0.25f, 0.0f, 0.0f });
            Cube("Fixed pillar", { 13.5f, 0.0f, 0.0f }, { 0.7f, 3.0f, 0.7f }, { 0.40f, 0.42f, 0.43f });
        }

        Entity Cube(const char* name, glm::vec3 position, glm::vec3 scale, glm::vec3 color)
        {
            Entity entity = GetScene().CreateEntity(name);
            auto& transform = entity.GetComponent<TransformComponent>();
            transform.Translation = position;
            transform.Scale = scale;
            auto& mesh = entity.AddComponent<MeshComponent>();
            mesh.m_Primitive = MeshPrimitive::Cube;
            if (const Ref<Mesh> cube = MeshPrimitives::CreateCube())
                mesh.m_MeshSource = cube->GetMeshSource();
            entity.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor(glm::vec4(color, 1.0f));
            return entity;
        }

        static EditorCamera MakeFoamCamera(const HullFoamPose& pose)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kCaptureWidth) / kCaptureHeight, 0.05f, 200.0f);
            camera.SetViewportSize(static_cast<f32>(kCaptureWidth), static_cast<f32>(kCaptureHeight));
            camera.SetPose(pose.Eye, 0.0f, pose.Pitch);
            return camera;
        }

        static f64 MeanRgbAt(const std::vector<u8>& pixels, const HullFoamPose& pose, const glm::vec3& point, i32 radius)
        {
            const glm::vec4 clip = MakeFoamCamera(pose).GetViewProjection() * glm::vec4(point, 1.0f);
            // ReadbackComposite supplies the bottom row first.
            const glm::vec2 uv = glm::vec2(clip) / clip.w * 0.5f + 0.5f;
            const glm::ivec2 pixel(uv * glm::vec2(kCaptureWidth, kCaptureHeight));
            if (pixel.x < radius || pixel.y < radius ||
                pixel.x + radius >= static_cast<i32>(kCaptureWidth) || pixel.y + radius >= static_cast<i32>(kCaptureHeight))
            {
                ADD_FAILURE() << "the hull control region is outside the viewport";
                return 0.0;
            }

            f64 sum = 0.0;
            for (i32 y = pixel.y - radius; y <= pixel.y + radius; ++y)
            {
                for (i32 x = pixel.x - radius; x <= pixel.x + radius; ++x)
                {
                    const auto offset = (static_cast<sizet>(y) * kCaptureWidth + static_cast<sizet>(x)) * 4;
                    sum += static_cast<f64>(pixels[offset]) + pixels[offset + 1] + pixels[offset + 2];
                }
            }
            const i32 diameter = 2 * radius + 1;
            return sum / (3.0 * diameter * diameter);
        }

        void CaptureFoam(const HullFoamPose& pose, bool play, bool enabled, std::vector<u8>& pixels,
                         bool darkFoam = false)
        {
            m_Hull.GetComponent<BoatComponent>().m_Enabled = enabled;
            const EditorCamera camera = MakeFoamCamera(pose);
            const u32 settleFrames = GetParam().Upscale == UpscaleMode::Off ? 2u : 4u;
            if (play)
            {
                auto& transform = m_Camera.GetComponent<TransformComponent>();
                transform.Translation = pose.Eye;
                transform.SetRotation(glm::quat_cast(glm::mat3(glm::inverse(camera.GetViewMatrix()))));
                RunFrames(settleFrames);
            }
            else
            {
                for (u32 frame = 0; frame < settleFrames; ++frame)
                {
                    BoatWakeSystem::PublishEditorHulls(GetScene());
                    RunEditorFrames(camera, 1u);
                }
            }
            ASSERT_EQ(WaterWakeSystem::GetHullCount(), enabled ? 1u : 0u);
            const Ref<UniformBuffer> buffer = Renderer3D::GetWaterUBO();
            ASSERT_TRUE(buffer);
            ASSERT_EQ(buffer->GetCachedData().size(), sizeof(ShaderBindingLayout::WaterUBO));
            const auto params = buffer->GetData<ShaderBindingLayout::WaterUBO>();
            EXPECT_FLOAT_EQ(params.WakeShapeParams.x, enabled ? 1.0f : 0.0f);
            EXPECT_EQ(params.ProjectedGridParams.x > 0.0f, GetParam().Grid == HullFoamGrid::Projected)
                << "the camera pose fell back to the fixed water grid";
            EXPECT_EQ(params.TessParams.x > 1.0f, GetParam().Grid == HullFoamGrid::Tessellated);
            const auto& resolution = Renderer3D::GetUpscaleResolution();
            EXPECT_TRUE(resolution.Latched);
            EXPECT_EQ(resolution.Path, GetParam().Path);
            EXPECT_EQ(resolution.SceneSampleCount, GetParam().Samples);
            if (GetParam().Upscale != UpscaleMode::Off)
            {
                EXPECT_EQ(resolution.Result.Resolved, TemporalUpscalePolicy::ResolvedUpscaler::Spatial);
                const Ref<Framebuffer> sceneColor = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
                ASSERT_TRUE(sceneColor);
                const u32 renderWidth = sceneColor->GetActiveViewportWidth();
                const u32 renderHeight = sceneColor->GetActiveViewportHeight();
                EXPECT_GT(renderWidth, 0u);
                EXPECT_GT(renderHeight, 0u);
                EXPECT_LT(renderWidth, kCaptureWidth);
                EXPECT_LT(renderHeight, kCaptureHeight);
                if (enabled && pose.ShoreControl)
                {
                    std::cout << "[hull-foam-scale] " << TestRenderBackend::EvidenceName("HullFoam") << GetParam().Name
                              << (play ? "_Play" : "_Edit") << ' ' << renderWidth << 'x' << renderHeight
                              << " -> " << kCaptureWidth << 'x' << kCaptureHeight << '\n';
                }
            }

            u32 width = 0;
            u32 height = 0;
            ASSERT_TRUE(ReadbackComposite(pixels, width, height));
            ASSERT_EQ(width, kCaptureWidth);
            ASSERT_EQ(height, kCaptureHeight);
            ASSERT_EQ(pixels.size(), static_cast<sizet>(width) * height * 4);
            const char* feature = enabled ? "HullFoam" : "HullFoamOff";
            if (darkFoam)
                feature = pose.ShoreControl ? "HullFoamShoreDark" : "HullFoamContactDark";
            const std::string stem = TestRenderBackend::EvidenceName(feature) +
                                     GetParam().Name + (play ? "_Play_" : "_Edit_") + pose.Name;
            WriteWaterEvidence(stem, pixels, width, height);
        }

        static constexpr u32 kCaptureWidth = 640;
        static constexpr u32 kCaptureHeight = 360;
        Entity m_Hull;
        Entity m_Camera;
        Entity m_Ocean;
    };

    TEST_P(HullFoamEvidenceTest, SuppressesTheHullInteriorAndPreservesTheShore)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const HullFoamCell& cell = GetParam();
        if (cell.Samples > 1u && cell.Samples > Renderer3D::GetMaxMSAASamples())
            GTEST_SKIP() << "Device does not support " << cell.Samples << "x scene MSAA";
        auto& settings = Renderer3D::GetRendererSettings();
        settings.Path = cell.Path;
        settings.Deferred.MSAASampleCount = cell.Samples;
        settings.EditorDebugDrawsEnabled = false;
        auto& post = Renderer3D::GetPostProcessSettings();
        post.Upscale = cell.Upscale;
        post.Technique = UpscalerTechnique::Spatial;
        Renderer3D::ApplyRendererSettings();

        struct ResetTime
        {
            ~ResetTime()
            {
                Time::ClearMockTime();
            }
        } const resetTime;
        Time::SetMockTime(kCaptureTime);
        auto& hullTransform = m_Hull.GetComponent<TransformComponent>();
        const f32 authoredHeight = hullTransform.Translation.y;
        hullTransform.Translation.y = 10.0f;
        BoatWakeSystem::PublishEditorHulls(GetScene());
        ASSERT_EQ(WaterWakeSystem::GetHullCount(), 1u);
        const auto* hullRecord = WaterWakeSystem::GetHullData();
        EXPECT_GT(hullRecord[WaterWake::kOffsetBound].x, 0.0f) << "the airborne bounds must sit above the water surface";
        EXPECT_FLOAT_EQ(hullRecord[WaterWake::kOffsetBound].w, 0.0f) << "the authored hull must carry its owning plane";
        EXPECT_FLOAT_EQ(hullRecord[WaterWake::kOffsetControl].y, 0.0f) << "static footprints leave displacement unchanged";
        hullTransform.Translation.y = authoredHeight;
        auto& namedThreads = Tasks::FNamedThreadManager::Get();
        const auto previousThread = namedThreads.GetCurrentThreadIfKnown();
        if (previousThread != Tasks::ENamedThread::GameThread)
            ASSERT_FALSE(namedThreads.IsThreadAttached(Tasks::ENamedThread::GameThread));
        LowLevelTasks::InitGameThreadId();
        if (previousThread != Tasks::ENamedThread::GameThread)
            namedThreads.AttachToThread(Tasks::ENamedThread::GameThread);
        struct RestoreGameThread
        {
            Scene& TestScene;
            Tasks::FNamedThreadManager& Threads;
            Tasks::ENamedThread Previous;
            ~RestoreGameThread()
            {
                if (TestScene.IsRunning())
                {
                    TestScene.SetRunning(false);
                    TestScene.OnPhysics3DStop();
                }
                if (Previous != Tasks::ENamedThread::GameThread)
                {
                    Threads.DetachFromThread(Tasks::ENamedThread::GameThread);
                    if (Previous != Tasks::ENamedThread::Invalid)
                        Threads.AttachToThread(Previous);
                }
            }
        } const restoreGameThread{ GetScene(), namedThreads, previousThread };
        for (const bool play : { false, true })
        {
            if (play)
            {
                GetScene().OnPhysics3DStart();
                GetScene().SetRunning(true);
            }
            for (const HullFoamPose& pose : kHullFoamPoses)
            {
                SCOPED_TRACE(std::string(play ? "Play/" : "Edit/") + pose.Name);
                std::vector<u8> withHull;
                ASSERT_NO_FATAL_FAILURE(CaptureFoam(pose, play, true, withHull));
                std::vector<u8> control;
                ASSERT_NO_FATAL_FAILURE(CaptureFoam(pose, play, false, control));
                const f64 rmse = Rgba8Rmse(withHull, control);
                std::cout << "[hull-foam] " << TestRenderBackend::EvidenceName("HullFoam") << cell.Name
                          << (play ? "_Play_" : "_Edit_") << pose.Name << " RGB RMSE " << rmse << '\n';
                auto& ocean = m_Ocean.GetComponent<WaterComponent>();
                const f32 brightness = ocean.m_FoamBrightness;
                ocean.m_FoamBrightness = 0.0f;
                std::vector<u8> darkFoam;
                CaptureFoam(pose, play, !pose.ShoreControl, darkFoam, true);
                ocean.m_FoamBrightness = brightness;
                ASSERT_FALSE(::testing::Test::HasFatalFailure());
                if (pose.ShoreControl)
                {
                    EXPECT_LT(rmse, 0.5) << "the fixed shore changed with the hull record";
                    const f64 shoreRmse = Rgba8Rmse(control, darkFoam);
                    std::cout << "[shore-foam] " << TestRenderBackend::EvidenceName("HullFoam") << cell.Name
                              << (play ? "_Play_Shore" : "_Edit_Shore") << " RGB RMSE " << shoreRmse << '\n';
                    EXPECT_GT(shoreRmse, 1.0) << "the fixed shore rendered without visible foam";
                }
                else
                {
                    EXPECT_GT(rmse, 1.0) << "the hull footprint had no visible effect";
                    constexpr glm::vec3 kInterior{ 0.0f, 0.0f, 0.0f };
                    constexpr glm::vec3 kContactLedge{ -1.03f, 0.0f, 0.0f };
                    const f64 darkenedInterior = MeanRgbAt(control, pose, kInterior, 5) - MeanRgbAt(withHull, pose, kInterior, 5);
                    const f64 visibleContact = MeanRgbAt(withHull, pose, kContactLedge, 2) - MeanRgbAt(darkFoam, pose, kContactLedge, 2);
                    std::cout << "[hull-foam-regions] " << TestRenderBackend::EvidenceName("HullFoam") << cell.Name
                              << (play ? "_Play_" : "_Edit_") << pose.Name << " interior " << darkenedInterior
                              << " contact " << visibleContact << '\n';
                    EXPECT_GT(darkenedInterior, 10.0) << "the flooded interior must lose its bright foam";
                    EXPECT_GT(visibleContact, 10.0) << "the hull must retain visible contact foam";
                }
            }
        }
    }

    INSTANTIATE_TEST_SUITE_P(AllPaths, HullFoamEvidenceTest, ::testing::ValuesIn(kHullFoamCells),
                             [](const ::testing::TestParamInfo<HullFoamCell>& info)
                             { return std::string(info.param.Name); });

    // The post-process sees the same depth contract for each grid topology.
    // Reuse the native, multisampled and spatially upscaled plain-grid cells.
    class TiltedFogEvidenceTest : public HullFoamEvidenceTest
    {
    };

    TEST_P(TiltedFogEvidenceTest, OnlyTheWetEyeSeesUnderwaterAbsorptionLookingUphill)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const auto& cell = GetParam();
        if (cell.Samples > Renderer3D::GetMaxMSAASamples() && cell.Samples > 1u)
            GTEST_SKIP() << "Device does not support " << cell.Samples << "x scene MSAA";
        auto& settings = Renderer3D::GetRendererSettings();
        settings.Path = cell.Path;
        settings.Deferred.MSAASampleCount = cell.Samples;
        settings.EditorDebugDrawsEnabled = false;
        auto& post = Renderer3D::GetPostProcessSettings();
        post.Upscale = cell.Upscale;
        post.Technique = UpscalerTechnique::Spatial;
        Renderer3D::ApplyRendererSettings();

        struct ResetTime
        {
            ~ResetTime()
            {
                Time::ClearMockTime();
            }
        } const resetTime;
        Time::SetMockTime(kCaptureTime);
        Entity rig = GetScene().CreateEntity("Water carrier");
        constexpr f32 pitch = glm::radians(20.0f);
        rig.GetComponent<TransformComponent>().SetRotationEuler({ pitch, 0, 0 });
        m_Ocean.SetParent(rig);
        auto& water = m_Ocean.GetComponent<WaterComponent>();
        water.m_UnderwaterFogColor = { 0.85f, 0.03f, 0.35f };
        water.m_UnderwaterRefractionStrength = 0.0f;
        water.m_CausticsIntensity = 0.0f;
        water.m_GodRayIntensity = 0.0f;
        // The surface rises toward -Z. In the dry pose, uphill fragments
        // are above eye height even though the entire still plane is viewed
        // from its dry side. The eye is within the 2 m activation margin.
        constexpr f32 cameraZ = 8.0f;
        const f32 restY = -std::tan(pitch) * cameraZ;
        for (const bool wet : { false, true })
        {
            const char* pose = wet ? "Wet" : "DryUphill";
            const f32 clearance = wet ? -0.8f : 0.6f;
            const EditorCamera camera = MakeFoamCamera({ pose, { -9.0f, restY + clearance, cameraZ }, 0.0f });
            std::array<std::vector<u8>, 2> images;
            std::array<std::string, 2> names;
            u32 renderWidth = 0;
            u32 renderHeight = 0;
            for (u32 arm = 0; arm < images.size(); ++arm)
            {
                const f32 density = arm == 0 ? 0.0f : 0.8f;
                water.m_UnderwaterFogDensity = density;
                RunEditorFrames(camera, 4);
                const auto& state = Renderer3D::GetUnderwaterFogState();
                ASSERT_TRUE(state.Active) << pose;
                EXPECT_FLOAT_EQ(state.Density, density);
                EXPECT_NEAR(state.WaterSurfaceY, restY, 1e-4f);
                const auto buffer = Renderer3D::GetUnderwaterFogUBO();
                ASSERT_TRUE(buffer);
                ASSERT_EQ(buffer->GetCachedData().size(), sizeof(UnderwaterFogUBOData));
                const auto data = buffer->GetData<UnderwaterFogUBOData>();
                EXPECT_NEAR(data.Flags.w, -clearance, 1e-4f);
                EXPECT_NEAR(data.SurfaceGradient.y, -std::tan(pitch), 1e-4f);
                ASSERT_TRUE(Renderer3D::GetWaterSurfaceDepthTextureID().IsValid());

                const auto& resolution = Renderer3D::GetUpscaleResolution();
                EXPECT_TRUE(resolution.Latched);
                EXPECT_EQ(resolution.Path, cell.Path);
                EXPECT_EQ(resolution.SceneSampleCount, cell.Samples);
                const auto sceneColor = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
                ASSERT_TRUE(sceneColor);
                renderWidth = sceneColor->GetActiveViewportWidth();
                renderHeight = sceneColor->GetActiveViewportHeight();
                ASSERT_GT(renderWidth, 0u);
                ASSERT_GT(renderHeight, 0u);
                if (cell.Upscale == UpscaleMode::Quality)
                {
                    EXPECT_EQ(resolution.Result.Resolved, TemporalUpscalePolicy::ResolvedUpscaler::Spatial);
                    EXPECT_LT(renderWidth, kCaptureWidth);
                    EXPECT_LT(renderHeight, kCaptureHeight);
                }
                else
                {
                    EXPECT_EQ(renderWidth, kCaptureWidth);
                    EXPECT_EQ(renderHeight, kCaptureHeight);
                }
                u32 width = 0;
                u32 height = 0;
                ASSERT_TRUE(ReadbackComposite(images[arm], width, height));
                ASSERT_EQ(width, kCaptureWidth);
                ASSERT_EQ(height, kCaptureHeight);
                ASSERT_EQ(images[arm].size(), static_cast<sizet>(width) * height * 4);
                names[arm] = TestRenderBackend::EvidenceName(arm == 0 ? "TiltedFogOff" : "TiltedFog") + cell.Name + "_" + pose;
                ASSERT_NO_FATAL_FAILURE(WriteWaterEvidence(names[arm], images[arm], width, height));
            }
            u32 changed = 0;
            i32 largest = 0;
            for (sizet pixel = 0; pixel < images[0].size(); pixel += 4)
            {
                i32 delta = 0;
                for (sizet c = 0; c < 3; ++c)
                    delta = std::max(delta, std::abs(static_cast<i32>(images[0][pixel + c]) - images[1][pixel + c]));
                largest = std::max(largest, delta);
                if (delta > 5)
                    ++changed;
            }
            constexpr u32 pixels = kCaptureWidth * kCaptureHeight;
            std::cout << "[TiltedFog] " << names[1] << ".png vs " << names[0] << ".png scene=" << renderWidth << 'x' << renderHeight
                      << " display=" << kCaptureWidth << 'x' << kCaptureHeight << " samples=" << cell.Samples
                      << " changed=" << changed << '/' << pixels << " max=" << largest << "/255\n";
            if (wet)
                EXPECT_GT(changed, pixels / 8) << "submerged control must show absorption";
            else
                EXPECT_LT(changed, pixels / 500) << "uphill water should keep its above-water shading";
        }
    }

    // The water depth capture holds every drawn tile, but only the eye tile's
    // surface lies on the slope the underwater pass slides heights along. A
    // level tile past the downhill edge, five metres below a dry eye, slid up
    // that slope reads as a crest above the eye and is fogged over. It must keep
    // its own height and its above-water shading.
    TEST_P(TiltedFogEvidenceTest, ALevelTileBeyondATiltedEyeTileKeepsItsOwnHeight)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const auto& cell = GetParam();
        if (cell.Samples > Renderer3D::GetMaxMSAASamples() && cell.Samples > 1u)
            GTEST_SKIP() << "Device does not support " << cell.Samples << "x scene MSAA";
        auto& settings = Renderer3D::GetRendererSettings();
        settings.Path = cell.Path;
        settings.Deferred.MSAASampleCount = cell.Samples;
        settings.EditorDebugDrawsEnabled = false;
        auto& post = Renderer3D::GetPostProcessSettings();
        post.Upscale = cell.Upscale;
        post.Technique = UpscalerTechnique::Spatial;
        Renderer3D::ApplyRendererSettings();

        struct ResetTime
        {
            ~ResetTime()
            {
                Time::ClearMockTime();
            }
        } const resetTime;
        Time::SetMockTime(kCaptureTime);
        Entity rig = GetScene().CreateEntity("Water carrier");
        constexpr f32 pitch = glm::radians(20.0f);
        rig.GetComponent<TransformComponent>().SetRotationEuler({ pitch, 0, 0 });
        m_Ocean.SetParent(rig);
        auto& water = m_Ocean.GetComponent<WaterComponent>();
        water.m_UnderwaterFogColor = { 0.85f, 0.03f, 0.35f };
        water.m_UnderwaterRefractionStrength = 0.0f;
        water.m_CausticsIntensity = 0.0f;
        water.m_GodRayIntensity = 0.0f;

        // The tilted 80 m tile falls toward +Z and ends near z = 37.6. The eye
        // stands 0.6 m above it at z = 8, looking downhill.
        constexpr f32 cameraZ = 8.0f;
        const f32 restY = -std::tan(pitch) * cameraZ;
        const glm::vec3 eye{ -9.0f, restY + 0.6f, cameraZ };
        Entity level = GetScene().CreateEntity("Level tile beyond the slope");
        level.GetComponent<TransformComponent>().Translation = { eye.x, eye.y - 5.0f, 72.0f };
        auto& levelWater = level.AddComponent<WaterComponent>();
        levelWater.m_WorldSizeX = levelWater.m_WorldSizeZ = 60.0f;
        levelWater.m_WaveAmplitude = 0.0f;
        levelWater.m_WaveSteepness0 = 0.0f;
        levelWater.m_WaveSteepness1 = 0.0f;
        levelWater.m_WakeShapeEnabled = false;
        levelWater.m_WakeFoamEnabled = false;
        levelWater.m_ProjectedGridEnabled = false;
        levelWater.m_TessellationEnabled = false;
        // Slid up the eye tile's slope from z >= 42, this surface would sit
        // 7.4 m or more above where it is drawn: well above the eye.
        ASSERT_GT(std::tan(pitch) * (42.0f - cameraZ) - 5.0f, 2.0f);

        EditorCamera camera(60.0f, static_cast<f32>(kCaptureWidth) / kCaptureHeight, 0.05f, 200.0f);
        camera.SetViewportSize(static_cast<f32>(kCaptureWidth), static_cast<f32>(kCaptureHeight));
        camera.SetPose(eye, glm::pi<f32>(), 0.0f);
        ASSERT_GT(camera.GetForwardDirection().z, 0.99f);

        std::array<std::vector<u8>, 2> images;
        u32 levelPixels = 0;
        u32 sampledPixels = 0;
        for (u32 arm = 0; arm < images.size(); ++arm)
        {
            const f32 density = arm == 0 ? 0.0f : 0.8f;
            // Adding the level tile can move the component pool; look both up.
            m_Ocean.GetComponent<WaterComponent>().m_UnderwaterFogDensity = density;
            level.GetComponent<WaterComponent>().m_UnderwaterFogDensity = density;
            RunEditorFrames(camera, 4);
            const auto& state = Renderer3D::GetUnderwaterFogState();
            ASSERT_TRUE(state.Active);
            EXPECT_NEAR(state.WaterSurfaceY, restY, 1e-4f) << "the tilted tile under the eye owns the fog";
            const auto data = Renderer3D::GetUnderwaterFogUBO()->GetData<UnderwaterFogUBOData>();
            EXPECT_NEAR(data.SurfaceGradient.y, -std::tan(pitch), 1e-4f);
            EXPECT_GT(glm::length(glm::vec2(data.SurfaceTileV.x, data.SurfaceTileV.z)), 0.0f);

            if (arm == 0)
            {
                // Measure how much of the frame the level tile covers in the
                // capture the fog pass reads, so a pass is not a missed tile.
                const auto sceneColor = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
                ASSERT_TRUE(sceneColor);
                const u32 width = sceneColor->GetActiveViewportWidth();
                const u32 height = sceneColor->GetActiveViewportHeight();
                const auto target = Renderer3D::GetWaterSurfaceDepthTextureID();
                ASSERT_TRUE(target.IsValid());
                std::vector<f32> depth(static_cast<sizet>(width) * height);
                ReadbackDepth32F(target, width, height, depth);
                const glm::mat4 invVP = glm::inverse(camera.GetViewProjection());
                for (u32 y = 1; y < height; y += 2)
                    for (u32 x = 1; x < width; x += 2)
                    {
                        ++sampledPixels;
                        const f32 d = depth[static_cast<sizet>(y) * width + x];
                        if (!(d < 1.0f))
                            continue;
                        const glm::vec4 h = invVP * glm::vec4((x + 0.5f) * 2.0f / width - 1.0f, (y + 0.5f) * 2.0f / height - 1.0f, d * 2.0f - 1.0f, 1.0f);
                        if (h.z / h.w > 42.0f)
                            ++levelPixels;
                    }
            }

            u32 width = 0;
            u32 height = 0;
            ASSERT_TRUE(ReadbackComposite(images[arm], width, height));
            ASSERT_EQ(width, kCaptureWidth);
            ASSERT_EQ(height, kCaptureHeight);
            const std::string name = TestRenderBackend::EvidenceName(arm == 0 ? "LevelTileFogOff" : "LevelTileFog") + cell.Name;
            ASSERT_NO_FATAL_FAILURE(WriteWaterEvidence(name, images[arm], width, height));
        }
        u32 changed = 0;
        i32 largest = 0;
        for (sizet pixel = 0; pixel < images[0].size(); pixel += 4)
        {
            i32 delta = 0;
            for (sizet c = 0; c < 3; ++c)
                delta = std::max(delta, std::abs(static_cast<i32>(images[0][pixel + c]) - images[1][pixel + c]));
            largest = std::max(largest, delta);
            if (delta > 5)
                ++changed;
        }
        constexpr u32 pixels = kCaptureWidth * kCaptureHeight;
        std::cout << "[TiltedFog] LevelTile" << cell.Name << " level tile covers " << levelPixels << '/' << sampledPixels
                  << " capture samples; fog on vs off changed=" << changed << '/' << pixels << " max=" << largest << "/255\n";
        // Measured 4.6% of the capture samples, at native and upscaled resolution.
        EXPECT_GT(levelPixels, sampledPixels / 40) << "the level tile must fill part of the frame";
        EXPECT_LT(changed, pixels / 500) << "a dry eye must leave the level tile's above-water shading alone";
    }

    INSTANTIATE_TEST_SUITE_P(AllPaths, TiltedFogEvidenceTest, ::testing::ValuesIn(PlainGridCells()),
                             [](const ::testing::TestParamInfo<HullFoamCell>& info)
                             { return std::string(info.param.Name); });

    class TiltedWaterEvidenceTest : public RendererAttachedTest
    {
      protected:
        static constexpr u32 kSize = 256;
        bool RunsInVulkanProcess() const override
        {
            return true;
        }

        void BuildScene() override
        {
            EnableRendering(kSize, kSize);
            m_Water = GetScene().CreateEntity("Water");
            auto& water = m_Water.AddComponent<WaterComponent>();
            water.m_WorldSizeX = 18.0f;
            water.m_WorldSizeZ = 18.0f;
            water.m_GridResolutionX = water.m_GridResolutionZ = 96;
            water.m_WaveAmplitude = 0.4f;
            water.m_WaveSpeed = 0.0f;
            m_Rig = GetScene().CreateEntity("Water carrier");
            auto& carrier = m_Rig.GetComponent<TransformComponent>();
            carrier.Translation = { 3, 2, -4 };
            carrier.SetRotationEuler({ 0.18f, -0.15f, 0.35f });
            auto& sun = GetScene().CreateEntity("Sun").AddComponent<DirectionalLightComponent>();
            sun.m_Direction = glm::normalize(glm::vec3(-0.3f, -0.9f, -0.2f));
        }

        void Measure(const char* feature, const char* path, const char* angle, const EditorCamera& camera,
                     std::vector<f32>& depth)
        {
            RunEditorFrames(camera, 3);
            const auto target = Renderer3D::GetWaterSurfaceDepthTextureID();
            ASSERT_TRUE(target.IsValid());
            depth.resize(kSize * kSize);
            ReadbackDepth32F(target, kSize, kSize, depth);
            const auto volumes = WaterProbe::CollectEnabledVolumes(&GetScene());
            ASSERT_EQ(volumes.size(), 1u);
            const auto invVP = glm::inverse(camera.GetViewProjection());
            f64 squaredError = 0.0;
            u32 samples = 0;
            u32 samplesAboveEye = 0;
            // Deproject the water-only depth capture. Each sampled pixel has a
            // real rasterized position independent of the CPU wave inversion.
            for (u32 y = 2; y < kSize; y += 4)
            {
                for (u32 x = 2; x < kSize; x += 4)
                {
                    const f32 d = depth[y * kSize + x];
                    if (!(d < 1.0f))
                        continue;
                    const glm::vec4 ndc{ (static_cast<f32>(x) + 0.5f) * 2.0f / kSize - 1.0f,
                                         (static_cast<f32>(y) + 0.5f) * 2.0f / kSize - 1.0f, d * 2.0f - 1.0f, 1 };
                    const glm::vec4 h = invVP * ndc;
                    const glm::vec3 point = glm::vec3(h) / h.w;
                    if (point.y > camera.GetPosition().y + 0.15f)
                        ++samplesAboveEye;
                    const f32 diff = WaterProbe::SampleSurfaceY(volumes.front(), { point.x, point.z }, 0) - point.y;
                    squaredError += static_cast<f64>(diff) * diff;
                    ++samples;
                }
            }
            ASSERT_GT(samples, 300u);
            if (std::string_view(angle) == "Across")
                EXPECT_GT(samplesAboveEye, 8u) << "the uphill side remains visible above the eye";
            const f64 rms = std::sqrt(squaredError / samples);
            EXPECT_LT(rms, 0.035) << "The rasterized water and buoyancy surface disagree";
            std::vector<u8> pixels;
            u32 width, height;
            ASSERT_TRUE(ReadbackComposite(pixels, width, height));
            ASSERT_EQ(width, kSize);
            ASSERT_EQ(height, kSize);
            const std::string name = TestRenderBackend::EvidenceName(feature) + path + "_" + angle;
            WriteWaterEvidence(name, pixels, width, height);
            std::cout << "[TiltedWater] " << name << " samples=" << samples << " depth/probe RMS=" << rms << " m\n";
        }

        Entity m_Water;
        Entity m_Rig;
    };

    TEST_F(TiltedWaterEvidenceTest, FogEyeDepthUsesTheShoreFieldDrawnAcrossOverlappingFFTTiles)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        struct ResetTime
        {
            ~ResetTime()
            {
                Time::ClearMockTime();
            }
        } const resetTime;
        Time::SetMockTime(kCaptureTime);

        Entity largeTile = GetScene().CreateEntity("Shore settings owner");
        largeTile.GetComponent<TransformComponent>().Translation.y = -2.0f;
        auto& large = largeTile.AddComponent<WaterComponent>();
        auto& local = m_Water.GetComponent<WaterComponent>();
        for (auto* water : { &large, &local })
        {
            water->m_UseFFT = true;
            water->m_FFTUseGpuCompute = false; // upload the same deterministic field we can read below
            water->m_FFTCascades = Ocean::kSingleCascadeCount;
            water->m_FFTResolution = 32;
            water->m_FFTPatchSize = 16.0f;
            water->m_FFTAmplitude = 6.0f;
            water->m_FFTChoppiness = 0.0f; // the query column is the undisplaced parameter
            water->m_FFTHeightScale = 1.0f;
            water->m_GridResolutionX = water->m_GridResolutionZ = 32;
        }
        large.m_WorldSizeX = large.m_WorldSizeZ = 64.0f;
        large.m_ShoreWavesEnabled = true;
        large.m_ShoreBreakerIndex = 0.10f;
        local.m_ShoreWavesEnabled = false;
        local.m_ShoreBreakerIndex = 0.02f;
        Entity bed = GetScene().CreateEntity("Flat seabed");
        bed.GetComponent<TransformComponent>().Translation = { -32, -3, -32 };
        auto& terrain = bed.AddComponent<TerrainComponent>();
        terrain.m_WorldSizeX = terrain.m_WorldSizeZ = 64.0f;
        terrain.m_ProceduralEnabled = false;
        terrain.m_CollisionEnabled = false;

        EditorCamera camera(60.0f, 1.0f, 0.05f, 100.0f);
        camera.SetViewportSize(kSize, kSize);
        camera.SetPose({ 0, 0.25f, 0 }, 0, 0.1f);
        RunEditorFrames(camera, 2);
        ASSERT_TRUE(local.m_OceanField && local.m_OceanField->GetDisplacementTextureHandle().IsValid());
        ASSERT_TRUE(local.m_OceanField->GetDerivativesTextureHandle().IsValid());
        // Select a crest tall enough for the two breaker settings to place
        // the eye on opposite sides of the visible waterline.
        glm::vec2 crestXZ(0.0f);
        f32 crest = 0.0f;
        for (i32 z = -7; z <= 7; ++z)
            for (i32 x = -7; x <= 7; ++x)
            {
                const glm::vec2 column(x, z);
                const f32 h = local.m_OceanField->SampleCascades(column).Height;
                if (h > crest)
                {
                    crest = h;
                    crestXZ = column;
                }
            }
        ASSERT_GT(crest, 0.5f);
        constexpr f32 eyeY = 0.25f;
        camera.SetPose({ crestXZ.x, eyeY, crestXZ.y }, 0, 0.1f);
        for (const bool localShore : { false, true })
        {
            local.m_ShoreWavesEnabled = localShore;
            large.m_ShoreBreakerIndex = localShore ? 2.0f : 0.10f;
            RunEditorFrames(camera, 2);
            const auto waterUBO = Renderer3D::GetWaterUBO();
            const auto fogUBO = Renderer3D::GetUnderwaterFogUBO();
            ASSERT_TRUE(waterUBO && fogUBO);
            ASSERT_EQ(waterUBO->GetCachedData().size(), sizeof(ShaderBindingLayout::WaterUBO));
            ASSERT_EQ(fogUBO->GetCachedData().size(), sizeof(UnderwaterFogUBOData));
            const auto drawn = waterUBO->GetData<ShaderBindingLayout::WaterUBO>();
            const auto fog = fogUBO->GetData<UnderwaterFogUBOData>();
            ASSERT_GT(drawn.ShoreParams.w, 0.0f);
            ASSERT_FLOAT_EQ(drawn.ShoreParams2.x, large.m_ShoreBreakerIndex);
            ASSERT_GT(fog.Flags.x, 0.0f);
            ASSERT_NEAR(fog.Flags.y, 0.0f, 1e-5f) << "the nearest tile owns the fog";
            const auto shore = WaterShoreDepthSystem::SampleWorld(crestXZ);
            ASSERT_TRUE(shore.Enabled);
            ASSERT_NEAR(shore.Depth, 1.0f, 1e-4f);
            const f32 drawnCrest = std::min(crest, drawn.ShoreParams2.x * shore.Depth);
            EXPECT_NEAR(fog.Flags.w, drawnCrest - eyeY, 1e-4f);
        }
    }

    TEST_F(TiltedWaterEvidenceTest, BuoyancyAndTheDrawnSurfaceCoincideAcrossPaths)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        for (const auto& [path, name] : std::array{
                 std::pair{ RenderingPath::Forward, "Forward" },
                 std::pair{ RenderingPath::ForwardPlus, "ForwardPlus" },
                 std::pair{ RenderingPath::Deferred, "Deferred" } })
        {
            auto& settings = Renderer3D::GetRendererSettings();
            settings.Path = path;
            settings.EditorDebugDrawsEnabled = false;
            settings.Deferred.MSAASampleCount = 1;
            Renderer3D::GetPostProcessSettings().Upscale = UpscaleMode::Off;
            Renderer3D::ApplyRendererSettings();
            for (const bool overhead : { false, true })
            {
                const char* angle = overhead ? "Above" : "Across";
                EditorCamera camera(50.0f, 1.0f, 0.1f, 100.0f);
                camera.SetViewportSize(kSize, kSize);
                camera.SetPose(overhead ? glm::vec3(3, 21, -3) : glm::vec3(3, 3.3f, 10), 0.0f,
                               overhead ? 1.50f : 0.12f);
                m_Water.SetParent({});
                m_Water.GetComponent<TransformComponent>() = m_Rig.GetComponent<TransformComponent>();
                std::vector<f32> root, child;
                ASSERT_NO_FATAL_FAILURE(Measure("TiltedWaterOff", name, angle, camera, root));
                m_Water.SetParent(m_Rig);
                m_Water.GetComponent<TransformComponent>() = TransformComponent{};
                ASSERT_NO_FATAL_FAILURE(Measure("TiltedWater", name, angle, camera, child));
                ASSERT_EQ(root.size(), child.size());
                f32 maxDelta = 0.0f;
                for (sizet i = 0; i < root.size(); ++i)
                    maxDelta = std::max(maxDelta, std::abs(child[i] - root[i]));
                EXPECT_LT(maxDelta, 1.0e-5f) << name << ' ' << angle;
            }
        }
    }
} // namespace OloEngine::Tests
