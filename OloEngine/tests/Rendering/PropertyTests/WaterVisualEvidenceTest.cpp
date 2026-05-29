// =============================================================================
// WaterVisualEvidenceTest.cpp
//
// Visual evidence (PNG) for the water surface + underwater rendering
// (WATER_FUTURE_IMPROVEMENTS.md §7.2). Renders a water scene (ocean + seafloor
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
// DISABLED by default
// ------------------
//   This test drives the full editor render path (OnUpdateEditor →
//   RenderScene3D → EndScene) by enabling rendering on the fixture's Scene.
//   Per RendererAttachedTest's notes, a full-pipeline render leaves Renderer3D
//   global GL state in a way that perturbs later GPU tests in the same process
//   (the documented "untangle rendering side-effects" follow-up). So, like
//   AssetSceneLoadTest, it is DISABLED_ and run on demand for visual evidence:
//
//     <test binary> --gtest_also_run_disabled_tests \
//                   --gtest_filter=*WaterVisualEvidence*
//
//   (run from OloEditor/ so assets resolve and PNGs land under
//   OloEditor/assets/tests/visual/). The water rendering contracts that DO run
//   in CI live in WaterRenderingTest.cpp (fog segment math, the per-fragment
//   waterline side rule, UBO layout).
//
// Classification: L8 / integration (full GL pipeline + RGBA8 readback + PNG).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;
    } // namespace

    class WaterVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            SetViewport(kWidth, kHeight);
            Scene& scene = GetScene();

            // Drive the editor render path (OnUpdateEditor → RenderScene3D →
            // EndScene). The fixture leaves rendering off by default; turn it on
            // (and 3D mode) so the full graph — scene/water/tone-map passes —
            // executes and produces a resolvable SceneColor.
            scene.SetRenderingEnabled(true);
            scene.SetIs3DModeEnabled(true);

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
                     std::vector<u8>& outPixels)
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

            // OnUpdateEditor runs the editor render path (→ RenderScene3D →
            // EndScene, executing the full graph incl. water + tone-map/
            // underwater-fog). Two ticks let the Gerstner waves advance off the
            // flat t=0 state.
            const Timestep dt(1.0f / 60.0f);
            GetScene().OnUpdateEditor(dt, camera);
            GetScene().OnUpdateEditor(dt, camera);

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

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
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

            fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ASSERT_FALSE(ec) << "Failed to create visual-evidence dir '" << dir.string()
                             << "': " << ec.message();
            const std::string path = (dir / ("Water_" + poseName + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth),
                                               static_cast<int>(kHeight), 4, outPixels.data(),
                                               static_cast<int>(kWidth) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed to write '" << path << "'";
        }
    };

    // DISABLED by default — see the file header. Run on demand with
    // --gtest_also_run_disabled_tests to (re)generate the PNG evidence.
    TEST_F(WaterVisualEvidenceTest, DISABLED_CaptureWaterFromMultipleAngles)
    {
        OLO_ENSURE_GPU_OR_SKIP();

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
} // namespace OloEngine::Tests
