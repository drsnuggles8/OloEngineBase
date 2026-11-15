// OLO_TEST_LAYER: L8
// =============================================================================
// MultiViewRenderingTest.cpp — two real scene views through the renderer
// (issue #1352, ADR 0026).
//
// THE CLAIM. A scene can render a second view (Renderer3D::CreateView +
// Scene::RequestAdditionalView) in the same physical frame as its main view,
// and each view renders what a single-view renderer renders from that camera:
// nothing one view measures (its texture histories, its jitter, its exposure,
// its occlusion pyramid, its shadow maps, its light grid) is read or advanced
// by the other, and nothing the WORLD owns (object poses, the GPU Scene
// records, the stochastic frame index, the simulation clocks) advances twice.
//
// HOW IT IS HELD. The reference for a view is the ordinary single-view
// renderer: the main view alone, posed and sized as that view. The reference
// and the two-view run start from the same sampling sequences
// (Renderer3D::ResetFrameSequences) and render the same number of physical
// frames, so on a deterministic configuration the comparison is exact, texel
// for texel, in both orders (second view after the main one, and before it,
// where the second view is the frame's primary view and the main view the one
// that only links its draws). A difference names the state that crossed.
//
// Evidence: OloEditor/assets/tests/visual/MultiView_<Backend>_<Case>_<View>.png,
// one per view and reference. Runs in the --olo-rhi=vulkan process too.
//
// Classification: L8 (full pipeline through the real Scene render path,
// readback + PNG). SKIPs cleanly without a GPU context. The CPU half of the
// contract (history keys, the frame bookkeeping, the descriptor ring hold) is
// MultiViewContractTest.cpp.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "TestRenderBackend.h"

#include "OloEngine/Utils/PlatformUtils.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/RenderView.h"
#include "OloEngine/Renderer/TemporalHistoryRegistry.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/SceneCamera.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        struct Extent
        {
            u32 Width = 0;
            u32 Height = 0;
        };

        // Deliberately different sizes and aspect ratios: a target, a light
        // grid or a pyramid one view sized for the other shows as a stretched
        // or cropped frame, not as a subtle one.
        constexpr Extent kMainSize{ 256u, 256u };
        constexpr Extent kSecondSize{ 192u, 128u };
        constexpr f32 kFovDegrees = 55.0f;
        constexpr f32 kNearClip = 0.05f;
        constexpr f32 kFarClip = 200.0f;
        constexpr f32 kCaptureTime = 4.0f;
        // Frames from the sequence reset to the capture. Enough for every
        // one-frame-latent input (the retained pyramid, the shadow page marks,
        // the previous view-projection) to be the view's own.
        constexpr u32 kSettleFrames = 6u;
        // A frame that drew nothing holds a handful of colours (the clear colour,
        // a gradient); this scene holds a few hundred from either camera.
        constexpr u32 kMinDistinctColours = 64u;

        struct Pose
        {
            glm::vec3 Position{ 0.0f };
            glm::vec3 EulerRadians{ 0.0f };
        };

        // Two pose pairs: the views look at the same objects from different
        // sides in the first, and at mostly different objects in the second.
        struct PosePair
        {
            const char* Name;
            Pose Main;
            Pose Second;
        };
        const std::array<PosePair, 2> kPosePairs{
            PosePair{ "Front", Pose{ { 0.0f, 2.2f, 9.0f }, { glm::radians(-10.0f), 0.0f, 0.0f } },
                      Pose{ { 8.0f, 3.0f, 2.0f }, { glm::radians(-16.0f), glm::radians(72.0f), 0.0f } } },
            PosePair{ "Apart", Pose{ { -6.0f, 1.6f, 6.5f }, { glm::radians(-6.0f), glm::radians(-38.0f), 0.0f } },
                      Pose{ { 1.0f, 7.5f, -1.0f }, { glm::radians(-78.0f), glm::radians(15.0f), 0.0f } } },
        };

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

        [[nodiscard]] glm::mat4 PoseTransform(const Pose& pose)
        {
            return glm::translate(glm::mat4(1.0f), pose.Position) * glm::mat4_cast(glm::quat(pose.EulerRadians));
        }

        [[nodiscard]] glm::mat4 ProjectionFor(Extent size)
        {
            SceneCamera camera;
            camera.SetPerspective(glm::radians(kFovDegrees), kNearClip, kFarClip);
            camera.SetViewportSize(size.Width, size.Height);
            return camera.GetProjection();
        }

        struct Frame
        {
            std::vector<u8> Rgba;
            Extent Size;
        };

        // Rows arrive bottom-up; the PNG is written top-down.
        void WriteEvidence(const std::string& name, const Frame& frame)
        {
            if (frame.Rgba.empty())
                return;
            const std::filesystem::path dir = std::filesystem::path("assets") / "tests" / "visual";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            const std::string file = (dir / (name + ".png")).string();
            const sizet rowBytes = static_cast<sizet>(frame.Size.Width) * 4u;
            std::vector<u8> topDown(frame.Rgba.size());
            for (u32 y = 0; y < frame.Size.Height; ++y)
                std::copy_n(frame.Rgba.data() + static_cast<sizet>(frame.Size.Height - 1u - y) * rowBytes, rowBytes,
                            topDown.data() + static_cast<sizet>(y) * rowBytes);
            EXPECT_NE(::stbi_write_png(file.c_str(), static_cast<int>(frame.Size.Width),
                                       static_cast<int>(frame.Size.Height), 4, topDown.data(),
                                       static_cast<int>(rowBytes)),
                      0)
                << "could not write " << file;
        }

        struct Difference
        {
            u32 Pixels = 0;
            u32 MaxDelta = 0;
            bool SameShape = false;
        };

        [[nodiscard]] Difference Compare(const Frame& a, const Frame& b)
        {
            Difference difference;
            difference.SameShape = a.Size.Width == b.Size.Width && a.Size.Height == b.Size.Height &&
                                   a.Rgba.size() == b.Rgba.size() && !a.Rgba.empty();
            if (!difference.SameShape)
                return difference;
            for (sizet px = 0; px + 3 < a.Rgba.size(); px += 4)
            {
                u32 delta = 0;
                for (sizet c = 0; c < 3; ++c)
                    delta = std::max(delta, static_cast<u32>(std::abs(static_cast<int>(a.Rgba[px + c]) -
                                                                      static_cast<int>(b.Rgba[px + c]))));
                if (delta != 0u)
                    ++difference.Pixels;
                difference.MaxDelta = std::max(difference.MaxDelta, delta);
            }
            return difference;
        }

        // How many different colours a frame holds: a frame that drew nothing
        // compares equal to another that drew nothing.
        [[nodiscard]] u32 CountDistinctColours(const Frame& frame)
        {
            std::vector<u32> colours;
            colours.reserve(frame.Rgba.size() / 4u);
            for (sizet px = 0; px + 3 < frame.Rgba.size(); px += 4)
                colours.push_back((static_cast<u32>(frame.Rgba[px]) << 16u) | (static_cast<u32>(frame.Rgba[px + 1]) << 8u) |
                                  frame.Rgba[px + 2]);
            std::ranges::sort(colours);
            return static_cast<u32>(std::ranges::unique(colours).begin() - colours.begin());
        }

        // Pins the engine clock for a capture and puts back whatever was there.
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

        enum class Order : u8
        {
            SecondAfterMain,
            SecondBeforeMain,
        };

        [[nodiscard]] const char* OrderName(Order order)
        {
            return order == Order::SecondAfterMain ? "SecondAfterMain" : "SecondBeforeMain";
        }
    } // namespace

    // -------------------------------------------------------------------------
    // The fixture: one scene, a main view and (on demand) a second view.
    // -------------------------------------------------------------------------
    class MultiViewFixture : public RendererAttachedTest
    {
      protected:
        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return true;
        }

        void BuildScene() override
        {
            EnableRendering(kMainSize.Width, kMainSize.Height);

            auto& renderer = Renderer3D::GetRendererSettings();
            renderer.ShowGrid = false;
            auto& post = Renderer3D::GetPostProcessSettings();
            post.AutoExposureEnabled = false;
            post.Exposure = 1.0f;
            Renderer3D::ApplyRendererSettings();

            m_Cube = MeshPrimitives::CreateCube()->GetMeshSource();
            m_Plane = MeshPrimitives::CreatePlane(1.0f, 1.0f)->GetMeshSource();

            m_Camera = GetScene().CreateEntity("Camera");
            auto& camera = m_Camera.AddComponent<CameraComponent>();
            camera.Primary = true;
            camera.Camera.SetPerspective(glm::radians(kFovDegrees), kNearClip, kFarClip);
            camera.Camera.SetViewportSize(kMainSize.Width, kMainSize.Height);
            SetMainCamera(kPosePairs[0].Main);

            {
                Entity sun = GetScene().CreateEntity("Sun");
                auto& light = sun.AddComponent<DirectionalLightComponent>();
                light.m_Direction = glm::normalize(glm::vec3(-0.45f, -0.8f, -0.35f));
                light.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                light.m_Intensity = 2.2f;
                light.m_CastShadows = true;
            }
            {
                Entity lamp = GetScene().CreateEntity("Lamp");
                lamp.GetComponent<TransformComponent>().Translation = { -2.5f, 2.5f, 2.5f };
                auto& light = lamp.AddComponent<PointLightComponent>();
                light.m_Color = glm::vec3(0.3f, 0.6f, 1.0f);
                light.m_Intensity = 6.0f;
                light.m_Range = 9.0f;
            }
            {
                Entity ground = GetScene().CreateEntity("Ground");
                auto& transform = ground.GetComponent<TransformComponent>();
                transform.Translation = { 0.0f, 0.0f, 0.0f };
                transform.Scale = { 30.0f, 1.0f, 30.0f };
                auto& mesh = ground.AddComponent<MeshComponent>();
                mesh.m_Primitive = MeshPrimitive::Plane;
                mesh.m_MeshSource = m_Plane;
                ground.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor(glm::vec4(0.5f, 0.5f, 0.52f, 1.0f));
            }
            const std::array<glm::vec3, 5> positions{ glm::vec3(-3.0f, 0.75f, 0.0f), glm::vec3(0.0f, 0.75f, -1.5f),
                                                      glm::vec3(3.0f, 0.75f, 0.5f), glm::vec3(1.0f, 0.4f, 3.0f),
                                                      glm::vec3(-1.5f, 1.5f, -4.0f) };
            const std::array<glm::vec4, 5> colours{ glm::vec4(0.85f, 0.2f, 0.15f, 1.0f), glm::vec4(0.2f, 0.75f, 0.25f, 1.0f),
                                                    glm::vec4(0.2f, 0.3f, 0.9f, 1.0f), glm::vec4(0.9f, 0.8f, 0.2f, 1.0f),
                                                    glm::vec4(0.8f, 0.3f, 0.8f, 1.0f) };
            for (sizet i = 0; i < positions.size(); ++i)
            {
                Entity cube = GetScene().CreateEntity("Cube");
                auto& transform = cube.GetComponent<TransformComponent>();
                transform.Translation = positions[i];
                transform.Scale = glm::vec3(i == 3u ? 0.8f : 1.5f);
                transform.SetRotationEuler({ 0.0f, glm::radians(17.0f * static_cast<f32>(i)), 0.0f });
                auto& mesh = cube.AddComponent<MeshComponent>();
                mesh.m_Primitive = MeshPrimitive::Cube;
                mesh.m_MeshSource = m_Cube;
                cube.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor(colours[i]);
                if (i == 3u)
                    m_Mover = cube;
            }
        }

        void TearDown() override
        {
            // A view outlives no test: the next one starts with the main view
            // alone, and its settings snapshot is the main view's.
            DestroySecondView();
            RendererAttachedTest::TearDown();
        }

        void SetMainCamera(const Pose& pose)
        {
            auto& transform = m_Camera.GetComponent<TransformComponent>();
            transform.Translation = pose.Position;
            transform.SetRotationEuler(pose.EulerRadians);
        }

        // Size the main view and its camera. The reference for the second view
        // is the main view at the second view's size.
        void SizeMainView(Extent size)
        {
            ResizeRenderTarget(size.Width, size.Height);
            m_Camera.GetComponent<CameraComponent>().Camera.SetViewportSize(size.Width, size.Height);
        }

        void CreateSecondView(Extent size = kSecondSize)
        {
            ASSERT_TRUE(m_SecondView.IsMain()) << "the fixture holds one second view at a time";
            m_SecondView = Renderer3D::CreateView(
                RenderViewDesc{ .Width = size.Width, .Height = size.Height, .DebugName = "MultiViewTestSecond" });
            ASSERT_FALSE(m_SecondView.IsMain()) << "Renderer3D::CreateView refused";
            m_SecondSize = size;
        }

        void DestroySecondView()
        {
            if (m_SecondView.IsMain())
                return;
            if (Renderer3D::IsViewAlive(m_SecondView))
                Renderer3D::DestroyView(m_SecondView);
            m_SecondView = kMainRenderView;
        }

        // The same starting point for a reference and for the run compared
        // with it: every sampling sequence of every view restarted, and every
        // history of every view dropped with them.
        static void RestartSequences()
        {
            Renderer3D::ResetFrameSequences();
        }

        [[nodiscard]] Scene::SceneViewRequest SecondViewRequest(const Pose& pose, Order order) const
        {
            Scene::SceneViewRequest request;
            request.View = m_SecondView;
            request.CameraTransform = PoseTransform(pose);
            request.Projection = ProjectionFor(m_SecondSize);
            request.NearClip = kNearClip;
            request.FarClip = kFarClip;
            request.BeforeMainView = order == Order::SecondBeforeMain;
            return request;
        }

        // `frames` physical frames with both views in each.
        void RunSharedFrames(const Pose& secondPose, Order order, u32 frames)
        {
            for (u32 i = 0; i < frames; ++i)
            {
                GetScene().RequestAdditionalView(SecondViewRequest(secondPose, order));
                RunFrames(1);
            }
        }

        [[nodiscard]] Frame ReadMainView()
        {
            Frame frame;
            EXPECT_TRUE(ReadbackComposite(frame.Rgba, frame.Size.Width, frame.Size.Height)) << "no main-view frame to read back";
            return frame;
        }

        [[nodiscard]] Frame ReadSecondView()
        {
            Frame frame;
            const Renderer3D::ScopedView scope(m_SecondView);
            EXPECT_TRUE(ReadbackComposite(frame.Rgba, frame.Size.Width, frame.Size.Height))
                << "no second-view frame to read back";
            return frame;
        }

        // The single-view reference for a camera: the main view alone, at
        // `size`, `frames` physical frames after a sequence restart.
        [[nodiscard]] Frame RenderReference(const Pose& pose, Extent size, u32 frames = kSettleFrames)
        {
            SizeMainView(size);
            SetMainCamera(pose);
            RestartSequences();
            RunFrames(frames);
            return ReadMainView();
        }

        struct SharedFrames
        {
            Frame Main;
            Frame Second;
        };

        // Both views together, `frames` physical frames after a sequence restart.
        [[nodiscard]] SharedFrames RenderShared(const PosePair& poses, Order order, u32 frames = kSettleFrames)
        {
            SizeMainView(kMainSize);
            SetMainCamera(poses.Main);
            RestartSequences();
            RunSharedFrames(poses.Second, order, frames);
            SharedFrames shared;
            shared.Main = ReadMainView();
            shared.Second = ReadSecondView();
            return shared;
        }

        static void ExpectSameFrame(const Frame& reference, const Frame& shared, const std::string& what)
        {
            const Difference difference = Compare(reference, shared);
            EXPECT_TRUE(difference.SameShape) << what << ": the shared view is " << shared.Size.Width << "x"
                                              << shared.Size.Height << ", its single-view reference "
                                              << reference.Size.Width << "x" << reference.Size.Height;
            EXPECT_EQ(difference.Pixels, 0u) << what << ": " << difference.Pixels
                                             << " pixel(s) differ from the single-view reference, max delta "
                                             << difference.MaxDelta << "/255";
            std::cout << "[MultiView] " << what << ": " << difference.Pixels << " differing pixel(s), max delta "
                      << difference.MaxDelta << "/255\n";
        }

        Entity m_Camera;
        Entity m_Mover;
        Ref<MeshSource> m_Cube;
        Ref<MeshSource> m_Plane;
        RenderViewId m_SecondView{};
        Extent m_SecondSize = kSecondSize;
    };

    // -------------------------------------------------------------------------
    // Acceptance 1: two views match their isolated counterparts, on every path.
    // -------------------------------------------------------------------------
    class MultiViewRendering : public MultiViewFixture, public ::testing::WithParamInterface<RenderingPath>
    {
      protected:
        void BuildScene() override
        {
            MultiViewFixture::BuildScene();
            Renderer3D::GetRendererSettings().Path = GetParam();
            Renderer3D::ApplyRendererSettings();
        }
    };

    TEST_P(MultiViewRendering, EachOfTwoViewsRendersWhatASingleViewRendersFromItsCamera)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime mockTime(kCaptureTime);
        const std::string prefix = TestRenderBackend::EvidenceName("MultiView") + PathName(GetParam());

        for (const PosePair& poses : kPosePairs)
        {
            // The references first, with no second view alive at all.
            const Frame mainReference = RenderReference(poses.Main, kMainSize);
            const Frame secondReference = RenderReference(poses.Second, kSecondSize);
            WriteEvidence(prefix + "_" + poses.Name + "_MainAlone", mainReference);
            WriteEvidence(prefix + "_" + poses.Name + "_SecondAlone", secondReference);

            // The premise: both cameras see a real frame.
            ASSERT_GT(CountDistinctColours(mainReference), kMinDistinctColours) << poses.Name << ": the main reference is nearly flat";
            ASSERT_GT(CountDistinctColours(secondReference), kMinDistinctColours) << poses.Name << ": the second reference is nearly flat";

            CreateSecondView();
            if (HasFatalFailure())
                return;
            for (const Order order : { Order::SecondAfterMain, Order::SecondBeforeMain })
            {
                const SharedFrames shared = RenderShared(poses, order);
                const std::string label = std::string(PathName(GetParam())) + "/" + poses.Name + "/" + OrderName(order);
                WriteEvidence(prefix + "_" + poses.Name + "_" + OrderName(order) + "_Main", shared.Main);
                WriteEvidence(prefix + "_" + poses.Name + "_" + OrderName(order) + "_Second", shared.Second);
                ExpectSameFrame(mainReference, shared.Main, label + " main view");
                ExpectSameFrame(secondReference, shared.Second, label + " second view");
            }
            DestroySecondView();
        }
    }

    INSTANTIATE_TEST_SUITE_P(AllPaths, MultiViewRendering,
                             ::testing::Values(RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred),
                             [](const ::testing::TestParamInfo<RenderingPath>& info)
                             { return std::string(PathName(info.param)); });

    // -------------------------------------------------------------------------
    // Acceptance 3: the scene advances once per physical frame.
    // -------------------------------------------------------------------------
    class MultiViewSceneAdvance : public MultiViewFixture
    {
    };

    TEST_F(MultiViewSceneAdvance, TwoViewsAdvanceTheSceneOncePerPhysicalFrame)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime mockTime(kCaptureTime);
        constexpr u32 kFrames = 5u;

        CreateSecondView();
        if (HasFatalFailure())
            return;
        RunSharedFrames(kPosePairs[0].Second, Order::SecondAfterMain, 2u); // past both views' first frame

        for (const Order order : { Order::SecondAfterMain, Order::SecondBeforeMain })
        {
            const Renderer3D::SceneAdvanceCounters before = Renderer3D::GetSceneAdvanceCounters();
            const u32 stochasticBefore = Renderer3D::GetStochasticFrameIndex();
            RunSharedFrames(kPosePairs[0].Second, order, kFrames);
            const Renderer3D::SceneAdvanceCounters after = Renderer3D::GetSceneAdvanceCounters();

            EXPECT_EQ(after.PhysicalFrames - before.PhysicalFrames, kFrames) << OrderName(order);
            EXPECT_EQ(after.ViewFrames - before.ViewFrames, 2u * kFrames)
                << OrderName(order) << ": both views render every frame";
            EXPECT_EQ(after.PoseHistoryRotations - before.PoseHistoryRotations, kFrames)
                << OrderName(order) << ": object poses move to 'previous' once per frame, not once per view";
            EXPECT_EQ(after.SceneExtractions - before.SceneExtractions, kFrames)
                << OrderName(order) << ": the GPU Scene is extracted and committed once per frame";
            EXPECT_EQ(after.WorldSimulationSteps - before.WorldSimulationSteps, kFrames)
                << OrderName(order) << ": wind, snow, wake, precipitation and the cloud and fog clocks step once";
            EXPECT_EQ(after.StochasticIndexSteps - before.StochasticIndexSteps, kFrames) << OrderName(order);
            EXPECT_EQ(Renderer3D::GetStochasticFrameIndex() - stochasticBefore, kFrames)
                << OrderName(order) << ": every view of a frame samples the same stochastic index";

            const RenderViewInfo main = Renderer3D::GetViewInfo(kMainRenderView);
            const RenderViewInfo second = Renderer3D::GetViewInfo(m_SecondView);
            EXPECT_EQ(main.LastRenderedFrameSerial, second.LastRenderedFrameSerial)
                << OrderName(order) << ": both views rendered in the same physical frame";
            EXPECT_EQ(main.WasPrimary, order == Order::SecondAfterMain) << OrderName(order);
            EXPECT_EQ(second.WasPrimary, order == Order::SecondBeforeMain) << OrderName(order);
        }

        // The single-view control: one view frame per physical frame.
        const Renderer3D::SceneAdvanceCounters before = Renderer3D::GetSceneAdvanceCounters();
        RunFrames(kFrames);
        const Renderer3D::SceneAdvanceCounters after = Renderer3D::GetSceneAdvanceCounters();
        EXPECT_EQ(after.PhysicalFrames - before.PhysicalFrames, kFrames);
        EXPECT_EQ(after.ViewFrames - before.ViewFrames, kFrames);
        EXPECT_EQ(after.SceneExtractions - before.SceneExtractions, kFrames);
    }

    // The history registry of a view's graph is keyed by that view.
    TEST_F(MultiViewSceneAdvance, AViewsHistoriesAreKeyedByItsOwnId)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime mockTime(kCaptureTime);

        Renderer3D::GetPostProcessSettings().TAAEnabled = true; // a history every path acquires
        CreateSecondView();
        if (HasFatalFailure())
            return;
        RunSharedFrames(kPosePairs[0].Second, Order::SecondAfterMain, 4u);

        const auto viewKeysOf = [](RenderViewId view)
        {
            const Renderer3D::ScopedView scope(view);
            std::vector<u64> keys;
            if (const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph())
            {
                EXPECT_EQ(graph->GetTemporalHistoryRegistry().GetBoundView().value_or(~0ull), view.Value);
                for (const TemporalHistorySnapshot& history : graph->GetTemporalHistoryRegistry().Snapshot())
                    keys.push_back(history.Key.View);
            }
            return keys;
        };
        const std::vector<u64> mainKeys = viewKeysOf(kMainRenderView);
        const std::vector<u64> secondKeys = viewKeysOf(m_SecondView);
        ASSERT_FALSE(mainKeys.empty()) << "TAA acquired no history in the main view";
        ASSERT_FALSE(secondKeys.empty()) << "TAA acquired no history in the second view";
        EXPECT_TRUE(std::ranges::all_of(mainKeys, [](u64 key)
                                        { return key == 0u; }));
        EXPECT_TRUE(std::ranges::all_of(secondKeys, [this](u64 key)
                                        { return key == m_SecondView.Value; }));
        EXPECT_NE(m_SecondView.Value, 0u);
    }
} // namespace OloEngine::Tests
