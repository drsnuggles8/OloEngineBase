// OLO_TEST_LAYER: L8
// =============================================================================
// ProbeBakeVisualEvidenceTest.cpp -- what a baked probe puts on the screen, the
// right way up, on both backends and every rendering path.
//
// THE SCENE. A wide, low room with an emissive GREEN floor, an emissive BLUE
// ceiling and dim grey walls far away. No lights. A reflection probe and a
// 2x2x2 light-probe volume are baked in it through the editor's own bake
// entry points (ReflectionProbeBaker::BakeProbe, LightProbeBaker::BakeVolume),
// and then two spheres are put in the room: a polished metal one, which shows
// the reflection probe, and a white diffuse one, which shows the volume.
//
// THE CLAIM. Seen from a level camera, the metal sphere reflects the ceiling
// just above its middle and the floor just below it, and the diffuse sphere is
// lit blue from above and green from below. Both follow from the bake storing
// the room the right way up.
//
// WHY THE ROOM IS LOW. A probe capture reads six faces back from a render
// target, and a Vulkan target stores its rows top-down. Handed over as read,
// the four SIDE faces of the capture are upside down while the floor and
// ceiling faces are only mirrored (#1352). In a low room the side faces hold
// floor in one half and ceiling in the other, and the bands this test reads on
// the metal sphere reflect directions 20 to 45 degrees off the horizon, which
// lie in the side faces: with the rows not turned over the two bands swap
// colour. LightProbeRasterBakeTest measures the same thing in the baked data;
// this one looks at the frame.
//
// THE FAR WALL. The first pictures this test wrote showed a faint disc on the
// wall behind the spheres, where the wall faces the camera. That was the
// split-sum table read past its edge (PBRCommon.glsl, oloEnvBRDF); the Front
// pose holds the wall to one colour.
//
// Evidence: OloEditor/assets/tests/visual/
//   ProbeBake_<Backend>_<Config>_<State>_<Pose>.png
// Config: Forward, ForwardPlus, Deferred, DeferredMsaa4, DeferredUpscaled (the
// bake renders through the main view, so it runs under that view's path, its
// sample count and its render scale). State: Off, Reflection, LightProbes.
// Pose: Front, Left, Right (level, asserted) and High (looked at). The frames
// are the runtime camera's, so they carry no editor marker.
//
// Classification: L8 (the real Scene render path, two bakes, readback, PNG).
// Runs in the --olo-rhi=vulkan process too. SKIPs cleanly without a GPU.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "TestRenderBackend.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/LightProbeBaker.h"
#include "OloEngine/Renderer/LightProbeVolumeAsset.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/ReflectionProbeBaker.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 640u;
        constexpr u32 kHeight = 480u;
        constexpr f32 kFovDegrees = 50.0f;
        constexpr f32 kCaptureTime = 4.0f;
        constexpr f32 kSphereRadius = 1.0f;
        constexpr f32 kCameraDistance = 6.5f;
        const glm::vec3 kMetalCentre{ -1.6f, 0.0f, 0.0f };
        const glm::vec3 kDiffuseCentre{ 1.6f, 0.0f, 0.0f };
        const glm::vec3 kProbePosition{ 0.0f, 0.0f, 0.0f };

        struct Rgb
        {
            f64 R = 0.0;
            f64 G = 0.0;
            f64 B = 0.0;
        };

        // A camera `kCameraDistance` from the room's centre, looking at it:
        // `Yaw` round the vertical axis from +Z, `Pitch` up from the horizon.
        struct Pose
        {
            const char* Name;
            f32 Yaw;
            f32 Pitch;
            bool Level; // the band assertions hold for a level camera only
        };
        constexpr Pose kPoses[] = {
            { "Front", 0.0f, 0.0f, true },
            { "Left", 0.6f, 0.0f, true },
            { "Right", -0.6f, 0.0f, true },
            { "High", 0.35f, 0.3f, false },
        };

        struct Frame
        {
            std::vector<u8> TopDown; // RGBA8, row 0 is the top of the picture
            glm::mat4 ViewProjection{ 1.0f };
        };

        // Where a world point lands in the picture, in pixels from the top left.
        [[nodiscard]] glm::vec2 ToPixel(const glm::mat4& viewProjection, const glm::vec3& world)
        {
            const glm::vec4 clip = viewProjection * glm::vec4(world, 1.0f);
            const glm::vec2 ndc = glm::vec2(clip) / clip.w;
            return { (ndc.x * 0.5f + 0.5f) * static_cast<f32>(kWidth), (1.0f - (ndc.y * 0.5f + 0.5f)) * static_cast<f32>(kHeight) };
        }

        // Mean colour of a band on a sphere, given as fractions of the sphere's
        // on-screen radius: `up0`..`up1` above its centre (negative is below),
        // `halfWidth` to either side.
        [[nodiscard]] Rgb SphereBand(const Frame& frame, const glm::vec3& centre, f32 up0, f32 up1, f32 halfWidth)
        {
            const glm::vec2 c = ToPixel(frame.ViewProjection, centre);
            const glm::vec2 top = ToPixel(frame.ViewProjection, centre + glm::vec3(0.0f, kSphereRadius, 0.0f));
            const f32 radiusPx = c.y - top.y;
            const f32 yA = c.y - up0 * radiusPx;
            const f32 yB = c.y - up1 * radiusPx;
            const i32 y0 = static_cast<i32>(std::floor(std::min(yA, yB)));
            const i32 y1 = static_cast<i32>(std::ceil(std::max(yA, yB)));
            const i32 x0 = static_cast<i32>(std::floor(c.x - halfWidth * radiusPx));
            const i32 x1 = static_cast<i32>(std::ceil(c.x + halfWidth * radiusPx));
            Rgb sum;
            u64 count = 0;
            for (i32 y = std::max(y0, 0); y < std::min(y1, static_cast<i32>(kHeight)); ++y)
            {
                for (i32 x = std::max(x0, 0); x < std::min(x1, static_cast<i32>(kWidth)); ++x)
                {
                    const sizet i = (static_cast<sizet>(y) * kWidth + static_cast<sizet>(x)) * 4u;
                    sum.R += frame.TopDown[i];
                    sum.G += frame.TopDown[i + 1];
                    sum.B += frame.TopDown[i + 2];
                    ++count;
                }
            }
            if (count == 0)
            {
                ADD_FAILURE() << "a sphere band is outside the picture: the camera is not framing the spheres";
                return {};
            }
            return { sum.R / static_cast<f64>(count), sum.G / static_cast<f64>(count), sum.B / static_cast<f64>(count) };
        }

        // Mean colour of the square of `halfSize` pixels round (`x`, `y`).
        [[nodiscard]] Rgb Patch(const Frame& frame, u32 x, u32 y, u32 halfSize)
        {
            Rgb sum;
            u64 count = 0;
            for (u32 py = y - halfSize; py <= y + halfSize; ++py)
            {
                for (u32 px = x - halfSize; px <= x + halfSize; ++px)
                {
                    const sizet i = (static_cast<sizet>(py) * kWidth + px) * 4u;
                    sum.R += frame.TopDown[i];
                    sum.G += frame.TopDown[i + 1];
                    sum.B += frame.TopDown[i + 2];
                    ++count;
                }
            }
            return { sum.R / static_cast<f64>(count), sum.G / static_cast<f64>(count), sum.B / static_cast<f64>(count) };
        }

        // The metal sphere, 0.17 to 0.38 of a radius off its middle: surface
        // normals 10 to 22 degrees off the horizon, so mirror directions 20 to
        // 45 degrees off it. Those lie in the capture's side faces.
        constexpr f32 kMetalBand0 = 0.17f;
        constexpr f32 kMetalBand1 = 0.38f;
        // The diffuse sphere, well up its cap and well down it.
        constexpr f32 kDiffuseBand0 = 0.45f;
        constexpr f32 kDiffuseBand1 = 0.72f;
        constexpr f32 kBandHalfWidth = 0.18f;
    } // namespace

    class ProbeBakeVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return true;
        }

        // A throwaway project: the baked volume reaches the frame the way the
        // editor's does, as an asset the scene resolves by handle.
        void SetUpAssetManager()
        {
            m_TempDir = OloEngine::Tests::TempDir("probebake");
            std::error_code ec;
            fs::remove_all(m_TempDir, ec);
            fs::create_directories(m_TempDir / "Assets", ec);
            ASSERT_FALSE(ec) << "failed to create the temp project dir: " << ec.message();
            const fs::path projectFile = m_TempDir / "ProbeBake.oloproj";
            {
                std::ofstream project(projectFile);
                project << "Project:\n"
                           "  Name: ProbeBake\n"
                           "  StartScene: \"\"\n"
                           "  AssetDirectory: \"Assets\"\n"
                           "  ScriptModulePath: \"\"\n";
            }
            ASSERT_TRUE(Project::Load(projectFile)) << "Project::Load failed for " << m_TempDir.string();
            m_AssetManager = Ref<EditorAssetManager>::Create();
            m_AssetManager->Initialize(/*startFileWatcher=*/false);
            Project::SetAssetManager(m_AssetManager);
        }

        void TearDown() override
        {
            RendererAttachedTest::TearDown();
            m_AssetManager.Reset();
            std::error_code ec;
            fs::remove_all(m_TempDir, ec);
        }

        void BuildScene() override
        {
            SetUpAssetManager();
            EnableRendering(kWidth, kHeight);
            Scene& scene = GetScene();
            auto& renderer = Renderer3D::GetRendererSettings();
            renderer.ShowGrid = false;

            m_Cube = MeshPrimitives::CreateCube()->GetMeshSource();
            // Emissive, so the room is the same colours whatever lights it and
            // on whichever path; base colour near black so nothing else adds.
            AddSlab("Floor", { 0.0f, -2.75f, 0.0f }, { 30.0f, 0.5f, 30.0f }, { 0.05f, 0.9f, 0.05f });
            AddSlab("Ceiling", { 0.0f, 2.75f, 0.0f }, { 30.0f, 0.5f, 30.0f }, { 0.05f, 0.05f, 0.9f });
            AddSlab("Wall-X", { -15.0f, 0.0f, 0.0f }, { 0.5f, 6.0f, 30.0f }, { 0.12f, 0.12f, 0.12f });
            AddSlab("Wall+X", { 15.0f, 0.0f, 0.0f }, { 0.5f, 6.0f, 30.0f }, { 0.12f, 0.12f, 0.12f });
            AddSlab("Wall-Z", { 0.0f, 0.0f, -15.0f }, { 30.0f, 6.0f, 0.5f }, { 0.12f, 0.12f, 0.12f });
            AddSlab("Wall+Z", { 0.0f, 0.0f, 15.0f }, { 30.0f, 6.0f, 0.5f }, { 0.12f, 0.12f, 0.12f });

            // The runtime camera, so the frame is the one a game shows: no
            // probe marker, no volume bounds, no grid.
            m_Camera = scene.CreateEntity("Camera");
            {
                auto& camera = m_Camera.AddComponent<CameraComponent>();
                camera.Primary = true;
                camera.Camera.SetPerspective(glm::radians(kFovDegrees), 0.05f, 200.0f);
                camera.Camera.SetViewportSize(kWidth, kHeight);
            }
            PlaceCamera(kPoses[0]);

            m_Probe = scene.CreateEntity("ReflectionProbe");
            m_Probe.GetComponent<TransformComponent>().Translation = kProbePosition;
            {
                auto& probe = m_Probe.AddComponent<ReflectionProbeComponent>();
                probe.m_InfluenceRadius = 60.0f;
                probe.m_Resolution = 128;
                probe.m_Intensity = 1.0f;
                probe.m_Active = false;
            }
            m_Volume = scene.CreateEntity("LightProbeVolume");
            {
                auto& volume = m_Volume.AddComponent<LightProbeVolumeComponent>();
                volume.m_BoundsMin = { -5.0f, -1.0f, -5.0f };
                volume.m_BoundsMax = { 5.0f, 1.0f, 5.0f };
                volume.m_Resolution = { 2, 2, 2 };
                volume.m_Spacing = 10.0f;
                volume.m_Intensity = 1.0f;
                volume.m_Mode = LightProbeVolumeComponent::Mode::Baked;
                volume.m_Active = false;
            }
        }

        void AddSlab(const char* name, const glm::vec3& position, const glm::vec3& scale, const glm::vec3& emissive)
        {
            Entity slab = GetScene().CreateEntity(name);
            auto& transform = slab.GetComponent<TransformComponent>();
            transform.Translation = position;
            transform.Scale = scale;
            auto& mesh = slab.AddComponent<MeshComponent>();
            mesh.m_Primitive = MeshPrimitive::Cube;
            mesh.m_MeshSource = m_Cube;
            auto& material = slab.AddComponent<MaterialComponent>().m_Material;
            material.SetBaseColorFactor(glm::vec4(0.02f, 0.02f, 0.02f, 1.0f));
            material.SetEmissiveFactor(glm::vec4(emissive, 1.0f));
            material.SetMetallicFactor(0.0f);
            material.SetRoughnessFactor(1.0f);
        }

        void AddSphere(const char* name, const glm::vec3& centre, f32 metallic, f32 roughness)
        {
            Entity sphere = GetScene().CreateEntity(name);
            sphere.GetComponent<TransformComponent>().Translation = centre;
            auto& mesh = sphere.AddComponent<MeshComponent>();
            mesh.m_Primitive = MeshPrimitive::Sphere;
            if (Ref<Mesh> source = MeshPrimitives::CreateSphere(kSphereRadius, 64))
            {
                mesh.m_MeshSource = source->GetMeshSource();
            }
            auto& material = sphere.AddComponent<MaterialComponent>().m_Material;
            material.SetBaseColorFactor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
            material.SetMetallicFactor(metallic);
            material.SetRoughnessFactor(roughness);
        }

        void PlaceCamera(const Pose& pose)
        {
            auto& transform = m_Camera.GetComponent<TransformComponent>();
            transform.Translation = kCameraDistance * glm::vec3(std::sin(pose.Yaw) * std::cos(pose.Pitch), std::sin(pose.Pitch),
                                                                std::cos(pose.Yaw) * std::cos(pose.Pitch));
            transform.SetRotationEuler({ -pose.Pitch, pose.Yaw, 0.0f });
        }

        void SetProbes(bool reflection, bool lightProbes)
        {
            m_Probe.GetComponent<ReflectionProbeComponent>().m_Active = reflection;
            auto& volume = m_Volume.GetComponent<LightProbeVolumeComponent>();
            volume.m_Active = lightProbes;
            volume.m_Dirty = true;
        }

        Frame Capture(const std::string& name, const Pose& pose)
        {
            Frame frame;
            PlaceCamera(pose);
            frame.ViewProjection = m_Camera.GetComponent<CameraComponent>().Camera.GetProjection() *
                                   glm::inverse(m_Camera.GetComponent<TransformComponent>().GetTransform());
            RunFrames(4);

            std::vector<u8> bottomUp;
            u32 width = 0;
            u32 height = 0;
            EXPECT_TRUE(ReadbackComposite(bottomUp, width, height)) << name << ": no frame to read back";
            if (width != kWidth || height != kHeight || bottomUp.size() != static_cast<sizet>(kWidth) * kHeight * 4u)
            {
                ADD_FAILURE() << name << ": the frame is " << width << " x " << height << ", not " << kWidth << " x " << kHeight;
                frame.TopDown.assign(static_cast<sizet>(kWidth) * kHeight * 4u, 0u);
                return frame;
            }
            const sizet rowBytes = static_cast<sizet>(kWidth) * 4u;
            frame.TopDown.resize(bottomUp.size());
            for (u32 y = 0; y < kHeight; ++y)
            {
                std::copy_n(bottomUp.data() + static_cast<sizet>(kHeight - 1u - y) * rowBytes, rowBytes,
                            frame.TopDown.data() + static_cast<sizet>(y) * rowBytes);
            }

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string file = (dir / (name + ".png")).string();
            EXPECT_NE(::stbi_write_png(file.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4, frame.TopDown.data(),
                                       static_cast<int>(rowBytes)),
                      0)
                << "could not write " << file;
            return frame;
        }

        // Bake both probes under the configured view, put the spheres in, and
        // capture every state from every pose.
        void BakeAndCapture(const std::string& config, const std::function<void()>& configure)
        {
            Time::SetMockTime(kCaptureTime);
            struct ClearMockTime
            {
                ~ClearMockTime()
                {
                    Time::ClearMockTime();
                }
            } const clearMockTime;

            configure();
            Renderer3D::ApplyRendererSettings();
            // The view exists, at its own size and under its own configuration,
            // before anything is baked through it.
            RunFrames(3);

            Ref<Scene> scene = GetSceneRef();
            bool reflectionBaked = false;
            RunInFrame([this, &scene, &reflectionBaked]()
                       { reflectionBaked = ReflectionProbeBaker::BakeProbe(scene, kProbePosition,
                                                                           m_Probe.GetComponent<ReflectionProbeComponent>()); });
            ASSERT_TRUE(reflectionBaked) << "ReflectionProbeBaker::BakeProbe failed";

            Ref<LightProbeVolumeAsset> baked = Ref<LightProbeVolumeAsset>::Create();
            RunInFrame([this, &scene, &baked]()
                       { LightProbeBaker::BakeVolume(scene, m_Volume.GetComponent<LightProbeVolumeComponent>(), baked, 32u); });
            ASSERT_TRUE(baked->HasBakedData()) << "LightProbeBaker::BakeVolume stored nothing";
            const AssetHandle handle = AssetManager::AddMemoryOnlyAsset(baked);
            ASSERT_NE(static_cast<u64>(handle), 0ULL);
            m_Volume.GetComponent<LightProbeVolumeComponent>().m_BakedDataAsset = handle;

            // After the bakes: the probes are baked in the empty room.
            AddSphere("MetalSphere", kMetalCentre, /*metallic=*/1.0f, /*roughness=*/0.12f);
            AddSphere("DiffuseSphere", kDiffuseCentre, /*metallic=*/0.0f, /*roughness=*/1.0f);

            const std::string prefix = TestRenderBackend::EvidenceName("ProbeBake") + config + "_";

            SetProbes(false, false);
            const Frame off = Capture(prefix + "Off_Front", kPoses[0]);
            const Rgb metalOff = SphereBand(off, kMetalCentre, kMetalBand0, kMetalBand1, kBandHalfWidth);
            const Rgb diffuseOff = SphereBand(off, kDiffuseCentre, -kDiffuseBand1, -kDiffuseBand0, kBandHalfWidth);

            for (const Pose& pose : kPoses)
            {
                SetProbes(true, false);
                const Frame reflection = Capture(prefix + "Reflection_" + pose.Name, pose);
                SetProbes(false, true);
                const Frame lightProbes = Capture(prefix + "LightProbes_" + pose.Name, pose);
                if (::testing::Test::HasFatalFailure() || !pose.Level)
                {
                    continue;
                }

                const Rgb metalUpper = SphereBand(reflection, kMetalCentre, kMetalBand0, kMetalBand1, kBandHalfWidth);
                const Rgb metalLower = SphereBand(reflection, kMetalCentre, -kMetalBand1, -kMetalBand0, kBandHalfWidth);
                const Rgb diffuseUpper = SphereBand(lightProbes, kDiffuseCentre, kDiffuseBand0, kDiffuseBand1, kBandHalfWidth);
                const Rgb diffuseLower = SphereBand(lightProbes, kDiffuseCentre, -kDiffuseBand1, -kDiffuseBand0, kBandHalfWidth);
                std::printf("[ProbeBake] %s%s: metal upper (%.1f, %.1f, %.1f) lower (%.1f, %.1f, %.1f); diffuse upper "
                            "(%.1f, %.1f, %.1f) lower (%.1f, %.1f, %.1f)\n",
                            prefix.c_str(), pose.Name, metalUpper.R, metalUpper.G, metalUpper.B, metalLower.R, metalLower.G,
                            metalLower.B, diffuseUpper.R, diffuseUpper.G, diffuseUpper.B, diffuseLower.R, diffuseLower.G,
                            diffuseLower.B);

                // The reflection probe: ceiling above the sphere's middle,
                // floor below it. Upside-down side faces swap the two.
                EXPECT_GT(metalUpper.B, metalUpper.G * 2.0)
                    << pose.Name << ": the metal sphere does not reflect the blue ceiling above its middle. See " << prefix
                    << "Reflection_" << pose.Name << ".png";
                EXPECT_GT(metalLower.G, metalLower.B * 2.0)
                    << pose.Name << ": the metal sphere does not reflect the green floor below its middle. See " << prefix
                    << "Reflection_" << pose.Name << ".png";

                // The light-probe volume: blue light from above, green from
                // below.
                EXPECT_GT(diffuseUpper.B, diffuseUpper.G * 1.2)
                    << pose.Name << ": the diffuse sphere is not lit blue from above. See " << prefix << "LightProbes_"
                    << pose.Name << ".png";
                EXPECT_GT(diffuseLower.G, diffuseLower.B * 1.2)
                    << pose.Name << ": the diffuse sphere is not lit green from below. See " << prefix << "LightProbes_"
                    << pose.Name << ".png";

                if (&pose == &kPoses[0])
                {
                    // The far wall, seen head-on at the middle of the picture
                    // and a few degrees to the side, between the spheres. It is
                    // one flat, fully rough surface under one environment, so
                    // the two are the same colour. They were not: the split-sum
                    // table was read with a sampler that repeats, and at
                    // NdotV = 1 a tap wrapped to the grazing column, which put
                    // a disc of environment colour on the wall where it faced
                    // the camera (23/255 brighter in green and blue here).
                    const Rgb headOn = Patch(reflection, kWidth / 2u, kHeight / 2u, 5u);
                    const Rgb beside = Patch(reflection, kWidth / 2u - 38u, kHeight / 2u, 4u);
                    std::printf("[ProbeBake] %sReflection_Front: far wall head-on (%.1f, %.1f, %.1f), beside it (%.1f, %.1f, %.1f)\n",
                                prefix.c_str(), headOn.R, headOn.G, headOn.B, beside.R, beside.G, beside.B);
                    EXPECT_NEAR(headOn.G, beside.G, 3.0) << "a rough wall is brighter where it faces the camera. See " << prefix
                                                         << "Reflection_Front.png";
                    EXPECT_NEAR(headOn.B, beside.B, 3.0) << "a rough wall is brighter where it faces the camera. See " << prefix
                                                         << "Reflection_Front.png";

                    // The premise: with neither probe the spheres show none of
                    // this, so the colours above are the probes'.
                    std::printf("[ProbeBake] %sOff_Front: metal upper (%.1f, %.1f, %.1f), diffuse lower (%.1f, %.1f, %.1f)\n",
                                prefix.c_str(), metalOff.R, metalOff.G, metalOff.B, diffuseOff.R, diffuseOff.G, diffuseOff.B);
                    EXPECT_GT(metalUpper.B, metalOff.B + 30.0) << "the reflection probe did not change the metal sphere";
                    EXPECT_GT(diffuseLower.G, diffuseOff.G + 15.0) << "the light-probe volume did not change the diffuse sphere";
                }
            }
        }

        Entity m_Camera;
        Entity m_Probe;
        Entity m_Volume;
        Ref<MeshSource> m_Cube;
        Ref<EditorAssetManager> m_AssetManager;
        fs::path m_TempDir;
    };

    TEST_F(ProbeBakeVisualEvidenceTest, ForwardShowsBothBakesTheRightWayUp)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        BakeAndCapture("Forward", []()
                       { Renderer3D::GetRendererSettings().Path = RenderingPath::Forward; });
    }

    TEST_F(ProbeBakeVisualEvidenceTest, ForwardPlusShowsBothBakesTheRightWayUp)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        BakeAndCapture("ForwardPlus", []()
                       { Renderer3D::GetRendererSettings().Path = RenderingPath::ForwardPlus; });
    }

    TEST_F(ProbeBakeVisualEvidenceTest, DeferredShowsBothBakesTheRightWayUp)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        BakeAndCapture("Deferred", []()
                       { Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred; });
    }

    // The bake renders through the main view, so a multisampled G-Buffer is
    // what it reads its faces from.
    TEST_F(ProbeBakeVisualEvidenceTest, DeferredWithFourSamplesShowsBothBakesTheRightWayUp)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        BakeAndCapture("DeferredMsaa4", []()
                       {
                           Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
                           Renderer3D::GetRendererSettings().Deferred.MSAASampleCount = 4u; });
    }

    // With an upscaler the view renders below its size. The capture sets render
    // scale 1 for its faces and puts the view's own back.
    TEST_F(ProbeBakeVisualEvidenceTest, DeferredUpscaledShowsBothBakesTheRightWayUp)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        BakeAndCapture("DeferredUpscaled", []()
                       {
                           Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
                           Renderer3D::GetPostProcessSettings().Upscale = UpscaleMode::Quality; });
    }
} // namespace OloEngine::Tests
