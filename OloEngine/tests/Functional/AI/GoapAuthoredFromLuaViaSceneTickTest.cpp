#include "OloEnginePCH.h"

// =============================================================================
// GoapAuthoredFromLuaViaSceneTickTest — Functional Test.
//
// Cross-subsystem seam under test:
//   LuaScriptGlue GOAP authoring API × GoapAgentComponent × AISystem × Scene
//   tick. The unit/Functional GOAP tests build the agent in C++; this proves
//   the *Lua* authoring path — goap:AddAction{...}, goap:AddGoal{...},
//   goap:SetWorldFactBool(...) — actually constructs a working brain that the
//   AI system then plans and executes through a real Scene::OnUpdateRuntime.
//   This is the path the editor's playable sample (LuaGoapHungryNPC.lua) uses,
//   so a regression here means "add a GOAP NPC, press Play, nothing happens".
//
// Scenario: an entity with a GoapAgentComponent whose Lua OnCreate defines two
// actions (Prep -> Finish, the first via a perform callback) and a goal
// (done=true). After a few ticks the agent should have planned the two-step
// chain, executed it, and driven its world state to the goal.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/AI/AIComponents.h"
#include "OloEngine/AI/GOAP/GoapWorldState.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scripting/Lua/LuaScriptEngine.h"

#include <filesystem>
#include <fstream>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    std::filesystem::path WriteScript(const std::string& contents, const char* stem)
    {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string name = std::string("olo_functional_") + stem + "_" + (info ? info->name() : "x") + ".lua";
        const auto path = std::filesystem::temp_directory_path() / name;
        std::ofstream(path, std::ios::binary | std::ios::trunc) << contents;
        return path;
    }
} // namespace

class GoapAuthoredFromLuaViaSceneTickTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // Small UUID keeps sol2's u64→Lua-double coercion exact.
        m_Agent = GetScene().CreateEntityWithUUID(UUID{ 4242 }, "Planner");
        m_Agent.AddComponent<GoapAgentComponent>();

        EnableLua();

        const std::string scriptSrc = R"(
local script = {}
function script.OnCreate(entityID)
    local goap = entity_utils.get_component(math.tointeger(entityID) or 0, "GoapAgentComponent")
    if not goap then return end
    goap:AddAction{ name = "Prep",   cost = 1.0, pre = {},                effects = { prepped = true },
                    perform = function(dt) return GoapStatus.Success end }
    goap:AddAction{ name = "Finish", cost = 1.0, pre = { prepped = true }, effects = { done = true } }
    goap:AddGoal{ name = "GetDone", priority = 2.0, desired = { done = true } }
    goap:SetWorldFactBool("done", false)
end
function script.OnDestroy(entityID)
    local goap = entity_utils.get_component(math.tointeger(entityID) or 0, "GoapAgentComponent")
    if goap then goap:ClearAgent() end
end
return script
)";
        m_ScriptPath = WriteScript(scriptSrc, "goap_author");
        RegisterLuaScript(m_Agent, m_ScriptPath);
    }

    void TearDown() override
    {
        FunctionalTest::TearDown();
        std::error_code ec;
        std::filesystem::remove(m_ScriptPath, ec);
    }

    Entity m_Agent;
    std::filesystem::path m_ScriptPath;
};

TEST_F(GoapAuthoredFromLuaViaSceneTickTest, LuaBuiltAgentPlansAndReachesGoal)
{
    // OnCreate (run at RegisterLuaScript) should have built the brain.
    auto& gac = m_Agent.GetComponent<GoapAgentComponent>();
    ASSERT_NE(gac.RuntimeAgent, nullptr)
        << "Lua OnCreate did not build a RuntimeAgent — goap:AddAction/AddGoal binding is broken";
    EXPECT_EQ(gac.RuntimeAgent->Actions().size(), 2u);
    EXPECT_EQ(gac.RuntimeAgent->Goals().size(), 1u);

    // Two instant actions complete in two ticks; a few extra frames are slack.
    RunFrames(/*count=*/5);

    GoapWorldState goal;
    goal.Set("done", true);
    EXPECT_TRUE(gac.RuntimeAgent->WorldState().Satisfies(goal))
        << "Lua-authored GOAP agent did not reach its goal through the Scene tick";
    EXPECT_GE(gac.RuntimeAgent->GoalsAchieved(), 1u);
}

// Regression: a Lua-built agent's actions hold sol::protected_function callbacks.
// If they outlive lua_close they crash in luaL_unref (observed when the editor
// is closed mid-play). LuaScriptEngine::OnRuntimeStop — the chokepoint both the
// editor (Scene::OnRuntimeStop calls it) and this harness's teardown go through
// — must release every Lua-built RuntimeAgent while the state is still alive,
// independent of the script's own OnDestroy.
TEST_F(GoapAuthoredFromLuaViaSceneTickTest, RuntimeStopReleasesLuaBuiltAgentBeforeStateTeardown)
{
    auto& gac = m_Agent.GetComponent<GoapAgentComponent>();
    ASSERT_NE(gac.RuntimeAgent, nullptr) << "precondition: Lua OnCreate built the agent";

    LuaScriptEngine::OnRuntimeStop();

    EXPECT_EQ(gac.RuntimeAgent, nullptr)
        << "Lua-built RuntimeAgent survived runtime stop — its sol callbacks "
           "could outlive lua_close and crash on destruction";
}
