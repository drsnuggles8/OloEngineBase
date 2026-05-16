#include "OloEnginePCH.h"

// =============================================================================
// EmptyTickReproTest — diagnostic-only Functional-test reproducer.
// Bisects the harness hang: does Scene::OnUpdateRuntime on an EMPTY scene
// also hang? If yes, the issue is in the scene tick infrastructure itself,
// not in animation/physics. Delete this file once the harness hang is fixed.
// =============================================================================

#include "Functional/FunctionalTest.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include <cstdio>

using namespace OloEngine;
using namespace OloEngine::Functional;

class EmptyTickReproTest : public FunctionalTest
{
  protected:
    void BuildScene() override {}
};

TEST_F(EmptyTickReproTest, EmptySceneSingleTickReturns)
{
    RunFrames(1);
    SUCCEED();
}

class CameraOnlyTickTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        auto camera = GetScene().CreateEntity("Camera");
        auto& cam = camera.AddComponent<CameraComponent>();
        cam.Primary = true;
    }
};

TEST_F(CameraOnlyTickTest, CameraOnlySceneTickReturns)
{
    RunFrames(1);
    SUCCEED();
}

class SpriteOnlyTickTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        auto e = GetScene().CreateEntity("S");
        auto& sprite = e.AddComponent<SpriteRendererComponent>();
        sprite.Color = { 1.0f, 0.0f, 0.0f, 1.0f };
    }
};

TEST_F(SpriteOnlyTickTest, SpriteOnlySceneTickReturns)
{
    RunFrames(1);
    SUCCEED();
}
