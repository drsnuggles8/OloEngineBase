#include "OloEnginePCH.h"

// =============================================================================
// LuaRaycastHitsPhysicsBodyTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene tick × LuaScriptEngine × LuaScriptGlue `Physics.Raycast` ×
//   ScriptEngine::GetSceneContext × Scene::GetPhysicsScene × JoltScene::CastRay
//   × Rigidbody3DComponent. The Lua raycast lambda pulls the scene out of
//   the C# ScriptEngine's scene-context (which the harness sets via
//   ScriptEngine::SetSceneContextForTesting in EnableLua) and asks Jolt
//   for the nearest body along a ray. Several seams must agree:
//     - Lua sees a Scene*: ScriptEngine context wiring is intact.
//     - Scene has a live JoltScene: Physics3D is started.
//     - CastRay walks the body table and resolves the hit's m_HitEntity
//       back to the entity's UUID.
//   A regression in any of those silently returns nil from the Lua
//   binding, and gameplay scripts that rely on raycasts (line-of-sight,
//   click-to-pick, hitscan weapons) all break.
//
// Scenario: a static box at x=5 acts as the target. A second "Shooter"
// entity owns a Lua script that on first OnUpdate casts a ray from
// origin (0, 0.5, 0) towards +x. If the ray hits, the script writes the
// hit distance into the Shooter's transform.x as the test's observation
// channel. After one tick, the Shooter's x should match the expected
// hit distance (target_center_x - target_half_extent ≈ 4.5).
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

class LuaRaycastHitsPhysicsBodyTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // Static target at x=5, half-extent 0.5 → its near face is at x≈4.5.
        // Use a small explicit UUID: random UUIDs are usually > 2^53, and
        // when Physics.Raycast packs `hit.entityID = static_cast<u64>(uuid)`
        // into the Lua result table, sol2 with SOL_ALL_SAFETIES_ON rejects
        // the push with "integer value will be misrepresented in lua",
        // erroring OnUpdate and silently turning the hit into nil. This
        // surfaces randomly depending on UUID() output — flaky from gtest's
        // point of view, but a real Lua-binding precision issue worth
        // pinning. Real production scripts hit the same wall.
        m_Target = GetScene().CreateEntityWithUUID(UUID{ 1001 }, "Target");
        m_Target.GetComponent<TransformComponent>().Translation = { 5.0f, 0.5f, 0.0f };
        Rigidbody3DComponent body;
        body.m_Type = BodyType3D::Static;
        m_Target.AddComponent<Rigidbody3DComponent>(body);
        BoxCollider3DComponent col;
        col.m_HalfExtents = { 0.5f, 0.5f, 0.5f };
        m_Target.AddComponent<BoxCollider3DComponent>(col);

        // Shooter — bears the Lua script. Small UUID so sol2's u64→Lua
        // number coercion is exact.
        m_Shooter = GetScene().CreateEntityWithUUID(UUID{ 2024 }, "Shooter");
        m_Shooter.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 0.0f };

        EnablePhysics3D();
        EnableLua();

        // Cast a ray (+x, max=10). On hit, write hit.distance into the
        // Shooter's transform.x so the test can read it after the tick.
        const std::string scriptSrc = R"(
local script = {}
local applied = false
function script.OnUpdate(entityID, ts)
    if applied then return end
    local hit = Physics.Raycast(vec3.new(0, 0.5, 0), vec3.new(1, 0, 0), 10.0)
    if hit ~= nil then
        local id = math.tointeger(entityID) or 0
        entity_utils.set_translation(id, vec3.new(hit.distance, 0, 0))
        applied = true
    end
end
return script
)";
        m_ScriptPath = WriteScript(scriptSrc, "raycast");
        RegisterLuaScript(m_Shooter, m_ScriptPath);
    }

    void TearDown() override
    {
        FunctionalTest::TearDown();
        std::error_code ec;
        std::filesystem::remove(m_ScriptPath, ec);
    }

    Entity m_Target;
    Entity m_Shooter;
    std::filesystem::path m_ScriptPath;
};

TEST_F(LuaRaycastHitsPhysicsBodyTest, ScriptCastingRayAlongXObservesNearFaceOfStaticTarget)
{
    // One tick is enough — the script casts and writes on its first OnUpdate.
    RunFrames(1);

    const auto& t = m_Shooter.GetComponent<TransformComponent>().Translation;
    EXPECT_TRUE(std::isfinite(t.x));

    // Expected distance: target center 5.0, half-extent 0.5 → near face ≈ 4.5.
    // Allow slack because Jolt's narrow-phase may report a slightly different
    // surface intersection depending on the convex tolerance.
    EXPECT_GT(t.x, 4.0f)
        << "Lua raycast didn't observe a hit (transform.x left at 0.0) — "
           "Physics.Raycast returned nil, which means either Lua's scene-context "
           "lookup failed, JoltScene::CastRay didn't traverse the body, or the "
           "ray missed because origin/direction were misinterpreted.";
    EXPECT_LT(t.x, 5.0f)
        << "Lua raycast observed a hit BEYOND the target's near face — Jolt "
           "is reporting hit at the far face (or the target's transform is "
           "being ignored at body creation).";
}
