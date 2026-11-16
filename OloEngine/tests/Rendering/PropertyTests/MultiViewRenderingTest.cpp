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
// renderer: the main view alone, posed, sized and configured as that view. The
// reference and the two-view run start from the same sampling sequences
// (Renderer3D::ResetFrameSequences) and render the same number of physical
// frames, so the comparison is exact, texel for texel, in both orders (second
// view after the main one, and before it, where the second view is the frame's
// primary view and the main view the one that only links its draws). A
// difference names the state that crossed.
//
// Two negative controls re-create the coupling a renderer without views has
// (DebugLevers.inl, Fault injection) and must make the comparison fail.
//
// Evidence: OloEditor/assets/tests/visual/MultiView_<Backend>_<Case>_<View>.png,
// one per view and reference. Runs in the --olo-rhi=vulkan process too.
//
// Classification: L8 (full pipeline through the real Scene render path,
// readback + PNG). SKIPs cleanly without a GPU context. The CPU half of the
// contract is TemporalHistoryRegistryTest.cpp (history keys bound to a view)
// and RHIDescriptorHeapTest.cpp (the descriptor ring hold).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "TestRenderBackend.h"

#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/ReflectionProbeBaker.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "OloEngine/Renderer/RayTracing/RayTracingScene.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/RenderView.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Renderer/Shadow/ShadowTechnique.h"
#include "OloEngine/Renderer/TemporalHistoryRegistry.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/SceneCamera.h"
#include "OloEngine/Terrain/Foliage/FoliageRenderer.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <tuple>
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
        constexpr Extent kSecondResized{ 160u, 96u };
        constexpr f32 kFovDegrees = 55.0f;
        constexpr f32 kNearClip = 0.05f;
        constexpr f32 kFarClip = 200.0f;
        constexpr f32 kCaptureTime = 4.0f;
        constexpr f32 kFrameSeconds = 1.0f / 60.0f;
        // Frames from the sequence reset to the capture. Enough for every
        // one-frame-latent input (the retained pyramid, the shadow page marks,
        // the previous view-projection) to be the view's own.
        constexpr u32 kSettleFrames = 6u;
        // Frames rendered after a view is sized and configured and BEFORE the
        // sequence restart a comparison starts from. A resolution or upscale
        // change takes effect over the next frames (the scene band is resized
        // when the graph next compiles), and a restart does not undo that: a
        // run measured inside the transition differs from the same run started
        // a few frames later, with one view and no second view anywhere. Both
        // arms of every comparison therefore start from a settled configuration.
        constexpr u32 kConfigureSettleFrames = 3u;
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

        // How far a shared view may be from its single-view reference. Exact
        // by default. A bound is given only where the SAME single view rendered
        // twice already differs, and is sized from that measurement.
        struct Tolerance
        {
            u32 Pixels = 0;
            u32 MaxDelta = 0;
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

        [[nodiscard]] f64 MeanLuma(const Frame& frame)
        {
            f64 sum = 0.0;
            sizet count = 0;
            for (sizet px = 0; px + 3 < frame.Rgba.size(); px += 4, ++count)
                sum += 0.2126 * frame.Rgba[px] + 0.7152 * frame.Rgba[px + 1] + 0.0722 * frame.Rgba[px + 2];
            return count == 0u ? 0.0 : sum / static_cast<f64>(count);
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

        // A fault lever on for a scope (DebugLevers.inl, Fault injection).
        struct ScopedFault
        {
            using Getter = bool (*)();
            using Setter = void (*)(bool);
            ScopedFault(Getter get, Setter set)
                : m_Set(set), m_Previous(get())
            {
                m_Set(true);
            }
            ~ScopedFault()
            {
                m_Set(m_Previous);
            }
            ScopedFault(const ScopedFault&) = delete;
            ScopedFault& operator=(const ScopedFault&) = delete;

          private:
            Setter m_Set;
            bool m_Previous;
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

        // Edits the ACTIVE view's settings.
        using Configure = std::function<void()>;
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
            // What every view starts from; a test's Configure edits a copy.
            m_BaseRenderer = renderer;
            m_BasePost = post;
            m_BaseShadow = Renderer3D::GetShadowMap().GetSettings();

            m_Cube = MeshPrimitives::CreateCube()->GetMeshSource();
            m_Plane = MeshPrimitives::CreatePlane(1.0f, 1.0f)->GetMeshSource();

            m_Camera = GetScene().CreateEntity("Camera");
            auto& camera = m_Camera.AddComponent<CameraComponent>();
            camera.Primary = true;
            camera.Camera.SetPerspective(glm::radians(kFovDegrees), kNearClip, kFarClip);
            camera.Camera.SetViewportSize(kMainSize.Width, kMainSize.Height);
            SetMainCamera(kPosePairs[0].Main);

            {
                m_Sun = GetScene().CreateEntity("Sun");
                auto& light = m_Sun.AddComponent<DirectionalLightComponent>();
                light.m_Direction = glm::normalize(glm::vec3(-0.45f, -0.8f, -0.35f));
                light.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                light.m_Intensity = kSunIntensity;
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
                                                      glm::vec3(3.0f, 0.75f, 0.5f), kMoverStart,
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

        void ResizeSecondView(Extent size)
        {
            const Renderer3D::ScopedView scope(m_SecondView);
            Renderer3D::OnWindowResize(size.Width, size.Height);
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

        // The base settings plus `configure`, on the active view.
        void ConfigureActiveView(const Configure& configure) const
        {
            Renderer3D::GetRendererSettings() = m_BaseRenderer;
            Renderer3D::GetPostProcessSettings() = m_BasePost;
            Renderer3D::GetShadowMap().SetSettings(m_BaseShadow);
            if (configure)
                configure();
            Renderer3D::ApplyRendererSettings();
        }

        void ConfigureSecondView(const Configure& configure) const
        {
            const Renderer3D::ScopedView scope(m_SecondView);
            ConfigureActiveView(configure);
        }

        // Render past the transition a size or settings change starts (see
        // kConfigureSettleFrames), with the world as it is at frame 0 of the
        // run that follows.
        void SettleConfiguration(const std::function<void()>& renderOneFrame)
        {
            if (m_BeforeFrame)
                m_BeforeFrame(0u);
            for (u32 i = 0; i < kConfigureSettleFrames; ++i)
                renderOneFrame();
        }

        // The same starting point for a reference and for the run compared
        // with it: every sampling sequence of every view restarted, every
        // history of every view dropped with them, and the frame counter the
        // per-frame hook reads back at zero.
        void RestartSequences()
        {
            Renderer3D::ResetFrameSequences();
            m_FrameIndex = 0;
        }

        // The configuration this binary was built in, for a reported timing.
        [[nodiscard]] static const char* BuildConfigurationName()
        {
#if defined(OLO_DEBUG)
            return "Debug";
#elif defined(OLO_RELEASE)
            return "Release";
#else
            return "Dist";
#endif
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

        // `frames` physical frames of the main view alone.
        void RunMainFrames(u32 frames)
        {
            for (u32 i = 0; i < frames; ++i, ++m_FrameIndex)
            {
                if (m_BeforeFrame)
                    m_BeforeFrame(m_FrameIndex);
                RunFrames(1);
            }
        }

        // `frames` physical frames with both views in each.
        void RunSharedFrames(const Pose& secondPose, Order order, u32 frames)
        {
            for (u32 i = 0; i < frames; ++i, ++m_FrameIndex)
            {
                if (m_BeforeFrame)
                    m_BeforeFrame(m_FrameIndex);
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
        // `size`, configured by `configure`, `frames` physical frames after a
        // sequence restart.
        [[nodiscard]] Frame RenderReference(const Pose& pose, Extent size, u32 frames = kSettleFrames,
                                            const Configure& configure = {})
        {
            SizeMainView(size);
            SetMainCamera(pose);
            ConfigureActiveView(configure);
            SettleConfiguration([this]()
                                { RunFrames(1); });
            RestartSequences();
            RunMainFrames(frames);
            return ReadMainView();
        }

        struct SharedFrames
        {
            Frame Main;
            Frame Second;
        };

        // Both views together, `frames` physical frames after a sequence restart.
        [[nodiscard]] SharedFrames RenderShared(const PosePair& poses, Order order, u32 frames = kSettleFrames,
                                                const Configure& configureMain = {}, const Configure& configureSecond = {})
        {
            SizeMainView(kMainSize);
            SetMainCamera(poses.Main);
            ConfigureActiveView(configureMain);
            ConfigureSecondView(configureSecond);
            SettleConfiguration([this, &poses, order]()
                                {
                                    GetScene().RequestAdditionalView(SecondViewRequest(poses.Second, order));
                                    RunFrames(1); });
            RestartSequences();
            RunSharedFrames(poses.Second, order, frames);
            SharedFrames shared;
            shared.Main = ReadMainView();
            shared.Second = ReadSecondView();
            return shared;
        }

        static Difference ExpectSameFrame(const Frame& reference, const Frame& shared, const std::string& what,
                                          Tolerance tolerance = {})
        {
            const Difference difference = Compare(reference, shared);
            EXPECT_TRUE(difference.SameShape) << what << ": the shared view is " << shared.Size.Width << "x"
                                              << shared.Size.Height << ", its single-view reference "
                                              << reference.Size.Width << "x" << reference.Size.Height;
            EXPECT_LE(difference.Pixels, tolerance.Pixels) << what << ": " << difference.Pixels
                                                           << " pixel(s) differ from the single-view reference, max delta "
                                                           << difference.MaxDelta << "/255";
            EXPECT_LE(difference.MaxDelta, tolerance.MaxDelta) << what << ": a pixel differs by " << difference.MaxDelta << "/255";
            std::cout << "[MultiView] " << what << ": " << difference.Pixels << " differing pixel(s), max delta "
                      << difference.MaxDelta << "/255\n";
            return difference;
        }

        // The whole comparison for one configuration: references for both
        // cameras with no second view alive, then both orders of the two-view
        // frame against them. `evidence` names the PNGs.
        void ExpectBothViewsMatchTheirReferences(const std::string& evidence, const std::string& label,
                                                 const PosePair& poses, u32 frames, const Configure& configureMain,
                                                 const Configure& configureSecond, Tolerance tolerance = {})
        {
            const Frame mainReference = RenderReference(poses.Main, kMainSize, frames, configureMain);
            const Frame secondReference = RenderReference(poses.Second, kSecondSize, frames, configureSecond);
            WriteEvidence(evidence + "_MainAlone", mainReference);
            WriteEvidence(evidence + "_SecondAlone", secondReference);
            // The premise: both cameras see a real frame.
            ASSERT_GT(CountDistinctColours(mainReference), kMinDistinctColours) << label << ": the main reference is nearly flat";
            ASSERT_GT(CountDistinctColours(secondReference), kMinDistinctColours)
                << label << ": the second reference is nearly flat";

            CreateSecondView();
            if (HasFatalFailure())
                return;
            for (const Order order : { Order::SecondAfterMain, Order::SecondBeforeMain })
            {
                const SharedFrames shared = RenderShared(poses, order, frames, configureMain, configureSecond);
                const std::string where = label + "/" + OrderName(order);
                WriteEvidence(evidence + "_" + OrderName(order) + "_Main", shared.Main);
                WriteEvidence(evidence + "_" + OrderName(order) + "_Second", shared.Second);
                ExpectSameFrame(mainReference, shared.Main, where + " main view", tolerance);
                ExpectSameFrame(secondReference, shared.Second, where + " second view", tolerance);
            }
            DestroySecondView();
        }

        // The mover walks along +X by a fixed step per frame, as a function of
        // the frame index, so a reference and the run compared with it move it
        // identically.
        void MoveMoverEachFrame()
        {
            m_BeforeFrame = [this](u32 frame)
            {
                m_Mover.GetComponent<TransformComponent>().Translation =
                    kMoverStart + glm::vec3(0.12f * static_cast<f32>(frame), 0.0f, 0.0f);
            };
        }

        // The mock clock steps one nominal frame per physical frame, so
        // everything paced by time (eye adaptation) advances identically in a
        // reference and in the run compared with it.
        void StepClockEachFrame()
        {
            m_BeforeFrame = [](u32 frame)
            { Time::SetMockTime(kCaptureTime + kFrameSeconds * static_cast<f32>(frame)); };
        }

        // The clock steps as above and the sun brightens fourfold at
        // `brightenAt`: from there the eye has something to adapt to, at the
        // pace of each view's own time step.
        void StepClockAndBrightenTheSunAt(u32 brightenAt)
        {
            m_BeforeFrame = [this, brightenAt](u32 frame)
            {
                Time::SetMockTime(kCaptureTime + kFrameSeconds * static_cast<f32>(frame));
                m_Sun.GetComponent<DirectionalLightComponent>().m_Intensity = frame < brightenAt ? kSunIntensity
                                                                                                 : 4.0f * kSunIntensity;
            };
        }

        // A view's retained state must be the same OBJECTS, `frames`
        // frames older, as `before`.
        static void ExpectRetained(const Renderer3D::ViewRetainedState& before, const Renderer3D::ViewRetainedState& after,
                                   u32 frames, const std::string& what)
        {
            ASSERT_TRUE(after.Alive) << what;
            EXPECT_TRUE(after.OcclusionPyramidValid) << what << ": the surviving view's occlusion pyramid was invalidated";
            EXPECT_EQ(after.OcclusionPyramidIdentity, before.OcclusionPyramidIdentity)
                << what << ": the surviving view's occlusion pyramid was recreated";
            EXPECT_EQ(after.ShadowCascadeIdentity, before.ShadowCascadeIdentity)
                << what << ": the surviving view's shadow cascades were recreated";
            EXPECT_EQ(after.ShadowAtlasIdentity, before.ShadowAtlasIdentity)
                << what << ": the surviving view's shadow atlas was recreated";
            ASSERT_EQ(after.Histories.Num(), before.Histories.Num()) << what << ": the surviving view's history set changed";
            u32 checked = 0;
            for (const Renderer3D::ViewRetainedHistory& was : before.Histories)
            {
                // A history registered by an effect that never produced a frame
                // (the cloudscape's, with no cloudscape in the scene) holds
                // nothing to retain.
                if (!was.Valid)
                    continue;
                ++checked;
                const Renderer3D::ViewRetainedHistory* now =
                    after.Histories.FindByPredicate([&was](const Renderer3D::ViewRetainedHistory& history)
                                                    { return history.Key == was.Key; });
                ASSERT_NE(now, nullptr) << what << ": a surviving-view history is gone";
                EXPECT_TRUE(now->Valid) << what << ": a surviving-view history was invalidated";
                EXPECT_EQ(now->TextureIdentity, was.TextureIdentity) << what << ": a surviving-view history texture was recreated";
                EXPECT_EQ(now->Age, was.Age + frames) << what << ": a surviving-view history lineage was restarted (age "
                                                      << was.Age << " -> " << now->Age << " over " << frames << " frame(s))";
            }
            EXPECT_GT(checked, 0u) << what << ": the surviving view held no valid history to retain";
        }

        static inline const glm::vec3 kMoverStart{ 1.0f, 0.4f, 3.0f };

        static constexpr f32 kSunIntensity = 2.2f;

        Entity m_Camera;
        Entity m_Sun;
        Entity m_Mover;
        Ref<MeshSource> m_Cube;
        Ref<MeshSource> m_Plane;
        RendererSettings m_BaseRenderer{};
        PostProcessSettings m_BasePost{};
        ShadowSettings m_BaseShadow{};
        RenderViewId m_SecondView{};
        Extent m_SecondSize = kSecondSize;
        std::function<void(u32)> m_BeforeFrame;
        u32 m_FrameIndex = 0;
    };

    // -------------------------------------------------------------------------
    // Acceptance 1: two views match their isolated counterparts, on every path.
    // -------------------------------------------------------------------------
    class MultiViewRendering : public MultiViewFixture, public ::testing::WithParamInterface<RenderingPath>
    {
    };

    TEST_P(MultiViewRendering, EachOfTwoViewsRendersWhatASingleViewRendersFromItsCamera)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime mockTime(kCaptureTime);
        const RenderingPath path = GetParam();
        const Configure onPath = [path]()
        { Renderer3D::GetRendererSettings().Path = path; };

        for (const PosePair& poses : kPosePairs)
        {
            ExpectBothViewsMatchTheirReferences(TestRenderBackend::EvidenceName("MultiView") + PathName(path) + "_" + poses.Name,
                                                std::string(PathName(path)) + "/" + poses.Name, poses, kSettleFrames,
                                                onPath, onPath);
            if (HasFatalFailure())
                return;
        }
    }

    // Temporal accumulation in one view and none in the other: the resolved
    // view jitters and accumulates on its own sequence, the native one does
    // not jitter at all. Shared jitter indices or histories would move both.
    TEST_P(MultiViewRendering, ATemporallyResolvedViewAndANativeViewKeepTheirOwnHistories)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime mockTime(kCaptureTime);
        const RenderingPath path = GetParam();
        const Configure resolved = [path]()
        {
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::GetPostProcessSettings().TAAEnabled = true;
            Renderer3D::GetPostProcessSettings().Upscale = UpscaleMode::Quality;
        };
        const Configure native = [path]()
        { Renderer3D::GetRendererSettings().Path = path; };

        // Twelve frames: past one lap of the eight-phase jitter sequence.
        ExpectBothViewsMatchTheirReferences(TestRenderBackend::EvidenceName("MultiViewTemporal") + PathName(path),
                                            std::string(PathName(path)) + "/temporal-main", kPosePairs[0], 12u, resolved,
                                            native);
        if (HasFatalFailure())
            return;
        // And the other way round: the second view is the resolved one.
        ExpectBothViewsMatchTheirReferences(TestRenderBackend::EvidenceName("MultiViewTemporalSecond") + PathName(path),
                                            std::string(PathName(path)) + "/temporal-second", kPosePairs[1], 12u, native,
                                            resolved);
    }

    INSTANTIATE_TEST_SUITE_P(AllPaths, MultiViewRendering,
                             ::testing::Values(RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred),
                             [](const ::testing::TestParamInfo<RenderingPath>& info)
                             { return std::string(PathName(info.param)); });

    // -------------------------------------------------------------------------
    // Distinct configurations per view.
    // -------------------------------------------------------------------------
    class MultiViewConfigurations : public MultiViewFixture
    {
    };

    // A different rendering path and sample count in each view: the main view
    // shades deferred with a 4x G-Buffer, the second forward+ single-sample.
    TEST_F(MultiViewConfigurations, PathAndSampleCountDifferPerView)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime mockTime(kCaptureTime);
        const Configure deferredMsaa = []()
        {
            Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
            Renderer3D::GetRendererSettings().Deferred.MSAASampleCount = 4u;
        };
        const Configure forwardPlus = []()
        { Renderer3D::GetRendererSettings().Path = RenderingPath::ForwardPlus; };

        ExpectBothViewsMatchTheirReferences(TestRenderBackend::EvidenceName("MultiViewMsaa") + "DeferredMsaa4_ForwardPlus",
                                            "deferred-msaa4 + forward+", kPosePairs[0], kSettleFrames, deferredMsaa,
                                            forwardPlus);
        if (HasFatalFailure())
            return;
        ExpectBothViewsMatchTheirReferences(TestRenderBackend::EvidenceName("MultiViewMsaa") + "Forward_DeferredMsaa4", "forward + deferred-msaa4", kPosePairs[1], kSettleFrames, []()
                                            { Renderer3D::GetRendererSettings().Path = RenderingPath::Forward; }, deferredMsaa);
    }

    // The temporal upscaler (FSR2) in one view and native rendering in the
    // other. Each view's upscaler context, jitter phase and lock-decay time
    // step are its own. Where no temporal upscaler exists (Vulkan, a build
    // without FSR2) the request resolves to the spatial upscaler, which the
    // per-path test above already covers, so the case says so and stops.
    TEST_F(MultiViewConfigurations, ATemporallyUpscaledViewAndANativeViewKeepTheirOwnUpscalerState)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        if (TestRenderBackend::IsVulkanProcess() || !TemporalUpscalerUsable())
            GTEST_SKIP() << "no temporal upscaler in this process: Upscale + Technique::Temporal resolves to the "
                            "spatial upscaler, which AllPaths/MultiViewRendering.ATemporallyResolvedView... covers";
        const ScopedMockTime mockTime(kCaptureTime);
        const Configure upscaled = []()
        {
            Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
            Renderer3D::GetPostProcessSettings().Upscale = UpscaleMode::Quality;
            Renderer3D::GetPostProcessSettings().Technique = UpscalerTechnique::Temporal;
        };
        const Configure native = []()
        { Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred; };

        // The premise: the temporal upscaler is what reconstructs the frame.
        const Frame temporal = RenderReference(kPosePairs[0].Main, kMainSize, 12u, upscaled);
        const Frame plain = RenderReference(kPosePairs[0].Main, kMainSize, 12u, native);
        ASSERT_GT(Compare(temporal, plain).Pixels, 500u) << "the temporally upscaled frame is the native frame";

        ExpectBothViewsMatchTheirReferences(TestRenderBackend::EvidenceName("MultiViewTemporalUpscale") + "Main",
                                            "fsr2 main + native second", kPosePairs[0], 12u, upscaled, native);
        if (HasFatalFailure())
            return;
        ExpectBothViewsMatchTheirReferences(TestRenderBackend::EvidenceName("MultiViewTemporalUpscale") + "Both",
                                            "fsr2 in both views", kPosePairs[1], 12u, upscaled, upscaled);
    }

    // Eye adaptation is each view's own: its state is the view's tone-map
    // pass's and its time step the view's clock's. The second view looks
    // almost straight down at lit ground from above, the main view across the
    // scene at a dark horizon, so they meter different luminances.
    TEST_F(MultiViewConfigurations, EachViewAdaptsItsExposureToWhatItSees)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime mockTime(kCaptureTime);
        constexpr u32 kBrightenAt = 8u;
        // Twelve frames (0.2 s) after the sun brightened: the eye is part of
        // the way to its new exposure.
        constexpr u32 kMidAdaptation = kBrightenAt + 12u;
        StepClockAndBrightenTheSunAt(kBrightenAt);
        const Configure adapting = []()
        {
            auto& post = Renderer3D::GetPostProcessSettings();
            post.AutoExposureEnabled = true;
            post.AutoExposureSpeedUp = 1.5f;
            post.AutoExposureSpeedDown = 1.5f;
        };

        // The premise: the capture is taken while the eye is still adapting.
        // The same camera much later must differ from it, or a view whose
        // exposure never stepped (a zero time step) would pass.
        const Frame mid = RenderReference(kPosePairs[1].Second, kSecondSize, kMidAdaptation, adapting);
        const Frame settled = RenderReference(kPosePairs[1].Second, kSecondSize, kMidAdaptation + 150u, adapting);
        const u32 stillAdapting = Compare(mid, settled).Pixels;
        std::cout << "[MultiView] eye adaptation still moves " << stillAdapting << " pixel(s) at frame " << kMidAdaptation
                  << " (mean luma " << MeanLuma(mid) << " -> " << MeanLuma(settled) << ")\n";
        ASSERT_GT(stillAdapting, (kSecondSize.Width * kSecondSize.Height) / 4u)
            << "the second camera's exposure is not adapting at frame " << kMidAdaptation;

        ExpectBothViewsMatchTheirReferences(TestRenderBackend::EvidenceName("MultiViewExposure") + "Adapting",
                                            "auto-exposure", kPosePairs[1], kMidAdaptation, adapting, adapting);
    }

    // -------------------------------------------------------------------------
    // Acceptance 2: cut, resize or destroy one view; the other keeps its
    // exposure, histories, occlusion pyramid and resource identities.
    // -------------------------------------------------------------------------
    class MultiViewLifetime : public MultiViewFixture
    {
    };

    // A ScopedView puts the previous view back when it closes, so that view
    // has to be there. With scopes nested for A then B, A is not the active
    // view and DestroyView once took it: B's scope then had nothing to
    // activate and left B active under a caller that had asked for A.
    TEST_F(MultiViewLifetime, AViewAnOpenScopeReturnsToCannotBeDestroyed)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        CreateSecondView();
        if (HasFatalFailure())
            return;
        const RenderViewId third =
            Renderer3D::CreateView(RenderViewDesc{ .Width = 64u, .Height = 64u, .DebugName = "MultiViewTestThird" });
        ASSERT_FALSE(third.IsMain()) << "Renderer3D::CreateView refused";

        EXPECT_TRUE(Renderer3D::CanDestroyView(m_SecondView));
        {
            const Renderer3D::ScopedView outer(m_SecondView);
            EXPECT_FALSE(Renderer3D::CanDestroyView(m_SecondView)) << "the active view";
            {
                const Renderer3D::ScopedView inner(third);
                EXPECT_EQ(Renderer3D::GetActiveView(), third);
                EXPECT_FALSE(Renderer3D::CanDestroyView(m_SecondView))
                    << "the inner scope returns to this view when it closes";
                EXPECT_FALSE(Renderer3D::CanDestroyView(third)) << "the active view";
            }
            EXPECT_EQ(Renderer3D::GetActiveView(), m_SecondView) << "the inner scope did not put its view back";
            EXPECT_TRUE(Renderer3D::CanDestroyView(third)) << "nothing returns to the third view any more";
        }
        EXPECT_EQ(Renderer3D::GetActiveView(), kMainRenderView);
        EXPECT_TRUE(Renderer3D::CanDestroyView(m_SecondView)) << "the hold outlived its scope";
        EXPECT_FALSE(Renderer3D::CanDestroyView(kMainRenderView));

        Renderer3D::DestroyView(third);
        EXPECT_FALSE(Renderer3D::IsViewAlive(third));
        EXPECT_FALSE(Renderer3D::CanDestroyView(third)) << "a dead view";
    }

    TEST_F(MultiViewLifetime, ResizingCuttingOrDestroyingOneViewLeavesTheOtherIntact)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime mockTime(kCaptureTime);
        StepClockEachFrame();
        // Everything acceptance 2 names: histories (TAA), exposure (eye
        // adaptation), the culling pyramid (Hi-Z occlusion).
        const Configure retained = []()
        {
            Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
            Renderer3D::GetRendererSettings().HZBOcclusionCullingEnabled = true;
            auto& post = Renderer3D::GetPostProcessSettings();
            post.TAAEnabled = true;
            post.AutoExposureEnabled = true;
            post.AutoExposureSpeedUp = 3.0f;
            post.AutoExposureSpeedDown = 3.0f;
        };
        constexpr u32 kWarm = 6u;
        constexpr u32 kAfterResize = 4u;
        constexpr u32 kAfterCut = 3u;
        constexpr u32 kAfterDestroy = 3u;
        const PosePair& poses = kPosePairs[0];

        // The reference: the main view alone for the same total frame count.
        const Frame reference =
            RenderReference(poses.Main, kMainSize, kWarm + kAfterResize + kAfterCut + kAfterDestroy, retained);
        WriteEvidence(TestRenderBackend::EvidenceName("MultiViewLifetime") + "MainAlone", reference);

        CreateSecondView();
        if (HasFatalFailure())
            return;
        SizeMainView(kMainSize);
        SetMainCamera(poses.Main);
        ConfigureActiveView(retained);
        ConfigureSecondView(retained);
        SettleConfiguration([this, &poses]()
                            {
                                GetScene().RequestAdditionalView(SecondViewRequest(poses.Second, Order::SecondAfterMain));
                                RunFrames(1); });
        RestartSequences();
        RunSharedFrames(poses.Second, Order::SecondAfterMain, kWarm);

        const Renderer3D::ViewRetainedState warm = Renderer3D::GetViewRetainedState(kMainRenderView);
        ASSERT_TRUE(warm.Histories.ContainsByPredicate([](const Renderer3D::ViewRetainedHistory& history)
                                                       { return history.Valid && history.Age == kWarm; }))
            << "the main view holds no history " << kWarm << " frames old to retain";
        ASSERT_TRUE(warm.OcclusionPyramidValid) << "the main view built no occlusion pyramid to retain";
        ASSERT_NE(warm.ShadowCascadeIdentity, 0u);
        const Renderer3D::ViewRetainedState secondWarm = Renderer3D::GetViewRetainedState(m_SecondView);
        ASSERT_FALSE(secondWarm.Histories.IsEmpty()) << "the second view holds no history";
        EXPECT_NE(secondWarm.OcclusionPyramidIdentity, warm.OcclusionPyramidIdentity)
            << "both views name one occlusion pyramid";
        EXPECT_NE(secondWarm.ShadowCascadeIdentity, warm.ShadowCascadeIdentity) << "both views name one cascade array";

        // --- Resize the second view ------------------------------------------
        ResizeSecondView(kSecondResized);
        RunSharedFrames(poses.Second, Order::SecondAfterMain, kAfterResize);
        const Renderer3D::ViewRetainedState afterResize = Renderer3D::GetViewRetainedState(kMainRenderView);
        ExpectRetained(warm, afterResize, kAfterResize, "after resizing the second view");
        const Frame resizedSecond = ReadSecondView();
        EXPECT_EQ(resizedSecond.Size.Width, kSecondResized.Width);
        EXPECT_EQ(resizedSecond.Size.Height, kSecondResized.Height);
        WriteEvidence(TestRenderBackend::EvidenceName("MultiViewLifetime") + "SecondResized", resizedSecond);
        EXPECT_GT(CountDistinctColours(resizedSecond), kMinDistinctColours) << "the resized second view drew nothing";
        for (const Renderer3D::ViewRetainedHistory& history : Renderer3D::GetViewRetainedState(m_SecondView).Histories)
            EXPECT_LE(history.Age, kAfterResize) << "a second-view history survived the resize of its own view";

        // --- Cut the second view's camera ------------------------------------
        {
            const Renderer3D::ScopedView scope(m_SecondView);
            Renderer3D::ResetFrameCamera();
        }
        RunSharedFrames(poses.Main, Order::SecondAfterMain, kAfterCut); // and it now looks from the main pose
        const Renderer3D::ViewRetainedState afterCut = Renderer3D::GetViewRetainedState(kMainRenderView);
        ExpectRetained(afterResize, afterCut, kAfterCut, "after cutting the second view's camera");
        for (const Renderer3D::ViewRetainedHistory& history : Renderer3D::GetViewRetainedState(m_SecondView).Histories)
            EXPECT_LE(history.Age, kAfterCut) << "a second-view history survived its own camera cut";

        // --- Destroy the second view -----------------------------------------
        const RenderViewId destroyed = m_SecondView;
        DestroySecondView();
        EXPECT_FALSE(Renderer3D::IsViewAlive(destroyed));
        EXPECT_EQ(Renderer3D::GetViewCount(), 1u);
        EXPECT_FALSE(Renderer3D::GetViewRetainedState(destroyed).Alive);
        RunMainFrames(kAfterDestroy);
        const Renderer3D::ViewRetainedState afterDestroy = Renderer3D::GetViewRetainedState(kMainRenderView);
        ExpectRetained(afterCut, afterDestroy, kAfterDestroy, "after destroying the second view");

        // The main view's frame is the frame it renders alone: its jitter
        // sequence, its history and its adapted exposure never noticed.
        const Frame survivor = ReadMainView();
        WriteEvidence(TestRenderBackend::EvidenceName("MultiViewLifetime") + "MainAfterDestroy", survivor);
        ExpectSameFrame(reference, survivor, "main view after the second was resized, cut and destroyed");
    }

    // The other direction: the MAIN view is the one resized and cut (a window
    // resize, an editor camera teleport) and the second view is the survivor.
    // Its histories, pyramid and shadow maps keep their identities and ages,
    // and its frame is the frame the same camera renders alone.
    TEST_F(MultiViewLifetime, ResizingOrCuttingTheMainViewLeavesTheSecondIntact)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime mockTime(kCaptureTime);
        StepClockEachFrame();
        constexpr u32 kWarm = 8u;
        constexpr u32 kAfterResize = 4u;
        constexpr u32 kAfterCut = 4u;
        constexpr Extent kMainResized{ 224u, 160u };
        const PosePair& poses = kPosePairs[0];
        const Configure retained = []()
        {
            Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
            Renderer3D::GetRendererSettings().HZBOcclusionCullingEnabled = true;
            auto& post = Renderer3D::GetPostProcessSettings();
            post.TAAEnabled = true;
            post.AutoExposureEnabled = true;
        };

        // The reference: the second camera alone for the same total frame count.
        const Frame reference = RenderReference(poses.Second, kSecondSize, kWarm + kAfterResize + kAfterCut, retained);
        WriteEvidence(TestRenderBackend::EvidenceName("MultiViewLifetime") + "SecondAlone", reference);

        CreateSecondView();
        if (HasFatalFailure())
            return;
        SizeMainView(kMainSize);
        SetMainCamera(poses.Main);
        ConfigureActiveView(retained);
        ConfigureSecondView(retained);
        SettleConfiguration([this, &poses]()
                            {
                                GetScene().RequestAdditionalView(SecondViewRequest(poses.Second, Order::SecondAfterMain));
                                RunFrames(1); });
        RestartSequences();
        RunSharedFrames(poses.Second, Order::SecondAfterMain, kWarm);

        const Renderer3D::ViewRetainedState warm = Renderer3D::GetViewRetainedState(m_SecondView);
        ASSERT_TRUE(warm.Histories.ContainsByPredicate([](const Renderer3D::ViewRetainedHistory& history)
                                                       { return history.Valid && history.Age == kWarm; }))
            << "the second view holds no history " << kWarm << " frames old to retain";
        ASSERT_TRUE(warm.OcclusionPyramidValid) << "the second view built no occlusion pyramid to retain";

        // --- Resize the main view (the window-resize path) --------------------
        SizeMainView(kMainResized);
        RunSharedFrames(poses.Second, Order::SecondAfterMain, kAfterResize);
        const Renderer3D::ViewRetainedState afterResize = Renderer3D::GetViewRetainedState(m_SecondView);
        ExpectRetained(warm, afterResize, kAfterResize, "after resizing the main view");
        const Frame resizedMain = ReadMainView();
        EXPECT_EQ(resizedMain.Size.Width, kMainResized.Width);
        EXPECT_EQ(resizedMain.Size.Height, kMainResized.Height);
        for (const Renderer3D::ViewRetainedHistory& history : Renderer3D::GetViewRetainedState(kMainRenderView).Histories)
            EXPECT_LE(history.Age, kAfterResize) << "a main-view history survived the resize of its own view";

        // --- Cut the main view's camera --------------------------------------
        ASSERT_EQ(Renderer3D::GetActiveView(), kMainRenderView);
        Renderer3D::ResetFrameCamera();
        SetMainCamera(kPosePairs[1].Main);
        RunSharedFrames(poses.Second, Order::SecondAfterMain, kAfterCut);
        const Renderer3D::ViewRetainedState afterCut = Renderer3D::GetViewRetainedState(m_SecondView);
        ExpectRetained(afterResize, afterCut, kAfterCut, "after cutting the main view's camera");
        for (const Renderer3D::ViewRetainedHistory& history : Renderer3D::GetViewRetainedState(kMainRenderView).Histories)
            EXPECT_LE(history.Age, kAfterCut) << "a main-view history survived its own camera cut";

        // The second view's frame is the frame its camera renders alone.
        const Frame survivor = ReadSecondView();
        WriteEvidence(TestRenderBackend::EvidenceName("MultiViewLifetime") + "SecondAfterMainResizedAndCut", survivor);
        ExpectSameFrame(reference, survivor, "second view after the main view was resized and cut");
    }

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
            EXPECT_EQ(after.SceneClockSteps - before.SceneClockSteps, kFrames)
                << OrderName(order) << ": the scene's animation clock (wind, water, foliage motion) steps once per frame";
            EXPECT_EQ(Renderer3D::GetStochasticFrameIndex() - stochasticBefore, kFrames)
                << OrderName(order) << ": every view of a frame samples the same stochastic index";

            const RenderViewInfo main = Renderer3D::GetViewInfo(kMainRenderView);
            const RenderViewInfo second = Renderer3D::GetViewInfo(m_SecondView);
            EXPECT_EQ(main.LastRenderedFrameSerial, second.LastRenderedFrameSerial)
                << OrderName(order) << ": both views rendered in the same physical frame";
            EXPECT_EQ(main.WasPrimary, order == Order::SecondAfterMain) << OrderName(order);
            EXPECT_EQ(second.WasPrimary, order == Order::SecondBeforeMain) << OrderName(order);
            // A later view of the frame links its draws to the records the
            // primary view committed; every draw of this scene has one.
            EXPECT_GT(Renderer3D::GetGPUSceneLinkedDrawCount(), 0u) << OrderName(order);
            EXPECT_EQ(Renderer3D::GetGPUSceneUnlinkedDrawCount(), 0u)
                << OrderName(order) << ": a draw of the frame's last view found no committed record";
        }

        // The single-view control: one view frame per physical frame.
        const Renderer3D::SceneAdvanceCounters before = Renderer3D::GetSceneAdvanceCounters();
        RunMainFrames(kFrames);
        const Renderer3D::SceneAdvanceCounters after = Renderer3D::GetSceneAdvanceCounters();
        EXPECT_EQ(after.PhysicalFrames - before.PhysicalFrames, kFrames);
        EXPECT_EQ(after.ViewFrames - before.ViewFrames, kFrames);
        EXPECT_EQ(after.SceneExtractions - before.SceneExtractions, kFrames);
        EXPECT_EQ(after.PoseHistoryRotations - before.PoseHistoryRotations, kFrames);
    }

    // The history registry of a view's graph is keyed by that view.
    TEST_F(MultiViewSceneAdvance, AViewsHistoriesAreKeyedByItsOwnId)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime mockTime(kCaptureTime);
        const Configure withHistory = []()
        { Renderer3D::GetPostProcessSettings().TAAEnabled = true; };

        CreateSecondView();
        if (HasFatalFailure())
            return;
        (void)RenderShared(kPosePairs[0], Order::SecondAfterMain, 4u, withHistory, withHistory);

        const Renderer3D::ViewRetainedState main = Renderer3D::GetViewRetainedState(kMainRenderView);
        const Renderer3D::ViewRetainedState second = Renderer3D::GetViewRetainedState(m_SecondView);
        ASSERT_FALSE(main.Histories.IsEmpty()) << "TAA acquired no history in the main view";
        ASSERT_FALSE(second.Histories.IsEmpty()) << "TAA acquired no history in the second view";
        EXPECT_NE(m_SecondView.Value, 0u);
        for (const Renderer3D::ViewRetainedHistory& history : main.Histories)
            EXPECT_EQ(history.Key.View, 0u);
        for (const Renderer3D::ViewRetainedHistory& history : second.Histories)
        {
            EXPECT_EQ(history.Key.View, m_SecondView.Value);
            // And its storage is its own: no texture the main view also holds.
            if (history.TextureIdentity != 0u)
                EXPECT_FALSE(main.Histories.ContainsByPredicate([&history](const Renderer3D::ViewRetainedHistory& other)
                                                                { return other.TextureIdentity == history.TextureIdentity; }));
        }
    }

    // A change in what the world draws that no history can reproject across (a
    // streaming tier switched, foliage was regenerated) happened to every view
    // that draws the world, whichever view was active when the world noticed.
    // One camera's own discontinuity stays that view's.
    TEST_F(MultiViewSceneAdvance, AWorldInvalidationReachesEveryViewAndAViewsOwnOnlyItself)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime mockTime(kCaptureTime);
        constexpr u32 kWarm = 6u;
        constexpr u32 kAfter = 2u;
        const PosePair& poses = kPosePairs[0];
        const Configure withHistory = []()
        { Renderer3D::GetPostProcessSettings().TAAEnabled = true; };
        const auto oldestValid = [](RenderViewId view)
        {
            u32 oldest = 0;
            for (const Renderer3D::ViewRetainedHistory& history : Renderer3D::GetViewRetainedState(view).Histories)
            {
                if (history.Valid)
                    oldest = std::max(oldest, history.Age);
            }
            return oldest;
        };

        CreateSecondView();
        if (HasFatalFailure())
            return;
        (void)RenderShared(poses, Order::SecondAfterMain, kWarm, withHistory, withHistory);
        ASSERT_GE(oldestValid(kMainRenderView), kWarm) << "the main view holds no warm history to drop";
        ASSERT_GE(oldestValid(m_SecondView), kWarm) << "the second view holds no warm history to drop";

        // The world's: asked for with the MAIN view active, it reaches both.
        ASSERT_EQ(Renderer3D::GetActiveView(), kMainRenderView);
        EXPECT_GT(Renderer3D::InvalidateWorldTemporalHistories(), 0u);
        RunSharedFrames(poses.Second, Order::SecondAfterMain, kAfter);
        EXPECT_LE(oldestValid(kMainRenderView), kAfter) << "a main-view history survived a world invalidation";
        EXPECT_LE(oldestValid(m_SecondView), kAfter)
            << "a second-view history survived a world invalidation raised while the main view was active";

        // The control, and the other half of the rule: the same reset through
        // the per-view seam, with the main view active, leaves the second
        // view's histories ageing.
        RunSharedFrames(poses.Second, Order::SecondAfterMain, kWarm);
        const u32 secondBefore = oldestValid(m_SecondView);
        ASSERT_GE(secondBefore, kWarm);
        EXPECT_GT(Renderer3D::InvalidateTemporalHistories(TemporalHistoryInvalidationCause::Manual), 0u);
        RunSharedFrames(poses.Second, Order::SecondAfterMain, kAfter);
        EXPECT_LE(oldestValid(kMainRenderView), kAfter) << "a main-view history survived its own reset";
        EXPECT_GE(oldestValid(m_SecondView), secondBefore + kAfter)
            << "one view's own reset dropped the other view's histories";
    }

    // An object in motion writes the same velocity in both views: object poses
    // rotate to "previous" once per frame, so the frame's last view still
    // measures against LAST frame's pose. Motion blur makes the velocity
    // visible in the composite.
    TEST_F(MultiViewSceneAdvance, AMovingObjectBlursTheSameInBothViews)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime mockTime(kCaptureTime);
        MoveMoverEachFrame();
        const Configure blurred = []()
        {
            Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
            auto& post = Renderer3D::GetPostProcessSettings();
            post.MotionBlurEnabled = true;
            post.MotionBlurStrength = 1.0f;
        };
        const PosePair& poses = kPosePairs[0];

        // The premise: the motion is in the frame. The same camera with the
        // mover held still must differ from the frame with it moving.
        const Frame moving = RenderReference(poses.Second, kSecondSize, kSettleFrames, blurred);
        m_BeforeFrame = [this](u32)
        {
            m_Mover.GetComponent<TransformComponent>().Translation =
                kMoverStart + glm::vec3(0.12f * static_cast<f32>(kSettleFrames - 1u), 0.0f, 0.0f);
        };
        const Frame still = RenderReference(poses.Second, kSecondSize, kSettleFrames, blurred);
        const u32 blurPixels = Compare(moving, still).Pixels;
        std::cout << "[MultiView] motion blur moves " << blurPixels << " pixel(s) of the second camera's frame\n";
        ASSERT_GT(blurPixels, 20u) << "the mover's motion does not show in the second camera's frame";
        MoveMoverEachFrame();

        ExpectBothViewsMatchTheirReferences(TestRenderBackend::EvidenceName("MultiViewMotion") + "Deferred",
                                            "moving object", poses, kSettleFrames, blurred, blurred);
    }

    // -------------------------------------------------------------------------
    // Negative controls: a renderer without views, re-created by a lever.
    // -------------------------------------------------------------------------
    class MultiViewNegativeControl : public MultiViewFixture
    {
    };

    // Every view advances the world: the frame's last view finds this frame's
    // poses as its "previous", so the mover it draws has no velocity.
    TEST_F(MultiViewNegativeControl, AWorldAdvancedOncePerViewIsCaught)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime mockTime(kCaptureTime);
        MoveMoverEachFrame();
        const Configure blurred = []()
        {
            Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
            auto& post = Renderer3D::GetPostProcessSettings();
            post.MotionBlurEnabled = true;
            post.MotionBlurStrength = 1.0f;
        };
        const PosePair& poses = kPosePairs[0];
        const Frame secondReference = RenderReference(poses.Second, kSecondSize, kSettleFrames, blurred);

        CreateSecondView();
        if (HasFatalFailure())
            return;
        const SharedFrames clean = RenderShared(poses, Order::SecondAfterMain, kSettleFrames, blurred, blurred);
        ASSERT_EQ(Compare(secondReference, clean.Second).Pixels, 0u)
            << "the second view differs from its reference with the fault OFF; the control below proves nothing";

        SharedFrames faulty;
        Renderer3D::SceneAdvanceCounters before{};
        Renderer3D::SceneAdvanceCounters after{};
        {
            const ScopedFault fault(&Levers::FaultAdvanceWorldEveryView, &Levers::SetFaultAdvanceWorldEveryView);
            before = Renderer3D::GetSceneAdvanceCounters();
            faulty = RenderShared(poses, Order::SecondAfterMain, kSettleFrames, blurred, blurred);
            after = Renderer3D::GetSceneAdvanceCounters();
        }
        WriteEvidence(TestRenderBackend::EvidenceName("MultiViewNegativeControl") + "WorldPerView_Second", faulty.Second);
        const Difference difference = Compare(secondReference, faulty.Second);
        std::cout << "[MultiView] fault advance-world-every-view: " << difference.Pixels
                  << " pixel(s) of the second view differ, max delta " << difference.MaxDelta << "/255\n";
        EXPECT_GT(difference.Pixels, 20u)
            << "a world advanced once per view rendered the frame's last view exactly as a single view does: the "
               "comparison is blind to a per-view pose rotation";
        EXPECT_EQ(after.PoseHistoryRotations - before.PoseHistoryRotations, 2u * (kConfigureSettleFrames + kSettleFrames))
            << "the lever did not act: poses still rotated once per frame";
    }

    // One camera history for every view: each view finds the other's
    // projection and previous view-projection where its own should be, so its
    // temporal resolve reprojects from, and is reset by, the other camera.
    TEST_F(MultiViewNegativeControl, ACameraHistorySharedByBothViewsIsCaught)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime mockTime(kCaptureTime);
        const Configure resolved = []()
        { Renderer3D::GetPostProcessSettings().TAAEnabled = true; };
        const PosePair& poses = kPosePairs[0];
        constexpr u32 kFrames = 12u;
        const Frame mainReference = RenderReference(poses.Main, kMainSize, kFrames, resolved);
        const Frame secondReference = RenderReference(poses.Second, kSecondSize, kFrames, resolved);

        CreateSecondView();
        if (HasFatalFailure())
            return;
        const SharedFrames clean = RenderShared(poses, Order::SecondAfterMain, kFrames, resolved, resolved);
        ASSERT_EQ(Compare(mainReference, clean.Main).Pixels + Compare(secondReference, clean.Second).Pixels, 0u)
            << "the views differ from their references with the fault OFF; the control below proves nothing";

        SharedFrames faulty;
        {
            const ScopedFault fault(&Levers::FaultShareViewCameraHistory, &Levers::SetFaultShareViewCameraHistory);
            faulty = RenderShared(poses, Order::SecondAfterMain, kFrames, resolved, resolved);
        }
        WriteEvidence(TestRenderBackend::EvidenceName("MultiViewNegativeControl") + "SharedCameraHistory_Main", faulty.Main);
        WriteEvidence(TestRenderBackend::EvidenceName("MultiViewNegativeControl") + "SharedCameraHistory_Second",
                      faulty.Second);
        const Difference main = Compare(mainReference, faulty.Main);
        const Difference second = Compare(secondReference, faulty.Second);
        std::cout << "[MultiView] fault share-view-camera-history: main " << main.Pixels << " px (max " << main.MaxDelta
                  << "), second " << second.Pixels << " px (max " << second.MaxDelta << ")\n";
        EXPECT_GT(main.Pixels + second.Pixels, 100u)
            << "two views sharing one camera history rendered exactly what single views render: the comparison is "
               "blind to the coupling this issue exists to remove";
    }

    // -------------------------------------------------------------------------
    // Acceptance 4: the alternate-camera users inside a view.
    // -------------------------------------------------------------------------
    class MultiViewAlternateCamera : public MultiViewFixture
    {
      protected:
        // A calm reflective water plane and a tall red pillar standing in it.
        // Red is nowhere else in the scene, so red in the water is the mirror.
        void AddReflectivePool()
        {
            m_Water = GetScene().CreateEntity("ReflectiveWater");
            m_Water.GetComponent<TransformComponent>().Translation = { 0.0f, 0.15f, 0.0f };
            auto& water = m_Water.AddComponent<WaterComponent>();
            water.m_WorldSizeX = 60.0f;
            water.m_WorldSizeZ = 60.0f;
            water.m_GridResolutionX = 64;
            water.m_GridResolutionZ = 64;
            water.m_WaveAmplitude = 0.05f;
            water.m_SSREnabled = false;
            water.m_PlanarReflectionsEnabled = true;
            water.m_PlanarReflectionIntensity = 1.0f;
            water.m_PlanarReflectionDistortion = 0.01f;

            Entity pillar = GetScene().CreateEntity("RedPillar");
            auto& transform = pillar.GetComponent<TransformComponent>();
            transform.Translation = { -1.0f, 3.0f, -6.0f };
            transform.Scale = { 1.2f, 6.0f, 1.2f };
            auto& mesh = pillar.AddComponent<MeshComponent>();
            mesh.m_Primitive = MeshPrimitive::Cube;
            mesh.m_MeshSource = m_Cube;
            auto& material = pillar.AddComponent<MaterialComponent>();
            material.m_Material.SetBaseColorFactor(glm::vec4(0.95f, 0.03f, 0.03f, 1.0f));
            material.m_Material.SetEmissiveFactor(glm::vec4(0.6f, 0.0f, 0.0f, 1.0f));
        }

        void SetPlanarReflections(bool enabled)
        {
            m_Water.GetComponent<WaterComponent>().m_PlanarReflectionsEnabled = enabled;
        }

        // A scene with water publishes the pool's underwater fog to the
        // renderer, where it outlives the scene. Put back what was there.
        void TearDown() override
        {
            Renderer3D::SetUnderwaterFogState(m_SavedUnderwaterFog);
            MultiViewFixture::TearDown();
        }

        Entity m_Water;
        UnderwaterFogState m_SavedUnderwaterFog = Renderer3D::GetUnderwaterFogState();
    };

    // The planar reflection is a replay from a mirrored camera INSIDE a view's
    // frame (ADR 0025, V3). With two views each view mirrors its own camera:
    // the request is published per view frame and the mirror pass is the
    // view's own. Each view's reflection is the one a single view renders.
    TEST_F(MultiViewAlternateCamera, EachViewReflectsFromItsOwnCamera)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime mockTime(kCaptureTime);
        AddReflectivePool();
        // Both cameras look across the water at the pillar from different sides.
        const PosePair poses{ "Pool", Pose{ { 3.0f, 2.0f, 8.0f }, { glm::radians(-9.0f), glm::radians(18.0f), 0.0f } },
                              Pose{ { -9.0f, 2.6f, 3.0f }, { glm::radians(-11.0f), glm::radians(-62.0f), 0.0f } } };

        // The premise, for each camera: the mirror is in the frame.
        for (const auto& [pose, size, name] : { std::tuple{ poses.Main, kMainSize, "main" },
                                                std::tuple{ poses.Second, kSecondSize, "second" } })
        {
            SetPlanarReflections(true);
            const Frame mirrored = RenderReference(pose, size);
            SetPlanarReflections(false);
            const Frame plain = RenderReference(pose, size);
            const u32 reflected = Compare(mirrored, plain).Pixels;
            std::cout << "[MultiView] planar reflection moves " << reflected << " pixel(s) of the " << name
                      << " camera's frame\n";
            ASSERT_GT(reflected, 200u) << "the planar reflection does not show from the " << name << " camera";
        }
        SetPlanarReflections(true);

        ExpectBothViewsMatchTheirReferences(TestRenderBackend::EvidenceName("MultiViewPlanarReflection") + "Pool",
                                            "planar reflection", poses, kSettleFrames, {}, {});
    }

    // -------------------------------------------------------------------------
    // Ray tracing: every view traces the frame's one acceleration structure.
    // -------------------------------------------------------------------------
    class MultiViewRayTracing : public MultiViewFixture
    {
    };

    // The scene's acceleration structures are built once per physical frame, by
    // its primary view, about the frame's render origin. A later view of the
    // frame traces that build: its ray-traced shadows are the ones a single
    // view renders, in both orders (as the primary view that builds, and as
    // the later view that only traces).
    TEST_F(MultiViewRayTracing, BothViewsTraceTheFramesAccelerationStructure)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        if (!Renderer3D::GetRayTracingScene().IsAvailable())
            GTEST_SKIP() << "no ray-tracing device in this process: ray-traced shadows resolve to shadow maps here, "
                            "which MultiViewRendering.* already covers";
        const ScopedMockTime mockTime(kCaptureTime);
        // A light opts in to the ray-traced tier; the technique is each view's.
        m_Sun.GetComponent<DirectionalLightComponent>().m_RayTracedShadows = true;
        const Configure shadowMapped = []()
        { Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred; };
        const Configure rayTraced = []()
        {
            Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
            ShadowSettings shadows = Renderer3D::GetShadowMap().GetSettings();
            shadows.Technique = ShadowTechnique::RayTraced;
            Renderer3D::GetShadowMap().SetSettings(shadows);
        };
        const PosePair& poses = kPosePairs[0];
        constexpr u32 kFrames = 12u; // the mask denoises over a few frames

        // The premise: the ray-traced mask is what shades the frame.
        const Frame traced = RenderReference(poses.Second, kSecondSize, kFrames, rayTraced);
        const Frame mapped = RenderReference(poses.Second, kSecondSize, kFrames, shadowMapped);
        const u32 changed = Compare(traced, mapped).Pixels;
        std::cout << "[MultiView] ray-traced shadows move " << changed << " pixel(s) of the second camera's frame\n";
        ASSERT_GT(changed, 40u) << "ray-traced shadows render the shadow-mapped frame: the tier is not running";

        ExpectBothViewsMatchTheirReferences(TestRenderBackend::EvidenceName("MultiViewRayTracedShadows") + "Deferred",
                                            "ray-traced shadows", poses, kFrames, rayTraced, rayTraced);
        if (HasFatalFailure())
            return;
        // One view traces and the other does not: the frame is extracted by
        // whichever is primary, for what EITHER asks.
        ExpectBothViewsMatchTheirReferences(TestRenderBackend::EvidenceName("MultiViewRayTracedShadows") + "SecondOnly",
                                            "ray-traced second view only", poses, kFrames, shadowMapped, rayTraced);
    }

    // -------------------------------------------------------------------------
    // Acceptance 4, the stated limit: a probe bake renders through the main view.
    // -------------------------------------------------------------------------
    class MultiViewBake : public MultiViewFixture
    {
      protected:
        // The bake binds its own framebuffer and this test puts the GL state
        // back with GLStateGuard, as every bake test does; no bake test runs in
        // the Vulkan process.
        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return false;
        }
    };

    // The bakers render the scene from a probe's six faces through the renderer
    // (ADR 0026, section 6). They do it through the MAIN view whichever view
    // the caller had active: a bake started under another view's scope renders
    // nothing through that view and leaves everything it retains as it was.
    TEST_F(MultiViewBake, ABakeStartedUnderAnotherViewRendersThroughTheMainView)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime mockTime(kCaptureTime);
        constexpr u32 kWarm = 6u;
        const PosePair& poses = kPosePairs[0];
        const Configure withHistory = []()
        {
            Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
            Renderer3D::GetRendererSettings().HZBOcclusionCullingEnabled = true;
            Renderer3D::GetPostProcessSettings().TAAEnabled = true;
        };

        CreateSecondView();
        if (HasFatalFailure())
            return;
        (void)RenderShared(poses, Order::SecondAfterMain, kWarm, withHistory, withHistory);
        const Renderer3D::ViewRetainedState secondBefore = Renderer3D::GetViewRetainedState(m_SecondView);
        const RenderViewInfo secondInfoBefore = Renderer3D::GetViewInfo(m_SecondView);
        const RenderViewInfo mainInfoBefore = Renderer3D::GetViewInfo(kMainRenderView);
        ASSERT_TRUE(secondBefore.Histories.ContainsByPredicate([](const Renderer3D::ViewRetainedHistory& history)
                                                               { return history.Valid; }))
            << "the second view holds no history a bake could disturb";

        Ref<Scene> scene = GetSceneRef();
        // A component of the test's own: the bake reads its resolution and
        // stores its result on it, and nothing in the scene selects it.
        ReflectionProbeComponent probe;
        probe.m_Resolution = 32u;
        bool baked = false;
        {
            // The caller's view is the second one.
            const Renderer3D::ScopedView scope(m_SecondView);
            {
                GLStateGuard guard("MultiViewBake", GLStateGuard::Policy::Restore);
                baked = ReflectionProbeBaker::BakeProbe(scene, glm::vec3(0.0f, 2.0f, 1.0f), probe);
            }
            EXPECT_EQ(Renderer3D::GetActiveView(), m_SecondView) << "the bake left another view active";
        }
        ASSERT_TRUE(baked) << "ReflectionProbeBaker::BakeProbe failed";
        ASSERT_TRUE(probe.m_BakedEnvironment && probe.m_BakedEnvironment->HasIBL()) << "the bake produced no environment";

        const RenderViewInfo secondInfoAfter = Renderer3D::GetViewInfo(m_SecondView);
        const RenderViewInfo mainInfoAfter = Renderer3D::GetViewInfo(kMainRenderView);
        EXPECT_EQ(secondInfoAfter.FramesRendered, secondInfoBefore.FramesRendered)
            << "the bake rendered through the view its caller had active";
        EXPECT_GE(mainInfoAfter.FramesRendered, mainInfoBefore.FramesRendered + 6u)
            << "the bake's six faces did not render through the main view";
        EXPECT_EQ(mainInfoAfter.Width, mainInfoBefore.Width) << "the bake left the main view at its own size";
        EXPECT_EQ(mainInfoAfter.Height, mainInfoBefore.Height);
        // Nothing rendered through the second view, so nothing it retains aged.
        ExpectRetained(secondBefore, Renderer3D::GetViewRetainedState(m_SecondView), 0u, "after a bake under its scope");
    }

    // -------------------------------------------------------------------------
    // Foliage: which plants exist is the world's, what a camera sees of them is
    // the view's.
    // -------------------------------------------------------------------------
    class MultiViewFoliage : public MultiViewFixture
    {
      protected:
        void BuildScene() override
        {
            MultiViewFixture::BuildScene();

            // A flat meadow over the fixture's ground, card grass on all of it.
            constexpr f32 kMeadowSize = 48.0f;
            m_Meadow = GetScene().CreateEntity("Meadow");
            m_Meadow.GetComponent<TransformComponent>().Translation =
                glm::vec3(-0.5f * kMeadowSize, 0.02f, -0.5f * kMeadowSize);
            auto& terrain = m_Meadow.AddComponent<TerrainComponent>();
            terrain.m_WorldSizeX = kMeadowSize;
            terrain.m_WorldSizeZ = kMeadowSize;
            terrain.m_ProceduralEnabled = false; // with no heightmap: flat, at the entity's height
            terrain.m_HeightScale = 1.0f;
            terrain.m_TessellationEnabled = false;
            terrain.m_Material = Ref<TerrainMaterial>::Create();
            TerrainLayer turf;
            turf.Name = "Turf";
            turf.BaseColor = glm::vec3(0.32f, 0.3f, 0.26f);
            turf.Roughness = 0.95f;
            terrain.m_Material->AddLayer(turf);
            terrain.m_AutoMaterial = true;
            TerrainLayerRule everywhere;
            everywhere.LayerIndex = 0;
            everywhere.HeightBlend = 0.0f;
            terrain.m_LayerRules.Add(everywhere);

            auto& foliage = m_Meadow.AddComponent<FoliageComponent>();
            foliage.m_Enabled = true;
            FoliageLayer grass;
            grass.Name = "Meadow";
            grass.AlbedoPath = "assets/textures/grass.png";
            grass.Density = 6.0f;
            grass.SplatmapChannel = -1;
            grass.MinSlopeAngle = 0.0f;
            grass.MaxSlopeAngle = 70.0f;
            grass.MinScale = 0.8f;
            grass.MaxScale = 1.2f;
            grass.MinHeight = 0.5f;
            grass.MaxHeight = 0.9f;
            // Past both cameras' reach, with the fade and the density LOD
            // inside the frame: both are functions of a camera's distance.
            grass.ViewDistance = 30.0f;
            grass.FadeStartDistance = 18.0f;
            grass.UseDensityLod = true;
            grass.DensityLodStartDistance = 6.0f;
            grass.DensityLodEndDistance = 20.0f;
            grass.DensityLodMinFraction = 0.3f;
            grass.UseAuthoredMesh = false;
            grass.UseImpostor = false;
            grass.AlphaCutoff = 0.25f;
            // Swaying: the wind's previous-frame state is the world's.
            grass.WindStrength = 0.3f;
            grass.WindSpeed = 1.1f;
            grass.BaseColor = glm::vec3(0.18f, 0.48f, 0.14f);
            foliage.m_Layers.Add(grass);
            foliage.m_NeedsRebuild = true;
        }

        // Placement is generated across frames; a comparison starts from a
        // published meadow.
        void WaitForTheMeadow()
        {
            constexpr u32 kMaxFrames = 240u;
            for (u32 i = 0; i < kMaxFrames; ++i)
            {
                RunFrames(1);
                const auto& foliage = m_Meadow.GetComponent<FoliageComponent>();
                if (foliage.m_Renderer && !foliage.m_NeedsRebuild && !foliage.m_Renderer->IsRebuilding() &&
                    foliage.m_Renderer->GetTotalInstanceCount() > 0u)
                    return;
            }
            FAIL() << "the meadow published no plants in " << kMaxFrames << " frames";
        }

        Entity m_Meadow;
    };

    // Each view draws the meadow a single view draws from its camera: the same
    // plants (they are the world's, placed and published once per frame by its
    // primary view), culled, thinned and faded for THAT view's camera.
    //
    // The clock is pinned for the whole test. The scene's animation clock only
    // accumulates, so two runs that each stepped it would sway the grass at
    // two phases and differ with one view and no second view anywhere. That
    // the clock and the foliage step once per physical frame is counted below
    // and in TwoViewsAdvanceTheSceneOncePerPhysicalFrame.
    //
    // The comparison is bounded, not exact, and the bound is a single view's
    // own: GPU-culled card foliage under TAA does not reproduce itself (#1695).
    // The cull compacts its visible list with an atomic cursor, the plants are
    // drawn in whatever order that left, and where two cards intersect within
    // a depth quantum the first one drawn wins. ONE view rendered twice from a
    // sequence restart differed in 27 of 48 rounds on Vulkan and 1 of 40 on
    // OpenGL, by 2 to 6 pixels and at most 2/255, and in none of 24 with the
    // GPU cull off. A view that drew the other view's plants, cull or LOD
    // moves thousands of pixels: the meadow covers 17,000 of the smaller frame.
    TEST_F(MultiViewFoliage, EachViewDrawsTheMeadowASingleViewDrawsFromItsCamera)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime mockTime(kCaptureTime);
        const PosePair& poses = kPosePairs[1]; // the cameras look at different parts of it
        const Configure temporal = []()
        {
            Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
            Renderer3D::GetPostProcessSettings().TAAEnabled = true;
        };

        WaitForTheMeadow();
        if (HasFatalFailure())
            return;

        // The premise: the plants are in both cameras' frames.
        for (const auto& [pose, size, name] : { std::tuple{ poses.Main, kMainSize, "main" },
                                                std::tuple{ poses.Second, kSecondSize, "second" } })
        {
            const Frame planted = RenderReference(pose, size, kSettleFrames, temporal);
            m_Meadow.GetComponent<FoliageComponent>().m_Enabled = false;
            const Frame bare = RenderReference(pose, size, kSettleFrames, temporal);
            m_Meadow.GetComponent<FoliageComponent>().m_Enabled = true;
            WaitForTheMeadow();
            if (HasFatalFailure())
                return;
            const Difference plants = Compare(planted, bare);
            std::cout << "[MultiView] the meadow covers " << plants.Pixels << " pixel(s) of the " << name << " camera's frame\n";
            ASSERT_GT(plants.Pixels, 5000u) << "the " << name << " camera sees too few plants: the bounded comparison below would hold vacuously";
        }

        // Several times the measured floor: the software rasteriser CI runs
        // schedules the cull's invocations differently from this GPU. The
        // pixel count is the bound that tells noise from a leak.
        constexpr Tolerance kCullOrderNoise{ .Pixels = 32u, .MaxDelta = 16u };
        ExpectBothViewsMatchTheirReferences(TestRenderBackend::EvidenceName("MultiViewFoliage") + "Deferred",
                                            "foliage", poses, kSettleFrames, temporal, temporal, kCullOrderNoise);
        if (HasFatalFailure())
            return;

        // The meadow's placement and residency advance once per physical frame,
        // whichever view is primary, and the fault that advances the world in
        // every view is what doubles it.
        constexpr u32 kFrames = 4u;
        CreateSecondView();
        if (HasFatalFailure())
            return;
        for (const Order order : { Order::SecondAfterMain, Order::SecondBeforeMain })
        {
            const Renderer3D::SceneAdvanceCounters before = Renderer3D::GetSceneAdvanceCounters();
            RunSharedFrames(poses.Second, order, kFrames);
            const Renderer3D::SceneAdvanceCounters after = Renderer3D::GetSceneAdvanceCounters();
            EXPECT_EQ(after.FoliageWorldSteps - before.FoliageWorldSteps, kFrames)
                << OrderName(order) << ": the meadow was advanced once per view, not once per frame";
        }
        {
            const ScopedFault fault(&Levers::FaultAdvanceWorldEveryView, &Levers::SetFaultAdvanceWorldEveryView);
            const Renderer3D::SceneAdvanceCounters before = Renderer3D::GetSceneAdvanceCounters();
            RunSharedFrames(poses.Second, Order::SecondAfterMain, kFrames);
            const Renderer3D::SceneAdvanceCounters after = Renderer3D::GetSceneAdvanceCounters();
            EXPECT_EQ(after.FoliageWorldSteps - before.FoliageWorldSteps, 2u * kFrames)
                << "the count does not see a meadow advanced in every view: the assertion above proves nothing";
            EXPECT_EQ(after.SceneClockSteps - before.SceneClockSteps, 2u * kFrames);
        }
    }

    // -------------------------------------------------------------------------
    // Acceptance 5: what a second view costs, and what it leaves behind.
    // -------------------------------------------------------------------------
    class MultiViewCost : public MultiViewFixture
    {
      protected:
        struct Timing
        {
            f64 CpuMsPerFrame = 0.0;
            f64 GpuMsLastViewFrame = 0.0;
            bool GpuValid = false;
        };

        // Wall time of `frames` physical frames, and the GPU time of the last
        // view frame the timer pool resolved. Reported, never asserted: this is
        // possibly a Debug binary, on a shared machine. Every line names the
        // configuration it was measured in.
        template<typename TRun>
        [[nodiscard]] static Timing Measure(u32 frames, TRun&& run)
        {
            const auto start = std::chrono::steady_clock::now();
            run(frames);
            const f64 totalMs = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
            const GpuTimingSample gpu = GPUPassTimerPool::GetInstance().GetLastFrameGpuSample();
            return Timing{ .CpuMsPerFrame = totalMs / static_cast<f64>(frames),
                           .GpuMsLastViewFrame = gpu.ValueOr(0.0),
                           .GpuValid = gpu.IsValid() };
        }
    };

    // A view's memory is its own targets, histories, pyramid, light grid and
    // shadow maps: creating one adds them, destroying it gives every byte back,
    // and the main view's share does not move. The frame cost of the second
    // view is reported beside the single-view frame it is added to.
    TEST_F(MultiViewCost, ASecondViewCostsItsOwnStorageAndLeavesNothingBehind)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime mockTime(kCaptureTime);
        const auto& memory = RendererMemoryTracker::GetInstance();
        constexpr u32 kWarm = 8u;
        constexpr u32 kMeasured = 40u;
        constexpr f64 kMiB = 1024.0 * 1024.0;
        const PosePair& poses = kPosePairs[0];
        const Configure deferred = []()
        { Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred; };

        SizeMainView(kMainSize);
        SetMainCamera(poses.Main);
        ConfigureActiveView(deferred);
        RunMainFrames(kWarm);
        const u64 mainOnlyBytes = memory.GetGpuResidentBytes();
        const Timing single = Measure(kMeasured, [this](u32 frames)
                                      { RunMainFrames(frames); });
        // The frame-transient descriptor ring is the one budget the views of a
        // bracketed frame share where the backend runs behind the CPU
        // (RHI::DescriptorHeap::HoldFrameTransientReset).
        const RHI::DescriptorHeap::Stats ringBefore = RHI::DescriptorHeap::Get().GetStats();

        // The second view at the MAIN view's size, so its cost is a like-for-like
        // view rather than a smaller one.
        const auto createStart = std::chrono::steady_clock::now();
        CreateSecondView(kMainSize);
        const f64 createMs = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - createStart).count();
        if (HasFatalFailure())
            return;
        std::cout << "[MultiViewCost] " << TestRenderBackend::EvidenceBackendPrefix()
                  << BuildConfigurationName() << " binary, Renderer3D::CreateView: " << createMs
                  << " ms (a view's passes compile their own programs)\n";
        ConfigureSecondView(deferred);
        RunSharedFrames(poses.Second, Order::SecondAfterMain, kWarm);
        const u64 twoViewBytes = memory.GetGpuResidentBytes();
        // Each view's own share of a two-view frame, averaged over the same
        // frames: the main view is the primary one here and carries the scene
        // extraction and the world simulation, the second view only draws.
        f64 primaryViewMs = 0.0;
        f64 laterViewMs = 0.0;
        const Timing shared = Measure(kMeasured, [this, &poses, &primaryViewMs, &laterViewMs](u32 frames)
                                      {
                                          for (u32 i = 0; i < frames; ++i)
                                          {
                                              RunSharedFrames(poses.Second, Order::SecondAfterMain, 1u);
                                              primaryViewMs += Renderer3D::GetViewInfo(kMainRenderView).LastFrameCpuMs;
                                              laterViewMs += Renderer3D::GetViewInfo(m_SecondView).LastFrameCpuMs;
                                          } });
        primaryViewMs /= static_cast<f64>(kMeasured);
        laterViewMs /= static_cast<f64>(kMeasured);
        const RHI::DescriptorHeap::Stats ringAfter = RHI::DescriptorHeap::Get().GetStats();
        std::cout << "[MultiViewCost] " << TestRenderBackend::EvidenceBackendPrefix()
                  << "frame-transient descriptor ring: " << ringAfter.TransientCapacity
                  << " slots per frame, peak use before the second view " << ringBefore.TransientHighWater
                  << ", with it " << ringAfter.TransientHighWater << "\n";
        EXPECT_EQ(ringAfter.TransientOverflows, ringBefore.TransientOverflows)
            << "two views of one frame ran the frame-transient descriptor ring out";
        EXPECT_EQ(ringAfter.TransientOverflowsHeld, ringBefore.TransientOverflowsHeld);

        DestroySecondView();
        // Past the deferred-deletion window: a released GPU object is retired
        // two frames after the frame that dropped it.
        RunMainFrames(kWarm);
        const u64 afterDestroyBytes = memory.GetGpuResidentBytes();

        const f64 secondViewMiB = (static_cast<f64>(twoViewBytes) - static_cast<f64>(mainOnlyBytes)) / kMiB;
        const f64 leftBehindMiB = (static_cast<f64>(afterDestroyBytes) - static_cast<f64>(mainOnlyBytes)) / kMiB;
        std::cout << "[MultiViewCost] " << TestRenderBackend::EvidenceBackendPrefix() << "Deferred " << kMainSize.Width << "x"
                  << kMainSize.Height << " renderer GPU memory: main view alone " << static_cast<f64>(mainOnlyBytes) / kMiB
                  << " MiB, with a second view " << static_cast<f64>(twoViewBytes) / kMiB << " MiB (second view +"
                  << secondViewMiB << " MiB), after destroying it " << static_cast<f64>(afterDestroyBytes) / kMiB
                  << " MiB (" << leftBehindMiB << " MiB left behind)\n";
        std::cout << "[MultiViewCost] " << TestRenderBackend::EvidenceBackendPrefix()
                  << BuildConfigurationName() << " binary, scene tick wall time per physical frame: one view "
                  << single.CpuMsPerFrame
                  << " ms, two views " << shared.CpuMsPerFrame << " ms (second view +"
                  << shared.CpuMsPerFrame - single.CpuMsPerFrame << " ms, "
                  << (single.CpuMsPerFrame > 0.0 ? 100.0 * (shared.CpuMsPerFrame - single.CpuMsPerFrame) / single.CpuMsPerFrame : 0.0)
                  << "% of a single-view frame). GPU time of the last resolved view frame: one view "
                  << (single.GpuValid ? std::to_string(single.GpuMsLastViewFrame) : std::string("n/a")) << " ms, in the two-view run "
                  << (shared.GpuValid ? std::to_string(shared.GpuMsLastViewFrame) : std::string("n/a")) << " ms\n";
        std::cout << "[MultiViewCost] " << TestRenderBackend::EvidenceBackendPrefix()
                  << BuildConfigurationName()
                  << " binary, view frame wall time inside a two-view frame (BeginScene to the end of EndScene): "
                     "primary view "
                  << primaryViewMs << " ms, later view " << laterViewMs << " ms\n";
        EXPECT_GT(primaryViewMs, 0.0) << "the main view reported no frame time";
        EXPECT_GT(laterViewMs, 0.0) << "the second view reported no frame time";

        EXPECT_GT(twoViewBytes, mainOnlyBytes) << "a second view allocated nothing: it is not rendering into its own targets";
        // Everything the view owned is gone. A megabyte of slack covers pooled
        // per-frame buffers that grew while two views rendered.
        EXPECT_LT(leftBehindMiB, 1.0) << "destroying the second view left " << leftBehindMiB << " MiB of its storage alive";
    }

    // -------------------------------------------------------------------------
    // Found on the way: a sequence restart left the temporal upscaler's history.
    // -------------------------------------------------------------------------
    class SequenceRestart : public MultiViewFixture
    {
    };

    // Renderer3D::ResetFrameSequences drops every temporal history, so the
    // frames after it are the frames a fresh renderer renders. The temporal
    // upscaler keeps its history inside its own context, outside the registry,
    // and was not told: the second run of the same frames started from the
    // first run's accumulated image. One view; nothing here needs a second.
    TEST_F(SequenceRestart, ATemporallyUpscaledRunIsTheSameAfterEveryRestart)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        if (TestRenderBackend::IsVulkanProcess() || !TemporalUpscalerUsable())
            GTEST_SKIP() << "no temporal upscaler in this process";
        const ScopedMockTime mockTime(kCaptureTime);
        // The mover walks, so the upscaler's history holds an image the next
        // run's first frames do not.
        MoveMoverEachFrame();
        SizeMainView(kMainSize);
        SetMainCamera(kPosePairs[0].Main);
        ConfigureActiveView([]()
                            {
                                Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
                                Renderer3D::GetPostProcessSettings().Upscale = UpscaleMode::Quality;
                                Renderer3D::GetPostProcessSettings().Technique = UpscalerTechnique::Temporal; });
        SettleConfiguration([this]()
                            { RunFrames(1); });

        constexpr u32 kFrames = 2u; // early: the history still dominates the frame
        RestartSequences();
        RunMainFrames(kFrames);
        const Frame first = ReadMainView();
        RunMainFrames(10u); // let the history accumulate the mover further along
        RestartSequences();
        RunMainFrames(kFrames);
        const Frame second = ReadMainView();

        WriteEvidence(TestRenderBackend::EvidenceName("SequenceRestart") + "TemporalUpscale_FirstRun", first);
        WriteEvidence(TestRenderBackend::EvidenceName("SequenceRestart") + "TemporalUpscale_SecondRun", second);
        ExpectSameFrame(first, second, "temporally upscaled frame " + std::to_string(kFrames) + " after a second restart");
    }
} // namespace OloEngine::Tests
