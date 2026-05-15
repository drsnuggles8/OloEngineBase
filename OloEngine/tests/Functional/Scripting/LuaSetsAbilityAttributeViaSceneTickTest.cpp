#include "OloEnginePCH.h"

// =============================================================================
// LuaSetsAbilityAttributeViaSceneTickTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene tick × LuaScriptEngine::OnUpdateEntity × LuaScriptGlue's
//   `AbilityComponent` usertype × AttributeSet base-value setter ×
//   GameplayAbilitySystem::OnUpdate (Alive→Dead transition).
//   A Lua script reads the AbilityComponent off an entity through
//   `entity_utils.get_component(id, "AbilityComponent")`, then mutates
//   Health to 0 via `comp:SetAttribute("Health", 0)`. The very next
//   Scene tick is responsible for noticing the zero-HP state and
//   flipping the owner's State.Alive → State.Dead tags. If the Lua
//   binding writes only to a copy (forgetting to take the component by
//   reference), or if the per-frame death check skips when Health was
//   externally mutated, the entity stays Alive despite being dead.
//
// Scenario: one entity with AbilityComponent (Health=100, State.Alive).
// A Lua script on first OnUpdate calls SetAttribute("Health", 0). After
// one further Scene tick, the entity's OwnedTags must contain State.Dead
// and NOT State.Alive.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTag.h"

#include <filesystem>
#include <fstream>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    std::filesystem::path WriteScript(const std::string& contents, const char* nameStem)
    {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string fileName = std::string("olo_functional_") + nameStem + "_" + (info ? info->name() : "unknown") + ".lua";
        const auto path = std::filesystem::temp_directory_path() / fileName;
        std::ofstream(path, std::ios::binary | std::ios::trunc) << contents;
        return path;
    }
} // namespace

class LuaSetsAbilityAttributeViaSceneTickTest : public FunctionalTest
{
  protected:
    GameplayTag m_AliveTag{ "State.Alive" };
    GameplayTag m_DeadTag{ "State.Dead" };

    void BuildScene() override
    {
        // Use a small UUID so the sol2 u64 → Lua double conversion is exact.
        m_Entity = GetScene().CreateEntityWithUUID(UUID{ 4242 }, "Mortal");
        auto& ac = m_Entity.AddComponent<AbilityComponent>();
        ac.InitializeDefaultRPGAttributes(100.0f, 50.0f, 10.0f, 0.0f);
        ac.OwnedTags.AddTag(m_AliveTag);

        EnableLua();

        // The Lua script kills the entity on its FIRST OnUpdate by writing
        // Health=0 via the usertype's SetAttribute method. Subsequent ticks
        // are no-ops so we can assert before/after death transition.
        const std::string scriptSrc = R"(
local script = {}
local applied = false
function script.OnUpdate(entityID, ts)
    if applied then return end
    local id = math.tointeger(entityID) or 0
    local ac = entity_utils.get_component(id, "AbilityComponent")
    if ac ~= nil then
        ac:SetAttribute("Health", 0.0)
        applied = true
    end
end
return script
)";
        m_ScriptPath = WriteScript(scriptSrc, "kill_self");
        RegisterLuaScript(m_Entity, m_ScriptPath);
    }

    void TearDown() override
    {
        FunctionalTest::TearDown();
        std::error_code ec;
        std::filesystem::remove(m_ScriptPath, ec);
    }

    Entity m_Entity;
    std::filesystem::path m_ScriptPath;
};

TEST_F(LuaSetsAbilityAttributeViaSceneTickTest, LuaDrivenHealthToZeroFlipsAliveToDeadOnSubsequentTick)
{
    auto& ac = m_Entity.GetComponent<AbilityComponent>();
    ASSERT_NEAR(ac.Attributes.GetCurrentValue("Health"), 100.0f, 1e-3f);
    ASSERT_TRUE(ac.OwnedTags.HasTagExact(m_AliveTag));

    // Tick 1: Lua OnUpdate fires and writes Health=0. The GAS update for
    // this tick runs AFTER Lua (assuming script→GAS order in OnUpdateRuntime),
    // so by the end of this single tick the death-tag transition may
    // already be visible. Either way, two ticks must definitely see it.
    RunFrames(2);

    EXPECT_NEAR(ac.Attributes.GetCurrentValue("Health"), 0.0f, 1e-3f)
        << "Lua's SetAttribute didn't deduct Health on the component the engine "
           "sees — entity_utils.get_component is returning a copy instead of a "
           "reference, or the AbilityComponent usertype's setter writes to the "
           "wrong AttributeSet instance.";

    EXPECT_FALSE(ac.OwnedTags.HasTagExact(m_AliveTag))
        << "State.Alive not removed despite Health=0 — GAS::OnUpdate isn't seeing "
           "the Lua-driven attribute change (separate Attribute caches?) or the "
           "death-transition block was skipped.";
    EXPECT_TRUE(ac.OwnedTags.HasTagExact(m_DeadTag))
        << "State.Dead not added — half of the Alive→Dead transition completed "
           "(Alive was removed) but the new tag never landed.";
}
