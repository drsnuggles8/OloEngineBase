// OLO_TEST_LAYER: L8
// =============================================================================
// MeshVisibilityEvidenceTest.cpp
//
// The regression guard for issue #931: "a scene renders sky + water while every
// mesh in it draws nothing".
//
// THE SCENE IS THE ONE FROM THE REPORT — a `WaterComponent`, a
// `DirectionalLightComponent`, a `ProceduralSkyComponent`, two `ModelComponent`
// boats loaded from .glb, and a plain red `MeshComponent` cube as a control —
// captured through `RunEditorFrames` + `ResolveFrameGraphFramebuffer`, from
// three camera angles. Written to
//     OloEditor/assets/tests/visual/MeshVisibility_<pose>.png
//
// AND IT SITS 250 m FROM THE WORLD ORIGIN, ON PURPOSE. That is what made the
// original failure invisible: the captures were rendering from the EditorCamera
// constructor's default orbit view — eye (0, 0, 10) looking at the origin —
// because the test posed its camera with `SetPosition`/`SetYaw`/`SetPitch`,
// which stashed the members and never rebuilt the view matrix. Over an open sea
// that pose is a perfectly plausible frame of sky and water, so nothing looked
// broken; the subject was simply a quarter of a kilometre off-screen. A scene
// built AT the origin would have been framed by the stale camera by accident
// and the guard would pass for the wrong reason.
//
// WHAT IS ACTUALLY ASSERTED (VisualEvidenceGuards.h — the reusable half):
//   1. NOISE FLOOR. The same pose is captured twice and the RMSE between the
//      two is measured rather than assumed. Every threshold below is a multiple
//      of that measurement.
//   2. THE POSES ARE DISTINCT. Each pair of the three angles must differ by
//      far more than that floor. This is the assertion the original test lacked
//      and that would have failed instantly: a frozen camera makes every pair
//      differ by the floor exactly.
//   3. THE SUBJECT IS ON SCREEN. A content mask finds the control cube's red in
//      every capture. Distinctness alone is not enough — three different, valid,
//      empty skies are still three frames that prove nothing.
//
// Goldens follow the sibling convention (WaterVisualEvidenceTest): a normal run
// COMPARES against the committed PNGs and writes nothing; --olo-golden-rebase
// (re)writes them — but only if every guard above passed, so a rebase can never
// bake the very frames the guards exist to reject. Runs in the normal suite; SKIPs cleanly without a GL 4.6
// context. The CPU half of this contract — that the pose calls move the view
// matrix at all — is Rendering/EditorCameraPoseTest.cpp.
// =============================================================================

#include "OloEnginePCH.h"
#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"
#include "VisualEvidenceGuards.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
#include <gtest/gtest-spi.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <array>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;

        // Generous, same reasoning as the sibling evidence tests: the sea's
        // high-frequency wave and foam detail carries a few grey levels of
        // cross-GPU float variance. The contracts that matter here are the
        // guards, not the pixels.
        constexpr f64 kGoldenRmseThreshold = 8.0;

        // The subject sits here, far from the world origin — see the header
        // comment. Everything else in the scene is placed relative to it.
        constexpr glm::vec3 kSubject{ 0.0f, 0.0f, -250.0f };

        // The control cube's albedo is pure red at full intensity. Nothing else
        // in this scene can produce a red-dominant pixel: the sky is blue to
        // grey, the sea is teal, the hull is a desaturated tan whose red channel
        // never reaches twice its blue.
        [[nodiscard]] bool IsControlCubeRed(u32 r, u32 g, u32 b)
        {
            return r >= 70u && r >= g * 2u && r >= b * 2u;
        }

        [[nodiscard]] fs::path VisualDir()
        {
            return fs::path("assets") / "tests" / "visual";
        }

        [[nodiscard]] bool GoldenRebaseRequested()
        {
            return OloEngine::Tests::Options().GoldenRebase;
        }

        struct Pose
        {
            const char* Name;
            glm::vec3 Eye;
            f32 Yaw;
            f32 Pitch;
        };

        // Three angles on the boat, all of which frame the control cube.
        //
        // Yaw 0 looks down -Z and the forward direction is
        // `(sin(yaw), -sin(pitch), -cos(yaw))` — so POSITIVE yaw swings to the
        // right and POSITIVE pitch tilts DOWN. EditorCameraPoseTest pins the
        // pitch sign, because getting it backwards is how the editor's own
        // default pose ended up under the grid looking at the sky. Getting the
        // yaw sign backwards is how the first bake of this set framed the
        // "Quarter" pose at open water: sky, sea, and no subject — the #931
        // picture, produced this time by a camera that really did move. Which
        // is the second half of the lesson: `ExpectFrameHasSubject` is not
        // redundant with `ExpectCapturesAreDistinct`, and it is the one that
        // caught this.
        constexpr std::array<Pose, 3> kPoses{ {
            { "Astern", { 0.0f, 4.2f, -238.0f }, 0.0f, 0.06f },
            { "Quarter", { 11.0f, 3.4f, -240.0f }, -0.83f, 0.22f },
            { "Above", { 0.0f, 15.0f, -232.0f }, 0.0f, 0.65f },
        } };
    } // namespace

    class MeshVisibilityEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            // Derive the live renderer flags from the settings the way the editor
            // does at scene load, so the captures are order-independent (the same
            // call, for the same reason, as AtmosphereVisualEvidenceTest).
            Renderer3D::ApplyRendererSettings();

            {
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.32f, -0.78f, 0.54f));
                dl.m_Intensity = 3.0f;
                // Shadows off: this test is about whether geometry reaches the
                // screen at all, and a cascade bake per capture buys it nothing.
                dl.m_CastShadows = false;
            }

            {
                Entity atmosphere = scene.CreateEntity("Atmosphere");
                auto& sky = atmosphere.AddComponent<ProceduralSkyComponent>();
                sky.m_Turbidity = 2.6f;
                sky.m_SunDiskSize = 1.1f;
                sky.m_IBLIntensity = 0.8f;
                sky.m_CubemapResolution = 128; // four captures — keep each cheap
            }

            {
                Entity sea = scene.CreateEntity("Sea");
                auto& wc = sea.AddComponent<WaterComponent>();
                wc.m_WorldSizeX = 1000.0f;
                wc.m_WorldSizeZ = 1000.0f;
                wc.m_GridResolutionX = 256;
                wc.m_GridResolutionZ = 256;
                wc.m_WaveAmplitude = 0.12f;
                wc.m_WaveSpeed = 1.0f;
                wc.m_UseFFT = false;
                wc.m_RenderFromBelow = true;
            }

            // The two ModelComponent entities from the report, loaded from the
            // shipped .glb pair. A load failure is a hard failure rather than a
            // silently emptier scene — "the model did not load" and "the model
            // loaded and did not draw" are the two hypotheses this test exists to
            // tell apart, so they must not share a symptom.
            {
                constexpr std::array<const char*, 2> kFiles{
                    "SandboxProject/Assets/Models/KenneyVehicles/ship-small-hull.glb",
                    "SandboxProject/Assets/Models/KenneyVehicles/ship-small-sail.glb",
                };
                constexpr std::array<const char*, 2> kNames{ "Boat Hull", "Boat Sail" };
                for (sizet i = 0; i < kFiles.size(); ++i)
                {
                    Ref<Model> model = Ref<Model>::Create(std::string(kFiles[i]));
                    ASSERT_TRUE(model && model->GetMeshCount() > 0)
                        << "could not load '" << kFiles[i] << "' — run from OloEditor/";
                    Entity e = scene.CreateEntity(kNames[i]);
                    e.GetComponent<TransformComponent>().Translation = kSubject;
                    auto& mc = e.AddComponent<ModelComponent>();
                    mc.m_Model = model;
                    mc.m_Visible = true;
                }
            }

            // The control: a plain red primitive cube beside the boat. #931's
            // strongest clue was that this cube vanished too, which ruled out
            // anything ModelComponent-specific — so the guard keeps it.
            {
                Entity e = scene.CreateEntity("Control Cube");
                auto& tc = e.GetComponent<TransformComponent>();
                // ABEAM of the boat, at the same Z: any camera in front of the
                // subject then sees hull and cube side by side. Set a couple of
                // metres further out and the hull occludes it from the quarter
                // pose, which reads as "the cube did not render" — the report's
                // own control failing for a reason that has nothing to do with
                // the renderer.
                tc.Translation = kSubject + glm::vec3(-9.0f, 2.0f, 0.0f);
                tc.Scale = glm::vec3(3.0f);
                auto& mc = e.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Cube;
                // Mirrors the serializer: set the renderable MeshSource, not just
                // the enum. A MeshComponent with no MeshSource is skipped by the
                // draw loop without a word.
                if (Ref<Mesh> mesh = MeshPrimitives::CreateCube(); mesh)
                    mc.m_MeshSource = mesh->GetMeshSource();
                auto& mat = e.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
                mat.m_Material.SetRoughnessFactor(0.9f);
            }
        }

        /// Render `pose` through the full editor path and return the composited
        /// frame, flipped top-down.
        [[nodiscard]] std::vector<u8> Capture(const Pose& pose)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.15f, 2500.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(pose.Eye, pose.Yaw, pose.Pitch);

            RunEditorFrames(camera, 4);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            EXPECT_TRUE(fb) << "no composited framebuffer for pose '" << pose.Name << "'";
            if (!fb)
                return {};

            std::vector<u8> pixels;
            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, pixels);
            EXPECT_EQ(pixels.size(), static_cast<sizet>(kWidth) * kHeight * 4u);
            VisualEvidence::FlipRgbaRowsInPlace(pixels, kWidth, kHeight);
            return pixels;
        }

        void CompareOrRebaseGolden(const std::string& poseName, const std::vector<u8>& pixels)
        {
            const std::string path = (VisualDir() / ("MeshVisibility_" + poseName + ".png")).string();

            if (GoldenRebaseRequested())
            {
                std::error_code ec;
                fs::create_directories(VisualDir(), ec);
                ASSERT_FALSE(ec) << "failed to create '" << VisualDir().string() << "': " << ec.message();
                const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth),
                                                   static_cast<int>(kHeight), 4, pixels.data(),
                                                   static_cast<int>(kWidth) * 4);
                ASSERT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";
                return;
            }

            int gw = 0, gh = 0, gch = 0;
            stbi_uc* golden = ::stbi_load(path.c_str(), &gw, &gh, &gch, 4);
            ASSERT_NE(golden, nullptr)
                << "missing golden '" << path << "' — rerun with --olo-golden-rebase to create it.";
            const std::vector<u8> goldenPixels(golden, golden + (static_cast<sizet>(gw) * gh * 4));
            ::stbi_image_free(golden);
            ASSERT_EQ(gw, static_cast<int>(kWidth));
            ASSERT_EQ(gh, static_cast<int>(kHeight));

            EXPECT_LE(VisualEvidence::Rgba8Rmse(pixels, goldenPixels), kGoldenRmseThreshold)
                << "pose '" << poseName << "' drifted from its golden.";
        }
    };

    TEST_F(MeshVisibilityEvidenceTest, EveryPoseMovesTheCameraAndShowsTheGeometry)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // 1. The noise floor, measured rather than guessed: the SAME pose twice,
        //    through the same code path the real captures use.
        const std::vector<u8> floorA = Capture(kPoses[0]);
        const std::vector<u8> floorB = Capture(kPoses[0]);
        ASSERT_FALSE(floorA.empty());
        ASSERT_FALSE(floorB.empty());
        const f64 noiseFloor = VisualEvidence::Rgba8Rmse(floorA, floorB);
        std::cout << "[MeshVisibility] measured noise floor RMSE = " << noiseFloor << std::endl;

        // 2. The three angles. Captured FIRST, and every guard runs BEFORE any
        //    golden is touched — see the refusal below for why that ordering is
        //    the whole point rather than a tidiness preference.
        std::vector<std::vector<u8>> captures;
        std::vector<std::string> names;
        for (const Pose& pose : kPoses)
        {
            std::vector<u8> pixels = (&pose == &kPoses[0]) ? floorB : Capture(pose);
            ASSERT_FALSE(pixels.empty()) << "pose '" << pose.Name << "'";
            captures.push_back(std::move(pixels));
            names.emplace_back(pose.Name);
        }

        // 3. The subject is actually on screen in each frame, and 4. the frames
        //    are three different pictures rather than one picture three times —
        //    the assertion #931's test did not have.
        for (sizet i = 0; i < captures.size(); ++i)
        {
            VisualEvidence::ExpectFrameHasSubject(captures[i], names[i], IsControlCubeRed);
        }
        VisualEvidence::ExpectCapturesAreDistinct(captures, names, noiseFloor);

        // 5. Only now the goldens — and a rebase REFUSES to write if either
        //    guard failed.
        //
        //    Both guards are non-fatal by design (a run should report every bad
        //    pose, not just the first), so without this the rebase path would
        //    bake three identical, subject-less frames to disk and only THEN go
        //    red. The test would be failing while the committed baselines it
        //    just wrote became the #931 artefact this whole file exists to
        //    prevent — and a rebase is exactly the moment a human is least
        //    likely to re-read the output.
        if (::testing::Test::HasNonfatalFailure())
        {
            ADD_FAILURE() << "refusing to touch the goldens: a capture guard failed above. "
                             "Fix the poses or the scene, then rebase — never rebase over a "
                             "frozen camera or a missing subject (issue #931).";
            return;
        }

        for (sizet i = 0; i < captures.size(); ++i)
        {
            CompareOrRebaseGolden(names[i], captures[i]);
        }
    }

    // The guard is only worth having if it actually fires, and a guard that
    // cannot be shown to fail is indistinguishable from one that passes
    // vacuously — which is the exact species of test #931 is about. So: feed it
    // the failure it exists to catch (one pose captured twice = a camera that
    // did not move) and require it to fail.
    TEST_F(MeshVisibilityEvidenceTest, TheDistinctnessGuardFailsOnAFrozenCamera)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const std::vector<u8> first = Capture(kPoses[0]);
        const std::vector<u8> second = Capture(kPoses[0]);
        ASSERT_FALSE(first.empty());
        ASSERT_FALSE(second.empty());

        const std::vector<std::vector<u8>> frozen{ first, second };
        const std::vector<std::string> names{ "frozen", "frozen" };
        const f64 noiseFloor = VisualEvidence::Rgba8Rmse(first, second);

        EXPECT_NONFATAL_FAILURE(VisualEvidence::ExpectCapturesAreDistinct(frozen, names, noiseFloor),
                                "the camera did not move");
    }
} // namespace OloEngine::Tests
