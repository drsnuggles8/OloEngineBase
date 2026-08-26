#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
//
// =============================================================================
// RuntimeSceneSwitchTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene lifecycle × Lua scripting × scene serialization × the host's
//   end-of-frame transition servicing (issue #642).
//
// Runtime scene switching is the main-menu -> level -> next-level primitive.
// The request comes from a script running INSIDE the scene that is about to be
// destroyed, so it cannot be honoured where it is made: `Scene::UpdateScripts`
// is iterating the script component pools, and the swap tears down the very
// registry that iteration walks. So a request is recorded on the Scene and the
// HOST applies it once the tick has returned — the same deferral
// `SetPendingReload` has always used, generalized from "reload this path" to
// "load an arbitrary one".
//
// That split is exactly what makes the feature easy to get wrong in a way no
// single-layer test catches: the script glue can be perfect while the host
// swaps too early, or swaps without re-initializing scripting for the incoming
// scene, or re-fires the request it just serviced and bounces between two
// scenes forever. This test drives the whole loop through a real
// `Scene::OnUpdateRuntime` and asserts:
//
//   * a script's LoadScene is DEFERRED — the old scene is still the live one
//     when the tick returns, with all its entities intact,
//   * the host swap replaces it with the requested scene, loaded from disk,
//   * scripting is re-initialized for the incoming scene (its scripts run),
//   * the serviced request does not survive into the new scene — otherwise the
//     first tick after a switch would request the same switch again,
//   * a switch to a scene that does not exist leaves the current scene RUNNING
//     rather than dropping the player into a torn-down state,
//   * a chained switch works, so menu -> level -> next-level is reachable.
//
// The host emulated below mirrors RuntimeLayer::OnUpdate + ActivateScene
// (OloRuntimeApp.cpp) and the editor's Play-mode branch, minus the pieces that
// need a live Application. C# `SceneManager.LoadScene` is the same one-line
// request into `Scene::SetPendingSceneLoad`; Lua drives it here because the
// test binary has no Mono runtime.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Scene/SceneTransition.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/Scripting/Lua/LuaScriptEngine.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    // A script that asks for one scene switch and then stops asking, so a
    // re-fired request shows up as an extra switch rather than being masked by
    // a script that would have requested it again anyway.
    [[nodiscard]] std::string SwitchOnceScript(const std::string& target)
    {
        return R"(
local script = {}
script.done = false
function script.OnUpdate(entityID, ts)
    if script.done then return end
    script.done = true
    Scene.LoadScene(")" +
               target + R"(")
end
return script
)";
    }

    // Stamps the entity it runs on, so C++ can tell whether the incoming
    // scene's scripts were actually brought up by the swap.
    constexpr const char* kStampScript = R"(
local script = {}
function script.OnUpdate(entityID, ts)
    entity_utils.set_translation(entityID, vec3.new(99.0, 0.0, 0.0))
end
return script
)";

    [[nodiscard]] sizet CountEntitiesNamed(Scene& scene, const std::string& tag)
    {
        sizet count = 0;
        for (auto view = scene.GetAllEntitiesWith<TagComponent>(); auto e : view)
        {
            if (view.get<TagComponent>(e).Tag == tag)
                ++count;
        }
        return count;
    }
} // namespace

class RuntimeSceneSwitchTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // A project has to be mounted: the host resolves scene requests against
        // the project asset directory, and Lua script files are resolved
        // through Project::GetAssetFileSystemPath exactly as in production.
        EnableAssetManager({});
        EnableLua();

        std::error_code ec;
        std::filesystem::create_directories(AssetDir() / "Scenes", ec);
        std::filesystem::create_directories(AssetDir() / "Scripts", ec);
        ASSERT_FALSE(ec) << "could not create the temp project's Scenes/Scripts directories.";

        // The harness's scene stands in for the start scene ("MainMenu").
        GetScene().SetName("MainMenu");
        m_Active = GetSceneRef();

        Entity camera = GetScene().CreateEntity("MenuCamera");
        camera.AddComponent<CameraComponent>().Primary = true;
        m_MenuMarker = GetScene().CreateEntity("MenuMarker");
    }

    void TearDown() override
    {
        // FunctionalTest::TearDown stops the Lua engine, which dereferences the
        // scene it is bound to — so it has to run while m_Active is still alive.
        FunctionalTest::TearDown();
        m_Active.Reset();
    }

    [[nodiscard]] std::filesystem::path AssetDir() const
    {
        return Project::GetAssetDirectory();
    }

    /// Write a Lua file under the project's Scripts/ and return its
    /// project-relative path, the form a scene file stores.
    std::string WriteScript(const std::string& contents, const std::string& stem)
    {
        const std::string relative = "Scripts/" + stem + ".lua";
        std::ofstream out(AssetDir() / relative, std::ios::binary | std::ios::trunc);
        out << contents;
        return relative;
    }

    /// Author a scene file on disk: a primary camera, a uniquely named marker
    /// entity, and optionally a scripted entity. Returns the file path.
    std::filesystem::path WriteSceneFile(const std::string& name,
                                         const std::string& markerName,
                                         const std::string& scriptRelativePath = {})
    {
        Ref<Scene> authored = Scene::Create();
        authored->SetRenderingEnabled(false);
        authored->SetName(name);

        Entity camera = authored->CreateEntity(name + "Camera");
        camera.AddComponent<CameraComponent>().Primary = true;
        (void)authored->CreateEntity(markerName);

        if (!scriptRelativePath.empty())
        {
            Entity scripted = authored->CreateEntity(name + "Script");
            scripted.AddComponent<LuaScriptComponent>().ScriptFile = scriptRelativePath;
        }

        const auto path = AssetDir() / "Scenes" / (name + ".olo");
        SceneSerializer(authored).Serialize(path);
        return path;
    }

    /// Attach a Lua script to an entity in the CURRENTLY active scene.
    void AttachScript(Entity entity, const std::string& contents, const std::string& stem)
    {
        const std::string relative = WriteScript(contents, stem);
        entity.AddComponent<LuaScriptComponent>().ScriptFile = relative;
        LuaScriptEngine::OnCreateEntity(entity, Project::GetAssetFileSystemPath(relative).string());
    }

    void Tick(u32 frames = 1, f32 dt = 1.0f / 60.0f)
    {
        const Timestep ts{ dt };
        for (u32 i = 0; i < frames; ++i)
        {
            m_Active->OnUpdateRuntime(ts);
        }
    }

    /// The host's end-of-frame transition servicing, mirroring
    /// RuntimeLayer::OnUpdate + RuntimeLayer::ActivateScene. Returns true when a
    /// switch actually happened; on failure the current scene is left running,
    /// which is the production policy for a bad scene name from script.
    bool ServicePendingTransition()
    {
        m_LastError.clear();
        if (!m_Active->HasPendingSceneLoad())
        {
            return false;
        }

        const std::string request = m_Active->GetPendingSceneLoad();
        m_Active->ClearPendingSceneLoad();

        const auto resolved = SceneTransition::ResolveScenePath(request, AssetDir());
        if (resolved.empty())
        {
            m_LastError = "could not resolve scene request '" + request + "'";
            return false;
        }

        // Load and validate BEFORE stopping the running scene, so a bad target
        // costs nothing.
        auto loaded = SceneTransition::LoadSceneFile(resolved, /*requirePrimaryCamera=*/true);
        if (!loaded)
        {
            m_LastError = loaded.Error;
            return false;
        }

        // The script engines share ONE process-wide scene context, so the
        // outgoing scene must release it before the incoming one claims it.
        m_Active->SetRunning(false);
        LuaScriptEngine::OnRuntimeStop();

        m_Active = loaded.LoadedScene;
        m_Active->SetRenderingEnabled(false);

        LuaScriptEngine::OnRuntimeStart(m_Active.Raw());
        ScriptEngine::SetSceneContextForTesting(m_Active.Raw());
        m_Active->SetRunning(true);

        // Production does this inside Scene::OnRuntimeStart; the headless
        // harness can't call that (it needs Application::Get()), so the host
        // emulation does the same sweep here.
        for (auto view = m_Active->GetAllEntitiesWith<LuaScriptComponent>(); auto e : view)
        {
            const auto& component = view.get<LuaScriptComponent>(e);
            if (component.ScriptFile.empty())
            {
                continue;
            }
            LuaScriptEngine::OnCreateEntity(Entity{ e, m_Active.Raw() },
                                            Project::GetAssetFileSystemPath(component.ScriptFile).string());
        }

        ++m_SwitchCount;
        return true;
    }

    Ref<Scene> m_Active;
    Entity m_MenuMarker;
    std::string m_LastError;
    u32 m_SwitchCount = 0;
};

// -----------------------------------------------------------------------------
// The core contract: requested from script, applied by the host, not mid-tick.
// -----------------------------------------------------------------------------
TEST_F(RuntimeSceneSwitchTest, ScriptRequestIsRecordedButNotAppliedDuringTheTick)
{
    WriteSceneFile("Level1", "Level1Marker");
    AttachScript(m_MenuMarker, SwitchOnceScript("Level1"), "menu_switch");

    Tick();

    // The scene the script ran in must still be the live one when the tick
    // returns — applying the swap inline would have destroyed the registry
    // Scene::UpdateScripts was iterating.
    EXPECT_EQ(m_Active->GetName(), "MainMenu")
        << "the scene was swapped from inside the tick that requested it.";
    EXPECT_EQ(CountEntitiesNamed(*m_Active, "MenuMarker"), 1u)
        << "the requesting scene lost entities before the host serviced the request.";
    EXPECT_TRUE(m_Active->HasPendingSceneLoad())
        << "the script's LoadScene call left no request for the host to service.";
    EXPECT_EQ(m_Active->GetPendingSceneLoad(), "Level1");
}

TEST_F(RuntimeSceneSwitchTest, LuaContinueRequestRecordsSceneAndSaveSlotForTheHost)
{
    AttachScript(m_MenuMarker, R"(
local script = { done = false }
function script.OnUpdate(entityID, ts)
    if script.done then return end
    script.done = true
    Scene.LoadSceneFromSave("Drift", "drift_voyage")
end
return script
)",
                 "menu_continue");

    Tick();

    ASSERT_TRUE(m_Active->HasPendingSceneLoad());
    EXPECT_EQ(m_Active->GetPendingSceneLoad(), "Drift");
    EXPECT_EQ(m_Active->GetPendingSceneLoadSaveSlot(), "drift_voyage");
}

TEST_F(RuntimeSceneSwitchTest, HostSwapReplacesTheSceneAndTheOldOneIsGone)
{
    WriteSceneFile("Level1", "Level1Marker");
    AttachScript(m_MenuMarker, SwitchOnceScript("Level1"), "menu_switch");

    Tick();
    ASSERT_TRUE(ServicePendingTransition()) << "the host failed to service the request: " << m_LastError;

    EXPECT_EQ(m_Active->GetName(), "Level1.olo");
    EXPECT_EQ(CountEntitiesNamed(*m_Active, "Level1Marker"), 1u)
        << "the requested scene's entities are missing — the swap installed the wrong scene, or an "
           "empty one.";
    EXPECT_EQ(CountEntitiesNamed(*m_Active, "MenuMarker"), 0u)
        << "an entity from the outgoing scene survived into the new one — this is a hard cut, not a "
           "merge.";
    EXPECT_TRUE(static_cast<bool>(m_Active->GetPrimaryCameraEntity()))
        << "the new scene has no primary camera, so the game would render nothing.";

    // And the new scene is genuinely live, not just installed.
    Tick(5);
    EXPECT_TRUE(m_Active->IsRunning());
}

TEST_F(RuntimeSceneSwitchTest, ScriptsInTheIncomingSceneAreBroughtUpByTheSwap)
{
    // Level1 carries its own scripted entity. Nothing in the outgoing scene can
    // stamp it, so the sentinel can only appear if the swap re-initialized
    // scripting against the incoming scene.
    const std::string stampScript = WriteScript(kStampScript, "level1_stamp");
    WriteSceneFile("Level1", "Level1Marker", stampScript);
    AttachScript(m_MenuMarker, SwitchOnceScript("Level1"), "menu_switch");

    Tick();
    ASSERT_TRUE(ServicePendingTransition()) << m_LastError;

    Entity scripted = m_Active->FindEntityByName("Level1Script");
    ASSERT_TRUE(static_cast<bool>(scripted)) << "the incoming scene's scripted entity did not load.";
    ASSERT_FLOAT_EQ(scripted.GetComponent<TransformComponent>().Translation.x, 0.0f);

    Tick();

    EXPECT_FLOAT_EQ(scripted.GetComponent<TransformComponent>().Translation.x, 99.0f)
        << "a script in the scene we switched TO never ran — the swap installed the scene but did "
           "not bring scripting up for it, so every gameplay script in every level after the first "
           "would be dead.";
}

// -----------------------------------------------------------------------------
// The serviced request must not survive the swap, or the game bounces forever.
// -----------------------------------------------------------------------------
TEST_F(RuntimeSceneSwitchTest, AServicedRequestDoesNotRefireOnTheNewScene)
{
    WriteSceneFile("Level1", "Level1Marker");
    AttachScript(m_MenuMarker, SwitchOnceScript("Level1"), "menu_switch");

    Tick();
    ASSERT_TRUE(ServicePendingTransition()) << m_LastError;
    ASSERT_EQ(m_SwitchCount, 1u);

    // Several ticks on the new scene, servicing after each one exactly as the
    // host does. Nothing in Level1 asks for a switch, so nothing should happen.
    for (u32 i = 0; i < 5; ++i)
    {
        Tick();
        (void)ServicePendingTransition();
    }

    EXPECT_EQ(m_SwitchCount, 1u)
        << "the switch request re-fired after being serviced — the game would ping-pong between "
           "scenes every frame.";
    EXPECT_FALSE(m_Active->HasPendingSceneLoad());
    EXPECT_EQ(m_Active->GetName(), "Level1.olo");
}

// -----------------------------------------------------------------------------
// menu -> level -> next-level, the shape the issue is actually about.
// -----------------------------------------------------------------------------
TEST_F(RuntimeSceneSwitchTest, ChainedSwitchesReachTheThirdScene)
{
    WriteSceneFile("Level2", "Level2Marker");
    const std::string toLevel2 = WriteScript(SwitchOnceScript("Level2"), "level1_switch");
    WriteSceneFile("Level1", "Level1Marker", toLevel2);
    AttachScript(m_MenuMarker, SwitchOnceScript("Level1"), "menu_switch");

    Tick();
    ASSERT_TRUE(ServicePendingTransition()) << m_LastError;
    ASSERT_EQ(m_Active->GetName(), "Level1.olo");

    // Level1's own script now asks for Level2 — a switch requested from a scene
    // that was itself switched into, which is the case a "reload the start
    // scene" implementation gets wrong.
    Tick();
    ASSERT_TRUE(ServicePendingTransition()) << m_LastError;

    EXPECT_EQ(m_Active->GetName(), "Level2.olo");
    EXPECT_EQ(CountEntitiesNamed(*m_Active, "Level2Marker"), 1u);
    EXPECT_EQ(CountEntitiesNamed(*m_Active, "Level1Marker"), 0u);
    EXPECT_EQ(m_SwitchCount, 2u);
}

// -----------------------------------------------------------------------------
// Failure policy: a bad target is a content bug, not a reason to stop the game.
// -----------------------------------------------------------------------------
TEST_F(RuntimeSceneSwitchTest, AnUnknownSceneLeavesTheCurrentSceneRunning)
{
    WriteSceneFile("Level1", "Level1Marker");
    AttachScript(m_MenuMarker, SwitchOnceScript("Levle1"), "menu_typo");

    Tick();
    ASSERT_TRUE(m_Active->HasPendingSceneLoad());

    EXPECT_FALSE(ServicePendingTransition()) << "a typo'd scene name was resolved to something.";
    EXPECT_FALSE(m_LastError.empty()) << "a failed switch must report why.";

    EXPECT_EQ(m_Active->GetName(), "MainMenu");
    EXPECT_EQ(CountEntitiesNamed(*m_Active, "MenuMarker"), 1u)
        << "the current scene was torn down for a switch that never happened.";
    EXPECT_TRUE(m_Active->IsRunning());

    // The failed request must be consumed, not retried every frame.
    EXPECT_FALSE(m_Active->HasPendingSceneLoad());

    // And the scene keeps ticking normally afterwards.
    Tick(5);
    EXPECT_EQ(CountEntitiesNamed(*m_Active, "MenuMarker"), 1u);
}

TEST_F(RuntimeSceneSwitchTest, ASceneWithNoPrimaryCameraIsRefusedAndTheGameKeepsRunning)
{
    // Author a scene by hand without a camera — the host refuses it rather than
    // switching the player into a black screen.
    {
        Ref<Scene> authored = Scene::Create();
        authored->SetRenderingEnabled(false);
        authored->SetName("Blind");
        (void)authored->CreateEntity("BlindMarker");
        SceneSerializer(authored).Serialize(AssetDir() / "Scenes" / "Blind.olo");
    }

    AttachScript(m_MenuMarker, SwitchOnceScript("Blind"), "menu_blind");

    Tick();
    EXPECT_FALSE(ServicePendingTransition())
        << "a scene with no primary camera was accepted as a switch target.";
    EXPECT_FALSE(m_LastError.empty());
    EXPECT_EQ(m_Active->GetName(), "MainMenu");
    EXPECT_TRUE(m_Active->IsRunning());
}
