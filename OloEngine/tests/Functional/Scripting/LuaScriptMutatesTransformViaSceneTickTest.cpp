#include "OloEnginePCH.h"

// =============================================================================
// LuaScriptMutatesTransformViaSceneTickTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene tick × LuaScriptComponent × LuaScriptEngine × LuaScriptGlue
//   bindings × TransformComponent mutation. The signature Lua flow:
//   a script's `OnUpdate(entityID, ts)` calls into the engine's exposed
//   API (`entity_utils.set_translation` etc.) to mutate the entity's
//   state. A regression in any of:
//     - `Scene::OnUpdateRuntime` not invoking `LuaScriptEngine::OnUpdateEntity`,
//     - the per-entity instance lookup in `EntityScriptInstances`,
//     - the `entity_utils` table not being registered,
//     - the TransformComponent binding's writer
//   silently breaks every Lua gameplay script in the project.
//
// Scenario: write a tiny Lua script to a temp file. Attach it to an
// entity. The script's `OnUpdate` sets the entity's translation.x to
// the tick count it observes. After N scene ticks, the entity's x
// should equal N.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <filesystem>
#include <fstream>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    // Write the script contents to a per-test temp file. Returning the path
    // lets the test reference it via LuaScriptComponent.ScriptFile and the
    // engine load it via sol::state::load_file.
    std::filesystem::path WriteScriptTo(const std::string& contents, const char* nameStem)
    {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        std::string fileName = std::string("olo_l12_") + nameStem + "_" + (info ? info->name() : "unknown") + ".lua";
        const auto path = std::filesystem::temp_directory_path() / fileName;
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << contents;
        }
        return path;
    }
} // namespace

class LuaScriptMutatesTransformViaSceneTickTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        EnableLua();

        // Use an explicit small UUID. Random UUIDs are typically > 2^53 and
        // Lua's number type (double) can't represent them exactly — set_translation's
        // sol2 binding refuses the conversion with "not a numeric type that fits
        // exactly an integer". Real production Lua scripts get UUIDs the same way,
        // so this is a real precision concern, but for the test we want a stable,
        // round-trippable ID so the binding-correctness signal isn't masked by it.
        m_Scripted = GetScene().CreateEntityWithUUID(UUID{ 12345 }, "Scripted");

        // Lua script that increments a local counter each OnUpdate and
        // pushes it into the entity's translation.x. Counter is on the
        // script's instance table (captured in the closure) so it persists
        // across ticks for this one entity instance.
        // NOTE: declare lifecycle callbacks with dot syntax (script.OnUpdate)
        // rather than colon syntax (script:OnUpdate). LuaScriptEngine invokes
        // `onUpdate(entityID, ts)` without passing the script table as a
        // `self` argument, so colon-form OnUpdate would receive entityID as
        // `self`, ts as `entityID`, and nil as `ts` — silently confusing.
        // Also: sol2 pushes u64 to Lua as a number; the integer subtype is
        // preserved here because the value is small (we chose UUID 12345),
        // but production scripts must `math.tointeger(entityID)` when calling
        // engine APIs that take u64 — Lua doubles can't represent UUIDs > 2^53.
        const std::string scriptSrc = R"(
local script = {}
local count = 0
function script.OnUpdate(entityID, ts)
    count = count + 1
    local id = math.tointeger(entityID) or 0
    entity_utils.set_translation(id, vec3.new(count, 0, 0))
end
return script
)";
        m_ScriptPath = WriteScriptTo(scriptSrc, "tick_counter");
        RegisterLuaScript(m_Scripted, m_ScriptPath);
    }

    void TearDown() override
    {
        FunctionalTest::TearDown();
        std::error_code ec;
        std::filesystem::remove(m_ScriptPath, ec);
    }

    Entity m_Scripted;
    std::filesystem::path m_ScriptPath;
};

TEST_F(LuaScriptMutatesTransformViaSceneTickTest, OnUpdateRunsEachTickAndWritesTranslation)
{
    // Sanity: starting translation is the default-constructed zero vector.
    const auto& startT = m_Scripted.GetComponent<TransformComponent>().Translation;
    ASSERT_NEAR(startT.x, 0.0f, 1e-4f);

    constexpr u32 kFrames = 5;
    RunFrames(kFrames);

    const auto& endT = m_Scripted.GetComponent<TransformComponent>().Translation;
    EXPECT_NEAR(endT.x, static_cast<f32>(kFrames), 1e-4f)
        << "expected translation.x = " << kFrames
        << " after " << kFrames << " ticks; got " << endT.x
        << ". If 0: Lua OnUpdate didn't fire (Scene→LuaScriptEngine wiring broken). "
        << "If anything else: entity_utils.set_translation or vec3.new binding is wrong.";
    EXPECT_NEAR(endT.y, 0.0f, 1e-4f);
    EXPECT_NEAR(endT.z, 0.0f, 1e-4f);
}
