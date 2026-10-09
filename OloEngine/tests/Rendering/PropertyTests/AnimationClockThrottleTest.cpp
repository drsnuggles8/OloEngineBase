// =============================================================================
// AnimationClockThrottleTest.cpp
//
// Found while verifying #1354. A throttled editor (EditorLayer's skipRender)
// ticks the scene without rendering between rendered frames, and every
// "rendering disabled" branch reset Scene::m_LastAnimationTime to its -1
// sentinel. The next rendered frame then re-seeded prevAnimationTime ==
// animationTime, on every frame: wind, water and foliage reported zero velocity
// for their animated displacement, and the foliage ray-tracing plan read a
// clock that never moved. Measured live on the IntegratedRenderer benchmark
// (a background, throttled editor): the previous time was -1 on 100% of
// rendered frames, and a hardware watchpoint named the reset in
// Scene::OnUpdateEditor's rendering-disabled branch.
//
// The reset was never needed: the animation clock advances only on rendered
// frames and by at most 0.1 s per frame, so a resumed frame's step is bounded.
//
// Classification: integration (real Renderer3D bring-up, editor update path).
// =============================================================================

// OLO_TEST_LAYER: integration

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kWidth = 64;
        constexpr u32 kHeight = 64;
    } // namespace

    class AnimationClockThrottleTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            Entity light = scene.CreateEntity("Sun");
            auto& dl = light.AddComponent<DirectionalLightComponent>();
            dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.45f));

            Entity cube = scene.CreateEntity("Cube");
            auto& mc = cube.AddComponent<MeshComponent>();
            mc.m_Primitive = MeshPrimitive::Cube;
            if (Ref<Mesh> mesh = MeshPrimitives::CreateCube())
                mc.m_MeshSource = mesh->GetMeshSource();
            cube.AddComponent<MaterialComponent>();
        }
    };

    TEST_F(AnimationClockThrottleTest, AnUnrenderedTickKeepsThePreviousAnimationTime)
    {
        EditorCamera camera(45.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1f, 100.0f);
        RunEditorFrames(camera, 2);
        const f32 rendered = GetScene().GetLastAnimationTime();
        ASSERT_GE(rendered, 0.0f) << "a rendered frame did not record its animation time";

        // The throttle: ticks with rendering disabled, then a rendered frame.
        GetScene().SetRenderingEnabled(false);
        RunEditorFrames(camera, 3);
        EXPECT_FLOAT_EQ(GetScene().GetLastAnimationTime(), rendered)
            << "an unrendered tick reset the previous animation time, so the next rendered frame "
               "reports zero wind, water and foliage velocity";

        GetScene().SetRenderingEnabled(true);
        RunEditorFrames(camera, 1);
        EXPECT_GT(GetScene().GetLastAnimationTime(), rendered) << "the clock did not advance on the rendered frame";
    }
} // namespace OloEngine::Tests
