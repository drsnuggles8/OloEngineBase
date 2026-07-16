#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
// =============================================================================
// LuaGrantsExperienceViaSceneTickTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene tick × LuaScriptEngine::OnUpdateEntity × LuaScriptGlue's
//   `ProgressionComponent` usertype (GrantExperience routed through
//   ProgressionSystem when a scene context resolves) × the "Progression"
//   scheduler node draining PendingXP into a level-up. A Lua script pulls
//   the ProgressionComponent off its entity through
//   `entity_utils.get_component(id, "ProgressionComponent")` and calls
//   `pc:GrantExperience(250)` exactly once. If the binding writes to a copy,
//   if the usertype's mutation path bypasses PendingXP, or if the
//   Progression system stops running inside Scene::OnUpdateRuntime, the
//   level never rises.
//
// Numbers (engine-default curve, no assets): 250 XP at L1 (needs 100) ->
// Level 2 with 150 carried (level 2 needs 200 > 150, so it stops there).
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Gameplay/Progression/ProgressionComponents.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    std::filesystem::path WriteScript(const std::string& contents, const char* nameStem)
    {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string fileName = std::string("olo_functional_") + nameStem + "_" + (info ? info->name() : "unknown") + ".lua";
        const auto path = std::filesystem::temp_directory_path() / fileName;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
            throw std::runtime_error("WriteScript: failed to open temp file for writing: " + path.string());
        out << contents;
        out.flush();
        if (!out.good())
            throw std::runtime_error("WriteScript: write failed for: " + path.string());
        return path;
    }
} // namespace

class LuaGrantsExperienceViaSceneTickTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // Small UUID so the sol2 u64 -> Lua double conversion is exact.
        m_Entity = GetScene().CreateEntityWithUUID(UUID{ 4242 }, "Adventurer");
        m_Entity.AddComponent<ProgressionComponent>(); // handles 0 => default curve

        EnableLua();

        // Grant 250 XP exactly once on the first OnUpdate that can resolve
        // the component; every later tick is a no-op.
        const std::string scriptSrc = R"(
local script = {}
local granted = false
function script.OnUpdate(entityID, ts)
    if granted then return end
    local id = math.tointeger(entityID) or 0
    local pc = entity_utils.get_component(id, "ProgressionComponent")
    if pc ~= nil then
        pc:GrantExperience(250)
        granted = true
    end
end
return script
)";
        m_ScriptPath = WriteScript(scriptSrc, "grant_xp");
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

TEST_F(LuaGrantsExperienceViaSceneTickTest, LuaGrantedXPResolvesIntoLevelUpOnSceneTicks)
{
    const auto& comp = m_Entity.GetComponent<ProgressionComponent>();
    ASSERT_EQ(comp.Level, 1);
    ASSERT_EQ(comp.PendingXP, 0);

    // Tick 1: the Lua OnUpdate grants 250 into PendingXP. Whether the
    // Progression node runs before or after the script inside that same
    // tick, one further tick definitely drains it.
    const bool leveled = TickUntil([&]
                                   { return comp.Level >= 2; }, /*timeoutSeconds=*/1.0f);

    EXPECT_TRUE(leveled)
        << "Lua's GrantExperience never resolved into a level-up — the "
           "ProgressionComponent usertype is mutating a copy, PendingXP is "
           "bypassed, or the Progression scheduler node stopped ticking.";
    EXPECT_EQ(comp.Level, 2)
        << "250 XP on the default curve must stop at level 2 (needs 200 more for level 3)";
    EXPECT_EQ(comp.CurrentXP, 150) << "250 - 100 (L1 cost) = 150 must carry";
    EXPECT_EQ(comp.PendingXP, 0) << "the drain must consume the whole grant";

    // The script grants exactly once — further ticks must not re-grant.
    RunFrames(5);
    EXPECT_EQ(comp.Level, 2) << "the one-shot script must not keep granting XP";
    EXPECT_EQ(comp.CurrentXP, 150);
}
