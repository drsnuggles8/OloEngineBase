#include "OloEnginePCH.h"

// =============================================================================
// LuaCompletesQuestViaIncrementObjectiveTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene tick × LuaScriptEngine × LuaScriptGlue `QuestJournalComponent`
//   usertype × QuestJournal::IncrementObjective × TryAdvanceStage ×
//   auto-CompleteQuest. The Lua binding for IncrementObjective takes the
//   component BY REFERENCE (must — quest state isn't a copy-friendly
//   container) and forwards into QuestJournal which mutates m_ActiveQuests
//   in place. A regression where the usertype binds the component by
//   value (e.g. via `[](QuestJournalComponent comp, ...)` instead of
//   `[](QuestJournalComponent& comp, ...)`) silently swallows every
//   Lua-driven quest update.
//
// Scenario: a Player with a 3-required-count Kill quest. The Lua script
// calls IncrementObjective(quest, obj, 3) on first OnUpdate — that
// single Lua call should walk the threshold, advance the stage, and
// (since there's no completion choice) auto-complete the quest. After
// one Scene tick the journal must report:
//   - quest no longer Active
//   - quest in HasCompletedQuest list
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Gameplay/Quest/QuestComponents.h"
#include "OloEngine/Gameplay/Quest/Quest.h"
#include "OloEngine/Gameplay/Quest/QuestObjective.h"

#include <filesystem>
#include <fstream>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    std::filesystem::path WriteScript(const std::string& contents, const char* nameStem)
    {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string fileName = std::string("olo_functional_") + nameStem + "_"
                                   + (info ? info->name() : "unknown") + ".lua";
        const auto path = std::filesystem::temp_directory_path() / fileName;
        std::ofstream(path, std::ios::binary | std::ios::trunc) << contents;
        return path;
    }
} // namespace

class LuaCompletesQuestViaIncrementObjectiveTest : public FunctionalTest
{
  protected:
    static constexpr const char* kQuestID = "Q_KillThreeViaLua";
    static constexpr const char* kObjID = "kill_target";

    void BuildScene() override
    {
        m_Player = GetScene().CreateEntityWithUUID(UUID{ 4242 }, "Player");
        auto& jc = m_Player.AddComponent<QuestJournalComponent>();

        // Build a one-stage, 3-required Kill quest. Empty CompletionChoices
        // → auto-complete on final-stage advance.
        QuestDefinition def;
        def.QuestID = kQuestID;
        def.Title = "Three Targets";
        QuestStage stage;
        stage.StageID = "S0";
        stage.RequireAllObjectives = true;
        QuestObjective obj;
        obj.ObjectiveID = kObjID;
        obj.ObjectiveType = QuestObjective::Type::Kill;
        obj.TargetID = "target";
        obj.RequiredCount = 3;
        stage.Objectives.push_back(std::move(obj));
        def.Stages.push_back(std::move(stage));

        ASSERT_TRUE(jc.Journal.AcceptQuest(kQuestID, def));

        EnableLua();

        // Lua script: on first OnUpdate, fetch our QuestJournalComponent and
        // increment the objective by the required count in one call.
        const std::string scriptSrc = R"(
local script = {}
local applied = false
function script.OnUpdate(entityID, ts)
    if applied then return end
    local id = math.tointeger(entityID) or 0
    local jc = entity_utils.get_component(id, "QuestJournalComponent")
    if jc ~= nil then
        jc:IncrementObjective("Q_KillThreeViaLua", "kill_target", 3)
        applied = true
    end
end
return script
)";
        m_ScriptPath = WriteScript(scriptSrc, "complete_quest");
        RegisterLuaScript(m_Player, m_ScriptPath);
    }

    void TearDown() override
    {
        FunctionalTest::TearDown();
        std::error_code ec;
        std::filesystem::remove(m_ScriptPath, ec);
    }

    Entity m_Player;
    std::filesystem::path m_ScriptPath;
};

TEST_F(LuaCompletesQuestViaIncrementObjectiveTest, LuaIncrementObjectiveByThreeAutoCompletesSingleStageQuest)
{
    auto& jc = m_Player.GetComponent<QuestJournalComponent>();
    ASSERT_TRUE(jc.Journal.IsQuestActive(kQuestID));

    RunFrames(1);

    EXPECT_FALSE(jc.Journal.IsQuestActive(kQuestID))
        << "quest still listed as Active after Lua-driven IncrementObjective(3) — "
           "the Lua binding may be passing the component by value (mutations "
           "land on a temporary copy) or IncrementObjective itself rejected the "
           "increment because the QuestID/ObjectiveID lookup mismatched.";
    EXPECT_TRUE(jc.Journal.HasCompletedQuest(kQuestID))
        << "quest didn't auto-complete after final-stage objectives hit required "
           "count — TryAdvanceStage didn't fire from inside IncrementObjective.";
}
