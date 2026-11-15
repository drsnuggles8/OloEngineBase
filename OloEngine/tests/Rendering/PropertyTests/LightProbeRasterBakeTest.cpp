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
// Classification: L8 (the real Scene render path, six faces, readback). SKIPs
// cleanly without a GPU context. OpenGL process only, like every bake test:
// the bake's GL state is put back with GLStateGuard.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/Debug/GLStateGuard.h"
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
        {
            GLStateGuard guard("LightProbeRasterBake", GLStateGuard::Policy::Restore);
            sh = LightProbeBaker::BakeProbeAtPosition(scene, kProbePosition, kProbeResolution, &valid);
        }
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

        // The bake renders through the main view at the probe's size and puts
        // the live size back.
        const RenderViewInfo after = Renderer3D::GetViewInfo(kMainRenderView);
        EXPECT_EQ(after.Width, before.Width) << "the bake left the live view at the probe's size";
        EXPECT_EQ(after.Height, before.Height);
    }
} // namespace OloEngine::Tests
