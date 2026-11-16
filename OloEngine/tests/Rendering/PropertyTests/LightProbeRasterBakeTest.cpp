// OLO_TEST_LAYER: L8
// =============================================================================
// LightProbeRasterBakeTest.cpp — the raster light-probe bake reads the scene.
//
// THE CLAIM. LightProbeBaker::BakeProbeAtPosition renders the lit scene from a
// probe's six faces and projects what it SAW onto spherical harmonics: a probe
// between a red wall and a blue wall comes back valid, red towards the red
// wall and blue towards the blue one.
//
// WHY IT EXISTS. The bake once bound a framebuffer of its own and read that
// back. The scene renders into the render graph's targets, never into a bound
// framebuffer, so every face was the clear colour, every probe's energy was
// zero and BakeProbeAtPosition reported every probe invalid: the editor's
// "Bake Light Probes" stored nothing. No test called the raster bake and
// looked at its result. Found while pinning the bakers to the main view
// (issue #1352); both bakers now share ProbeSceneCapture.
//
// It also holds the bake's vertical sense. A render target stores its rows
// bottom-up on OpenGL and top-down on Vulkan, and the projection maps a row to
// a direction: handed a top-down face as it was read back, it stored every side
// face upside down, so a probe over a sunlit floor under a dark sky came back
// lit from above. The same test runs in the Vulkan test process.
//
// Classification: L8 (the real Scene render path, six faces, readback). SKIPs
// cleanly without a GPU context. The bake runs inside RunInFrame, which gives
// it a scene tick's hygiene on either backend.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/LightProbeBaker.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/SphericalHarmonics.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>

#include <iostream>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kViewSize = 192u;
        constexpr u32 kProbeResolution = 32u;
        constexpr f32 kCaptureTime = 4.0f;
        const glm::vec3 kProbePosition{ 0.0f, 1.5f, 0.0f };
    } // namespace

    class LightProbeRasterBakeTest : public RendererAttachedTest
    {
      protected:
        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return true;
        }

        void BuildScene() override
        {
            EnableRendering(kViewSize, kViewSize);
            auto& renderer = Renderer3D::GetRendererSettings();
            renderer.ShowGrid = false;
            Renderer3D::ApplyRendererSettings();

            m_Cube = MeshPrimitives::CreateCube()->GetMeshSource();

            Entity camera = GetScene().CreateEntity("Camera");
            auto& cameraComponent = camera.AddComponent<CameraComponent>();
            cameraComponent.Primary = true;
            cameraComponent.Camera.SetPerspective(glm::radians(55.0f), 0.05f, 200.0f);
            cameraComponent.Camera.SetViewportSize(kViewSize, kViewSize);
            camera.GetComponent<TransformComponent>().Translation = { 0.0f, 1.5f, 6.0f };

            {
                // Straight down, so both walls' inner faces are lit alike by
                // the lamp below and the floor by the sun.
                Entity sun = GetScene().CreateEntity("Sun");
                auto& light = sun.AddComponent<DirectionalLightComponent>();
                light.m_Direction = glm::normalize(glm::vec3(0.0f, -1.0f, 0.0f));
                light.m_Color = glm::vec3(1.0f);
                light.m_Intensity = 2.0f;
                light.m_CastShadows = false;
            }
            {
                // At the probe: lights each wall's inner face from the front.
                Entity lamp = GetScene().CreateEntity("Lamp");
                lamp.GetComponent<TransformComponent>().Translation = kProbePosition;
                auto& light = lamp.AddComponent<PointLightComponent>();
                light.m_Color = glm::vec3(1.0f);
                light.m_Intensity = 12.0f;
                light.m_Range = 12.0f;
            }
            AddBox("Floor", { 0.0f, -0.25f, 0.0f }, { 12.0f, 0.5f, 12.0f }, { 0.5f, 0.5f, 0.5f, 1.0f });
            AddBox("RedWall", { 3.0f, 1.5f, 0.0f }, { 0.5f, 5.0f, 8.0f }, { 0.9f, 0.05f, 0.05f, 1.0f });
            AddBox("BlueWall", { -3.0f, 1.5f, 0.0f }, { 0.5f, 5.0f, 8.0f }, { 0.05f, 0.05f, 0.9f, 1.0f });
        }

        void AddBox(const char* name, const glm::vec3& position, const glm::vec3& scale, const glm::vec4& colour)
        {
            Entity box = GetScene().CreateEntity(name);
            auto& transform = box.GetComponent<TransformComponent>();
            transform.Translation = position;
            transform.Scale = scale;
            auto& mesh = box.AddComponent<MeshComponent>();
            mesh.m_Primitive = MeshPrimitive::Cube;
            mesh.m_MeshSource = m_Cube;
            box.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor(colour);
        }

        Ref<MeshSource> m_Cube;
    };

    TEST_F(LightProbeRasterBakeTest, AProbeBetweenARedAndABlueWallSeesEachOnItsOwnSide)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        Time::SetMockTime(kCaptureTime);
        struct ClearMockTime
        {
            ~ClearMockTime()
            {
                Time::ClearMockTime();
            }
        } const clearMockTime;

        RunFrames(3); // the live view exists, at its own size, before the bake
        const RenderViewInfo before = Renderer3D::GetViewInfo(kMainRenderView);

        Ref<Scene> scene = GetSceneRef();
        bool valid = false;
        SHCoefficients sh;
        RunInFrame([&scene, &valid, &sh]()
                   { sh = LightProbeBaker::BakeProbeAtPosition(scene, kProbePosition, kProbeResolution, &valid); });
        ASSERT_TRUE(valid) << "the bake reported the probe invalid: it read back no light";

        const glm::vec3 towardsRed = SHBasis::EvaluateIrradiance(sh, glm::vec3(1.0f, 0.0f, 0.0f));
        const glm::vec3 towardsBlue = SHBasis::EvaluateIrradiance(sh, glm::vec3(-1.0f, 0.0f, 0.0f));
        std::cout << "[LightProbeRasterBake] L0 = (" << sh.Coefficients[0].r << ", " << sh.Coefficients[0].g << ", "
                  << sh.Coefficients[0].b << "), towards the red wall (" << towardsRed.r << ", " << towardsRed.g << ", "
                  << towardsRed.b << "), towards the blue wall (" << towardsBlue.r << ", " << towardsBlue.g << ", "
                  << towardsBlue.b << ")\n";

        // The probe saw the scene, not a clear colour.
        EXPECT_GT(sh.Coefficients[0].r, 0.01f);
        EXPECT_GT(sh.Coefficients[0].b, 0.01f);
        // And saw it the right way round: each wall on its own side. A capture
        // with a mirrored or transposed face convention swaps these.
        EXPECT_GT(towardsRed.r, towardsRed.b * 1.5f) << "the +X side of the probe is not red";
        EXPECT_GT(towardsBlue.b, towardsBlue.r * 1.5f) << "the -X side of the probe is not blue";
        EXPECT_GT(towardsRed.r, towardsBlue.r * 1.5f) << "red light does not come from the red wall's side";
        EXPECT_GT(towardsBlue.b, towardsRed.b * 1.5f) << "blue light does not come from the blue wall's side";

        // And the right way up: the sunlit floor is below the probe and nothing
        // but sky is above it. A face stored with its rows turned over puts the
        // floor in the upper half of every side face and swaps these.
        const glm::vec3 fromBelow = SHBasis::EvaluateIrradiance(sh, glm::vec3(0.0f, -1.0f, 0.0f));
        const glm::vec3 fromAbove = SHBasis::EvaluateIrradiance(sh, glm::vec3(0.0f, 1.0f, 0.0f));
        std::cout << "[LightProbeRasterBake] from below (" << fromBelow.r << ", " << fromBelow.g << ", " << fromBelow.b
                  << "), from above (" << fromAbove.r << ", " << fromAbove.g << ", " << fromAbove.b << ")\n";
        // Measured 4.1 to 1 the right way up and 1.7 to 1 with the side faces
        // turned over (the floor and sky faces themselves are only mirrored by
        // it, so the swap is partial); both backends agree to six digits.
        EXPECT_GT(fromBelow.g, fromAbove.g * 3.0f) << "the lit floor is not below the probe: the faces are upside down";

        // The bake renders through the main view at the probe's size and puts
        // the live size back.
        const RenderViewInfo after = Renderer3D::GetViewInfo(kMainRenderView);
        EXPECT_EQ(after.Width, before.Width) << "the bake left the live view at the probe's size";
        EXPECT_EQ(after.Height, before.Height);
    }

    // The size a bake puts back is the view's configured one. It was once read
    // from the SceneColor target, and a view does not always have one: not on
    // the first capture of a session, and not between a rendering-path switch
    // and the next frame, when the graph has yet to be compiled again. With no
    // target there was no size to put back and the live view stayed at the
    // probe's 32 x 32.
    TEST_F(LightProbeRasterBakeTest, ABakeWhileTheViewHasNoTargetLeavesTheViewItsSize)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        Time::SetMockTime(kCaptureTime);
        struct ClearMockTime
        {
            ~ClearMockTime()
            {
                Time::ClearMockTime();
            }
        } const clearMockTime;

        constexpr u32 kResized = 160u;
        RunFrames(2);
        ResizeRenderTarget(kResized, kResized);
        // The other path: the graph is not compiled for it until a frame renders.
        auto& settings = Renderer3D::GetRendererSettings();
        settings.Path = settings.Path == RenderingPath::Deferred ? RenderingPath::Forward : RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();
        ASSERT_EQ(Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor), nullptr)
            << "the view still has a SceneColor target after a path switch: this is no longer the case the test "
               "is for, and it needs another way to reach a view with no target";
        const RenderViewInfo before = Renderer3D::GetViewInfo(kMainRenderView);
        ASSERT_EQ(before.Width, kResized);

        Ref<Scene> scene = GetSceneRef();
        bool valid = false;
        RunInFrame([&scene, &valid]()
                   { (void)LightProbeBaker::BakeProbeAtPosition(scene, kProbePosition, kProbeResolution, &valid); });
        EXPECT_TRUE(valid);

        const RenderViewInfo after = Renderer3D::GetViewInfo(kMainRenderView);
        EXPECT_EQ(after.Width, kResized) << "the bake did not put the live view's configured size back";
        EXPECT_EQ(after.Height, kResized);
    }
} // namespace OloEngine::Tests
