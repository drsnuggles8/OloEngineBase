#include "OloEnginePCH.h"

// =============================================================================
// LuaReadsTransformOfAnotherEntityTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene tick × LuaScriptEngine × LuaScriptGlue `entity_utils.find_by_name`
//   + `get_translation` × cross-entity Scene::FindEntityByName lookup ×
//   Scene::GetEntityByUUID resolution. A common gameplay pattern is "agent
//   tracks target": a script on entity A reads entity B's position. That
//   crosses the scripting/scene boundary twice (name → UUID, UUID →
//   TransformComponent). If `find_by_name` mis-converts the entity handle
//   or if `get_translation` doesn't see entities outside the script's own
//   ownership, this seam breaks silently.
//
// Scenario: two entities. "Tracker" runs a Lua script that on first
// OnUpdate calls find_by_name("Target") and then get_translation on the
// returned handle, copying the target's x into the tracker's own
// transform.x. After one tick the tracker's x should equal the target's
// x (3.5). Verifies the round-trip Name→Entity→UUID→Transform without
// either entity having to be the script's own entity.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <cmath>
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

class LuaReadsTransformOfAnotherEntityTest : public FunctionalTest
{
  protected:
    static constexpr f32 kTargetX = 3.5f;

    void BuildScene() override
    {
        // Use small UUIDs so sol2's u64→Lua-double coercion stays exact —
        // see LuaRaycastHitsPhysicsBodyTest for the same gotcha.
        m_Target = GetScene().CreateEntityWithUUID(UUID{ 1234 }, "Target");
        m_Target.GetComponent<TransformComponent>().Translation = { kTargetX, 0.0f, 0.0f };

        m_Tracker = GetScene().CreateEntityWithUUID(UUID{ 5678 }, "Tracker");
        m_Tracker.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 0.0f };

        EnableLua();

        // Lua script on Tracker: find Target by name, read its translation,
        // write the x into Tracker's own translation.
        // find_by_name returns the target's UUID as a u64 directly (not an
        // Entity wrapper), so get_translation takes that u64 verbatim. The
        // UUIDs we picked are small enough for sol2's u64→Lua-double
        // conversion to be exact.
        const std::string scriptSrc = R"(
local script = {}
local applied = false
function script.OnUpdate(entityID, ts)
    if applied then return end
    local targetID = entity_utils.find_by_name("Target")
    if targetID ~= nil then
        local targetPos = entity_utils.get_translation(math.tointeger(targetID) or 0)
        if targetPos ~= nil then
            local myId = math.tointeger(entityID) or 0
            entity_utils.set_translation(myId, vec3.new(targetPos.x, 0, 0))
            applied = true
        end
    end
end
return script
)";
        m_ScriptPath = WriteScript(scriptSrc, "track_target");
        RegisterLuaScript(m_Tracker, m_ScriptPath);
    }

    void TearDown() override
    {
        FunctionalTest::TearDown();
        std::error_code ec;
        std::filesystem::remove(m_ScriptPath, ec);
    }

    Entity m_Target;
    Entity m_Tracker;
    std::filesystem::path m_ScriptPath;
};

TEST_F(LuaReadsTransformOfAnotherEntityTest, TrackerScriptCopiesTargetXIntoOwnTranslation)
{
    RunFrames(1);

    const auto& tt = m_Tracker.GetComponent<TransformComponent>().Translation;
    EXPECT_TRUE(std::isfinite(tt.x));
    EXPECT_NEAR(tt.x, kTargetX, 1e-3f)
        << "tracker.x didn't pick up target.x — either find_by_name returned "
           "nil for an existing entity, GetUUID()/get_translation roundtrip "
           "lost precision, or set_translation wrote to the wrong entity.";
}
