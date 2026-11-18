// OLO_TEST_LAYER: L8
// =============================================================================
// ParentedLocalLightEvidenceTest.cpp — a parented local light lights and
// shadows the scene exactly as a root light at the same world pose (#1707).
//
// THE BUG. The light collection packed the light entity's PARENT-RELATIVE
// TransformComponent::Translation for point, spot and sphere area lights, so a
// light under a moved parent shone from its local coordinates while its bulb
// mesh rendered in the right place. The canonical light record, Forward+, the
// multi-light UBO and the local shadow requests all inherited the local value.
//
// WHAT THIS PINS. One point, one spot and one sphere area light, all casting
// shadows past a cube onto a ground slab, built twice:
//   * ROOT: three root lights at chosen world poses;
//   * PARENTED: the same three under a parent that is translated AND rotated,
//     each authored at the local pose that composes to the same world pose
//     (the spot's direction in the parent's frame).
// Each arm is captured from two camera poses, and:
//   * the canonical record of every light matches between the arms;
//   * the frames match;
//   * the parented arm differs from MISPLACED, the frame the bug drew (root
//     lights at the local coordinates), so "the frames match" is not the
//     match of two frames the lights never reached;
//   * the root arm differs from its own shadows-off frame, so the local shadow
//     requests are in the comparison and not just the lighting.
//
// Runs on Forward, Forward+ and Deferred, and in the --olo-rhi=vulkan process.
// Classification: L8 (full pipeline through the real Scene render path). SKIPs
// cleanly without a GPU context.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "TestRenderBackend.h"

#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "OloEngine/Renderer/Instancing/InstanceData.h"
#include "OloEngine/Renderer/Instancing/InstancedMeshComponent.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kSize = 256;
        constexpr u32 kFramesPerCapture = 6;
        constexpr f32 kCaptureTime = 4.0f;

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

        struct ScopedMockTime
        {
            explicit ScopedMockTime(f32 t)
                : m_HadMock(Time::HasMockTime()), m_Previous(Time::GetMockTime())
            {
                Time::SetMockTime(t);
            }
            ~ScopedMockTime()
            {
                if (m_HadMock)
                    Time::SetMockTime(m_Previous);
                else
                    Time::ClearMockTime();
            }
            ScopedMockTime(const ScopedMockTime&) = delete;
            ScopedMockTime& operator=(const ScopedMockTime&) = delete;

          private:
            bool m_HadMock;
            f32 m_Previous;
        };

        void WriteEvidence(const std::string& name, const std::vector<u8>& rgba)
        {
            const std::filesystem::path dir = std::filesystem::path("assets") / "tests" / "visual";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            const std::string file = (dir / (name + ".png")).string();
            const sizet rowBytes = static_cast<sizet>(kSize) * 4u;
            std::vector<u8> topDown(rgba.size());
            for (u32 y = 0; y < kSize; ++y)
                std::copy_n(rgba.data() + static_cast<sizet>(kSize - 1u - y) * rowBytes, rowBytes,
                            topDown.data() + static_cast<sizet>(y) * rowBytes);
            EXPECT_NE(::stbi_write_png(file.c_str(), static_cast<int>(kSize), static_cast<int>(kSize), 4, topDown.data(),
                                       static_cast<int>(rowBytes)),
                      0)
                << "could not write " << file;
        }

        struct FrameDiff
        {
            f64 MeanAbs = 0.0;      // mean absolute 8-bit difference over RGB
            u32 PixelsOver8 = 0;    // pixels whose largest channel delta exceeds 8
            u32 MaxDelta = 0;
        };

        [[nodiscard]] FrameDiff Diff(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            FrameDiff diff;
            u64 sum = 0;
            for (sizet px = 0; px + 3 < a.size() && px + 3 < b.size(); px += 4)
            {
                u32 largest = 0;
                for (sizet c = 0; c < 3; ++c)
                {
                    const u32 delta = static_cast<u32>(std::abs(static_cast<i32>(a[px + c]) - static_cast<i32>(b[px + c])));
                    sum += delta;
                    largest = std::max(largest, delta);
                }
                diff.MaxDelta = std::max(diff.MaxDelta, largest);
                if (largest > 8u)
                    ++diff.PixelsOver8;
            }
            diff.MeanAbs = static_cast<f64>(sum) / static_cast<f64>(static_cast<sizet>(kSize) * kSize * 3u);
            return diff;
        }

        // The world poses the lights shine from, chosen so each lights its own
        // part of the ground and throws the cube's shadow across it.
        const glm::vec3 kPointWorld{ -2.5f, 2.5f, 1.5f };
        const glm::vec3 kSpotWorld{ 2.5f, 4.0f, -1.0f };
        const glm::vec3 kSpotWorldDirection{ -0.45f, -1.0f, 0.25f };
        const glm::vec3 kSphereWorld{ 0.5f, 3.0f, -3.0f };

        // The parent: translated and turned on two axes, so a collection that
        // ignores either the translation or the rotation lights elsewhere.
        const glm::vec3 kParentTranslation{ 7.0f, 1.0f, -5.0f };
        const glm::vec3 kParentEuler{ glm::radians(20.0f), glm::radians(120.0f), 0.0f };

        struct CameraPose
        {
            const char* Name;
            glm::vec3 Eye;
        };
        constexpr std::array<CameraPose, 2> kPoses{ { { "Oblique", { 0.0f, 9.0f, 9.0f } },
                                                       { "Side", { 10.0f, 4.0f, 1.0f } } } };

        enum class Arm : u8
        {
            Root,
            Parented,
            Misplaced,
            RootShadowsOff,
        };
    } // namespace

    class ParentedLocalLightEvidence : public RendererAttachedTest, public ::testing::WithParamInterface<RenderingPath>
    {
      protected:
        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return true;
        }

        Entity AddBox(const char* name, const glm::vec3& centre, const glm::vec3& scale, bool castsShadows)
        {
            Entity box = GetScene().CreateEntity(name);
            auto& imc = box.AddComponent<InstancedMeshComponent>();
            if (Ref<Mesh> cube = MeshPrimitives::CreateCube())
                imc.MeshSource = cube->GetMeshSource();
            imc.CastShadows = castsShadows;
            InstanceData inst;
            inst.Transform = glm::scale(glm::translate(glm::mat4(1.0f), centre), scale);
            inst.PrevTransform = inst.Transform;
            imc.Instances.Add(inst);
            return box;
        }

        void BuildScene() override
        {
            EnableRendering(kSize, kSize);

            auto& rs = Renderer3D::GetRendererSettings();
            rs.Path = GetParam();
            rs.ShowGrid = false;
            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.AutoExposureEnabled = false;
            pp.Exposure = 1.0f;
            Renderer3D::ApplyRendererSettings();

            m_Camera = GetScene().CreateEntity("Camera");
            auto& cam = m_Camera.AddComponent<CameraComponent>();
            cam.Primary = true;
            cam.Camera.SetPerspective(glm::radians(60.0f), 0.05f, 100.0f);
            cam.Camera.SetViewportSize(kSize, kSize);

            // The ground receives, the cube casts: every local light throws
            // the cube's shadow onto the ground, so a shadow request at the
            // wrong position moves a visible shadow.
            (void)AddBox("Ground", { 0.0f, -0.1f, 0.0f }, { 20.0f, 0.2f, 20.0f }, false);
            (void)AddBox("Cube", { 0.0f, 0.75f, 0.0f }, { 1.5f, 1.5f, 1.5f }, true);

            m_Parent = GetScene().CreateEntity("Tower");
            auto& parent = m_Parent.GetComponent<TransformComponent>();
            parent.Translation = kParentTranslation;
            parent.SetRotationEuler(kParentEuler);
        }

        void AimCamera(const CameraPose& pose)
        {
            auto& transform = m_Camera.GetComponent<TransformComponent>();
            transform.Translation = pose.Eye;
            transform.SetRotation(glm::quatLookAt(glm::normalize(-pose.Eye), glm::vec3(0.0f, 1.0f, 0.0f)));
        }

        [[nodiscard]] glm::mat4 ParentMatrix() const
        {
            return m_Parent.GetComponent<TransformComponent>().GetTransform();
        }

        // Replaces the lights with the arm's three.
        void PlaceLights(Arm arm)
        {
            for (Entity& light : m_Lights)
            {
                if (light)
                    GetScene().DestroyEntity(light);
                light = {};
            }

            const glm::mat4 toLocal = glm::inverse(ParentMatrix());
            const glm::quat parentRotation = m_Parent.GetComponent<TransformComponent>().GetRotation();
            const bool parented = arm == Arm::Parented;
            const auto authoredPosition = [&](const glm::vec3& world)
            {
                // MISPLACED is where the bug lit from: the local coordinates,
                // read as world ones by a root light.
                return (parented || arm == Arm::Misplaced) ? glm::vec3(toLocal * glm::vec4(world, 1.0f)) : world;
            };
            const auto place = [&](const char* name, const glm::vec3& world)
            {
                Entity light = GetScene().CreateEntity(name);
                light.GetComponent<TransformComponent>().Translation = authoredPosition(world);
                if (parented)
                    light.SetParent(m_Parent);
                return light;
            };
            const bool casts = arm != Arm::RootShadowsOff;

            m_Lights[0] = place("Point", kPointWorld);
            auto& point = m_Lights[0].AddComponent<PointLightComponent>();
            point.m_Color = { 1.0f, 0.85f, 0.6f };
            point.m_Intensity = 30.0f;
            point.m_Range = 14.0f;
            point.m_CastShadows = casts;

            m_Lights[1] = place("Spot", kSpotWorld);
            auto& spot = m_Lights[1].AddComponent<SpotLightComponent>();
            spot.m_Color = { 0.6f, 0.8f, 1.0f };
            spot.m_Intensity = 40.0f;
            spot.m_Range = 16.0f;
            spot.m_InnerCutoff = 20.0f;
            spot.m_OuterCutoff = 32.0f;
            spot.m_Direction = (parented || arm == Arm::Misplaced) ? glm::inverse(parentRotation) * kSpotWorldDirection
                                                                    : kSpotWorldDirection;
            spot.m_CastShadows = casts;

            m_Lights[2] = place("Sphere", kSphereWorld);
            auto& sphere = m_Lights[2].AddComponent<SphereAreaLightComponent>();
            sphere.m_Color = { 1.0f, 0.6f, 0.8f };
            sphere.m_Intensity = 30.0f;
            sphere.m_Range = 14.0f;
            sphere.m_Radius = 0.3f;
            sphere.m_CastShadows = casts;
        }

        // The canonical records of the three lights, in the order placed.
        [[nodiscard]] std::array<GPUSceneLight, 3> CanonicalRecords() const
        {
            std::array<GPUSceneLight, 3> records{};
            constexpr std::array<GPUSceneLightType, 3> kTypes{ GPUSceneLightType::Point, GPUSceneLightType::Spot,
                                                               GPUSceneLightType::SphereArea };
            const GPUScene& gpuScene = Renderer3D::GetGPUScene();
            for (sizet i = 0; i < records.size(); ++i)
            {
                const GPUSceneLightKey key{ .m_EntityId = static_cast<u64>(m_Lights[i].GetUUID()),
                                            .m_Type = std::to_underlying(kTypes[i]) };
                const GPUSceneLight* record = gpuScene.GetLightRecord(gpuScene.FindLight(key));
                EXPECT_NE(record, nullptr) << "no canonical record for light " << i;
                if (record)
                    records[i] = *record;
            }
            return records;
        }

        void Capture(Arm arm, const CameraPose& pose, std::vector<u8>& out, std::array<GPUSceneLight, 3>* records = nullptr)
        {
            PlaceLights(arm);
            AimCamera(pose);
            RunFrames(kFramesPerCapture);
            u32 w = 0, h = 0;
            ASSERT_TRUE(ReadbackComposite(out, w, h)) << "no composited frame to read back";
            ASSERT_EQ(w, kSize);
            ASSERT_EQ(h, kSize);
            if (records)
                *records = CanonicalRecords();
        }

        Entity m_Camera;
        Entity m_Parent;
        std::array<Entity, 3> m_Lights{};
    };

    TEST_P(ParentedLocalLightEvidence, AParentedLightLightsAndShadowsLikeItsRootTwin)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime mockTime(kCaptureTime);
        const std::string cell = std::string(PathName(GetParam()));

        for (const CameraPose& pose : kPoses)
        {
            SCOPED_TRACE(pose.Name);
            const std::string name = TestRenderBackend::EvidenceName("ParentedLocalLight") + cell + "_" + pose.Name;

            std::vector<u8> root, parented, misplaced, shadowsOff;
            std::array<GPUSceneLight, 3> rootRecords{}, parentedRecords{};
            Capture(Arm::Root, pose, root, &rootRecords);
            ASSERT_FALSE(HasFatalFailure());
            Capture(Arm::Parented, pose, parented, &parentedRecords);
            ASSERT_FALSE(HasFatalFailure());
            Capture(Arm::Misplaced, pose, misplaced);
            ASSERT_FALSE(HasFatalFailure());
            Capture(Arm::RootShadowsOff, pose, shadowsOff);
            ASSERT_FALSE(HasFatalFailure());

            WriteEvidence(name + "_Root", root);
            WriteEvidence(name + "_Parented", parented);
            WriteEvidence(name + "_Misplaced", misplaced);
            WriteEvidence(name + "_RootShadowsOff", shadowsOff);

            // The canonical record is what every raster consumer is derived
            // from. Positions are render-relative, and both arms share the
            // camera, so the same world pose is the same record.
            for (sizet i = 0; i < rootRecords.size(); ++i)
            {
                SCOPED_TRACE(i);
                for (int c = 0; c < 3; ++c)
                {
                    EXPECT_NEAR(parentedRecords[i].PositionAndRange[c], rootRecords[i].PositionAndRange[c], 1.0e-3f)
                        << "the parented light's canonical record is not at its world position";
                    EXPECT_NEAR(parentedRecords[i].DirectionAndRadius[c], rootRecords[i].DirectionAndRadius[c], 1.0e-4f)
                        << "the parented spot's canonical record is not aimed along its world direction";
                }
                EXPECT_FLOAT_EQ(parentedRecords[i].PositionAndRange.w, rootRecords[i].PositionAndRange.w);
                EXPECT_EQ(parentedRecords[i].Type, rootRecords[i].Type);
                EXPECT_EQ(parentedRecords[i].Flags, rootRecords[i].Flags);
            }

            const FrameDiff twin = Diff(root, parented);
            const FrameDiff bug = Diff(root, misplaced);
            const FrameDiff shadows = Diff(root, shadowsOff);
            std::cout << "[parented-local-light] " << name << ": root vs parented mean " << twin.MeanAbs << ", "
                      << twin.PixelsOver8 << " px > 8, max " << twin.MaxDelta << "; root vs misplaced mean " << bug.MeanAbs
                      << ", " << bug.PixelsOver8 << " px > 8; root vs shadows off mean " << shadows.MeanAbs << ", "
                      << shadows.PixelsOver8 << " px > 8\n";

            // Sensitivity first: what the bug drew is far from the root frame,
            // and the shadows are a visible part of it.
            EXPECT_GT(bug.PixelsOver8, 2000u)
                << "the lights at their local coordinates draw nearly the root frame, so the comparison below "
                << "cannot tell a parented light that works from one that does not. See " << name << "_Misplaced.png";
            EXPECT_GT(shadows.PixelsOver8, 500u)
                << "the local lights' shadows are not visible in the frame, so a misplaced shadow request would pass. "
                << "See " << name << "_RootShadowsOff.png";

            // The parented arm matches the root arm to rounding: its world pose
            // comes out of a matrix product, not the authored floats.
            EXPECT_LT(twin.MeanAbs, 0.25) << "the parented lights light the scene differently from root lights at "
                                          << "the same world pose. See " << name << "_Parented.png";
            EXPECT_LT(twin.PixelsOver8, 40u) << "See " << name << "_Parented.png against " << name << "_Root.png";
        }
    }

    INSTANTIATE_TEST_SUITE_P(AllPaths, ParentedLocalLightEvidence,
                             ::testing::Values(RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred),
                             [](const ::testing::TestParamInfo<RenderingPath>& info)
                             { return std::string(PathName(info.param)); });
} // namespace OloEngine::Tests
