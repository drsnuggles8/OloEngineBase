#include "OloEnginePCH.h"

// =============================================================================
// LuaScriptSetsRigidbody2DVelocityTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene tick × LuaScriptEngine::OnUpdateEntity × LuaScriptGlue's
//   `Rigidbody2DComponent.linearVelocity` property × Box2D's
//   `b2Body_SetLinearVelocity` × b2World_Step integrator.
//   The Lua property writer sets BOTH the cached `LinearVelocity` field
//   AND the live Box2D body velocity. If either side regresses (the
//   property pushes only to the component field but not Box2D, or vice
//   versa), the entity either doesn't move or moves only on the first
//   tick before Box2D overwrites it.
//
// Scenario: a kinematic body owned by a Lua script that sets
// `rb.linearVelocity = vec2.new(2, 0)` once on first OnUpdate. After
// ticking for ~0.5s with gravity disabled, the entity's transform.x
// should be near 1.0 (2 m/s × 0.5s). If x stays 0, the Lua → Box2D
// write path is broken.
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
        const std::string fileName = std::string("olo_functional_") + nameStem + "_" + (info ? info->name() : "unknown") + ".lua";
        const auto path = std::filesystem::temp_directory_path() / fileName;
        std::ofstream(path, std::ios::binary | std::ios::trunc) << contents;
        return path;
    }
} // namespace

class LuaScriptSetsRigidbody2DVelocityTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // Kinematic body so we don't need to worry about gravity or contacts
        // — pure Lua-driven motion proves the property writer fires.
        m_Body = GetScene().CreateEntityWithUUID(UUID{ 9876 }, "Mover");
        m_Body.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 0.0f };
        Rigidbody2DComponent rb;
        rb.Type = Rigidbody2DComponent::BodyType::Kinematic;
        m_Body.AddComponent<Rigidbody2DComponent>(rb);
        // BoxCollider2D so b2Body actually gets created in OnPhysics2DStart;
        // bodies without a shape are valid but worth attaching to mirror real
        // scenes (and the test catches the no-collider regression for free).
        BoxCollider2DComponent col;
        col.Size = { 0.25f, 0.25f };
        m_Body.AddComponent<BoxCollider2DComponent>(col);

        EnablePhysics2D();
        EnableLua();

        // OnUpdate sets the velocity once (when the local flag is false),
        // then no-ops. We can't easily detect first-tick from Lua otherwise.
        // entity_utils.get_component returns a raw pointer to the component
        // proxy registered in LuaScriptGlue, so the property writer fires.
        const std::string scriptSrc = R"(
local script = {}
local applied = false
function script.OnUpdate(entityID, ts)
    if applied then return end
    local id = math.tointeger(entityID) or 0
    local rb = entity_utils.get_component(id, "Rigidbody2DComponent")
    if rb ~= nil then
        rb.linearVelocity = vec2.new(2.0, 0.0)
        applied = true
    end
end
return script
)";
        m_ScriptPath = WriteScript(scriptSrc, "set_velocity");
        RegisterLuaScript(m_Body, m_ScriptPath);
    }

    void TearDown() override
    {
        FunctionalTest::TearDown();
        std::error_code ec;
        std::filesystem::remove(m_ScriptPath, ec);
    }

    Entity m_Body;
    std::filesystem::path m_ScriptPath;
};

TEST_F(LuaScriptSetsRigidbody2DVelocityTest, BodyTranslatesAfterLuaSetsLinearVelocity)
{
    const f32 startX = m_Body.GetComponent<TransformComponent>().Translation.x;
    ASSERT_NEAR(startX, 0.0f, 1e-4f);

    // 0.5s at 2 m/s → ~1.0m displacement. Generous tolerance since the
    // velocity is applied on tick 1, not tick 0 — first tick's contribution
    // is missed.
    TickFor(/*seconds=*/0.5f);

    const auto& t = m_Body.GetComponent<TransformComponent>().Translation;
    EXPECT_TRUE(std::isfinite(t.x) && std::isfinite(t.y));
    EXPECT_GT(t.x, 0.7f)
        << "body didn't translate after Lua set linearVelocity; x=" << t.x
        << ". Lua→Box2D property writer or Scene→Box2D step is broken.";
    EXPECT_LT(t.x, 1.2f)
        << "body translated farther than the velocity should allow; "
           "Lua may be setting velocity every tick (re-applying) instead "
           "of once.";
    EXPECT_NEAR(t.y, 0.0f, 1e-3f) << "kinematic body drifted on Y — gravity leak.";
}
