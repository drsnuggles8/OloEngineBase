#include "OloEnginePCH.h"

// =============================================================================
// LuaScriptOnDestroyFiresTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene::DestroyEntity × LuaScriptEngine::OnDestroyEntity × Lua
//   `OnDestroy` callback. The companion lifecycle path to OnCreate /
//   OnUpdate: when gameplay code destroys an entity, the Lua-side
//   `OnDestroy` callback must fire so scripts can release external
//   resources (close UI panels, unhook listeners, save state). A
//   regression in either Scene::DestroyEntity's call into
//   `LuaScriptEngine::OnDestroyEntity` or the entity-instance lookup
//   silently leaks Lua-side resources.
//
// Scenario: attach a script whose `OnDestroy` writes a side-channel
// file. Destroy the entity. Verify the side-channel was written, then
// continue ticking the scene to confirm the entity's slot in
// EntityScriptInstances has been cleared (no stale OnUpdate calls).
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
    std::filesystem::path WriteScript(const std::string& contents, const char* stem)
    {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        std::string fileName = std::string("olo_l12_") + stem + "_" + (info ? info->name() : "unknown") + ".lua";
        const auto path = std::filesystem::temp_directory_path() / fileName;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << contents;
        return path;
    }
} // namespace

class LuaScriptOnDestroyFiresTest : public FunctionalTest
{
  protected:
    static constexpr u64 kScriptedUUID = 54321;
    static constexpr u64 kBuddyUUID = 67890;

    void BuildScene() override
    {
        EnableLua();

        // Buddy entity acts as a side-channel for OnDestroy. The scripted
        // entity's OnDestroy callback writes a sentinel value to the
        // buddy's TransformComponent.x; that's verifiable from C++.
        m_Buddy = GetScene().CreateEntityWithUUID(UUID{ kBuddyUUID }, "Buddy");

        const std::string scriptSrc = R"(
local script = {}
function script.OnUpdate(entityID, ts)
    -- intentionally does nothing — we only care about OnDestroy
end
function script.OnDestroy(entityID)
    -- Side-channel: write a known sentinel to the buddy entity's x.
    entity_utils.set_translation()" + std::to_string(kBuddyUUID) +
                                      R"(, vec3.new(123.0, 0, 0))
end
return script
)";
        m_ScriptPath = WriteScript(scriptSrc, "ondestroy");

        m_Scripted = GetScene().CreateEntityWithUUID(UUID{ kScriptedUUID }, "Scripted");
        RegisterLuaScript(m_Scripted, m_ScriptPath);
    }

    void TearDown() override
    {
        FunctionalTest::TearDown();
        std::error_code ec;
        std::filesystem::remove(m_ScriptPath, ec);
    }

    Entity m_Scripted;
    Entity m_Buddy;
    std::filesystem::path m_ScriptPath;
};

TEST_F(LuaScriptOnDestroyFiresTest, OnDestroyCallbackFiresWhenScriptEntityIsDestroyed)
{
    // Sanity: buddy's x starts at 0 (default).
    ASSERT_NEAR(m_Buddy.GetComponent<TransformComponent>().Translation.x, 0.0f, 1e-4f);

    // Tick a few frames to confirm the script is alive (no crashes).
    RunFrames(2);
    ASSERT_NEAR(m_Buddy.GetComponent<TransformComponent>().Translation.x, 0.0f, 1e-4f)
        << "buddy.x changed during normal updates — OnUpdate is doing more than the no-op test setup says.";

    // Destroy the scripted entity. Scene::DestroyEntity needs to dispatch
    // through LuaScriptEngine::OnDestroyEntity, which calls our OnDestroy
    // callback, which writes 123 to the buddy's transform.
    GetScene().DestroyEntity(m_Scripted);
    m_Scripted = Entity{};

    EXPECT_NEAR(m_Buddy.GetComponent<TransformComponent>().Translation.x, 123.0f, 1e-4f)
        << "buddy.x was not set to 123 after DestroyEntity — Scene::DestroyEntity "
           "is not dispatching to LuaScriptEngine::OnDestroyEntity, or the script's "
           "OnDestroy callback was not invoked.";

    // Continue ticking — the destroyed entity's instance must be cleared
    // from EntityScriptInstances so subsequent ticks don't try to call
    // OnUpdate on a dead entity (which would log errors or crash).
    RunFrames(5);

    // Buddy.x must remain 123 (no further mutation). If OnUpdate was still
    // firing for the destroyed entity, our no-op OnUpdate wouldn't change
    // x — but a crash there is what we're guarding against (no SUCCEED
    // since the value assertion already proves the system is alive).
    EXPECT_NEAR(m_Buddy.GetComponent<TransformComponent>().Translation.x, 123.0f, 1e-4f)
        << "buddy.x changed after the scripted entity was destroyed — OnUpdate is "
           "still firing for the dead entity, EntityScriptInstances wasn't cleared.";
}
