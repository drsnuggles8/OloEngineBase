#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// A scene starts, ticks and stops in a host with no Application and no C#
// engine (found while working on #1796).
//
// The editor, OloRuntime and OloServer all construct an Application and
// initialise ScriptEngine before any scene runs, so neither was ever absent on
// the paths users reach. A tool or a test that starts a scene has neither:
// Scene::OnRuntimeStart read the run seed and the headless flag through
// Application::Get(), and ScriptEngine dereferenced its null s_Data when the
// scene started, created a scripted entity, ticked it or stopped. The
// lifecycle soak was the first rendered test to start a scene and took an
// access violation on its first Play. The contract: such a host is a windowed
// host on the default seed, and a script whose engine is absent does not run.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Core/Application.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/Scripting/Lua/LuaScriptEngine.h"

namespace OloEngine::Tests
{
    TEST(SceneStartWithoutHost, AScriptedSceneStartsTicksAndStopsWithNoApplicationOrCSharpEngine)
    {
        if (Application::TryGet() != nullptr)
            GTEST_SKIP() << "this process constructed an Application; the case needs a host without one";

        // Lua is what every host initialises unconditionally; its Init is
        // once per process and safe to repeat.
        LuaScriptEngine::Init();

        Ref<Scene> scene = Scene::Create();
        scene->SetRenderingEnabled(false);
        Entity scripted = scene->CreateEntity("Scripted");
        scripted.AddComponent<ScriptComponent>().ClassName = "Game.NotLoadedInThisHost";

        scene->OnRuntimeStart();
        EXPECT_TRUE(scene->IsRunning());
        EXPECT_EQ(ScriptEngine::GetSceneContext(), scene.Raw())
            << "the Lua bindings read the running scene through the C# engine's context";

        scene->OnUpdateRuntime(Timestep{ 1.0f / 60.0f });
        EXPECT_FALSE(ScriptEngine::GetEntityScriptInstance(scripted.GetUUID()))
            << "a script whose class no engine loaded must not have an instance";

        scene->OnRuntimeStop();
        EXPECT_FALSE(scene->IsRunning());
        EXPECT_EQ(ScriptEngine::GetSceneContext(), nullptr) << "Stop left the scene as the script context";
    }
} // namespace OloEngine::Tests
