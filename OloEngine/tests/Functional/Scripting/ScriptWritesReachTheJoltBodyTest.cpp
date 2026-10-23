#include "OloEnginePCH.h"
#include "TestTempDir.h"

// OLO_TEST_LAYER: Functional

// =============================================================================
// ScriptWritesReachTheJoltBodyTest — Functional Test (found on #1559).
//
// Seam: Lua entity_utils.set_translation / Rigidbody3DComponent.type ×
// JoltScene::ApplyAuthoredBodyState × the physics fence.
//
// A script (or the inspector, or an MCP write) that moved a 3D body's
// transform, or changed its Rigidbody3DComponent::m_Type, during Play changed
// only the component: the fence wrote the body's own pose straight back over the
// move, and the type never reached Jolt. The body now follows what was written
// before each step; a body created Static cannot move and the change is refused
// with the field put back.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Physics3D/JoltBody.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <filesystem>
#include <fstream>

using namespace OloEngine;
using namespace OloEngine::Functional;

class ScriptWritesReachTheJoltBodyTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Ball = GetScene().CreateEntityWithUUID(UUID{ 4201 }, "Ball");
        m_Ball.GetComponent<TransformComponent>().Translation = { 0.0f, 1.0f, 0.0f };
        SphereCollider3DComponent shape;
        shape.m_Radius = 0.1f;
        m_Ball.AddComponent<SphereCollider3DComponent>(shape);
        Rigidbody3DComponent ballBody;
        ballBody.m_Type = BodyType3D::Dynamic;
        m_Ball.AddComponent<Rigidbody3DComponent>(ballBody);

        m_Post = GetScene().CreateEntityWithUUID(UUID{ 4202 }, "Post");
        m_Post.GetComponent<TransformComponent>().Translation = { -4.0f, 0.5f, 0.0f };
        BoxCollider3DComponent box;
        box.m_HalfExtents = { 0.1f, 0.5f, 0.1f };
        m_Post.AddComponent<BoxCollider3DComponent>(box);
        m_Post.AddComponent<Rigidbody3DComponent>();

        m_Driver = GetScene().CreateEntity("Driver");
        EnablePhysics3D();
        EnableLua();
    }

    void TearDown() override
    {
        FunctionalTest::TearDown();
        std::error_code ec;
        std::filesystem::remove(m_Script, ec);
    }

    void Run(const std::string& source)
    {
        m_Script = OloEngine::Tests::TempFile("olo_functional_script_writes.lua");
        std::ofstream(m_Script, std::ios::binary | std::ios::trunc) << source;
        RegisterLuaScript(m_Driver, m_Script);
    }

    [[nodiscard]] Ref<JoltBody> BodyOf(Entity entity)
    {
        return GetScene().GetPhysicsScene()->GetBodyByEntityID(entity.GetUUID());
    }

    Entity m_Ball;
    Entity m_Post;
    Entity m_Driver;
    std::filesystem::path m_Script;
};

TEST_F(ScriptWritesReachTheJoltBodyTest, ATranslationWrittenFromLuaTeleportsTheBodyInsteadOfSnappingBack)
{
    Run(R"(
local s = {}
local frame = 0
function s.OnUpdate(id, ts)
    frame = frame + 1
    if frame == 2 then entity_utils.set_translation(4201, vec3.new(5, 1, 0)) end
end
return s
)");
    RunFrames(5);
    EXPECT_NEAR(m_Ball.GetComponent<TransformComponent>().Translation.x, 5.0f, 1e-3f)
        << "the physics fence wrote the old body pose back over the script's move";
    EXPECT_NEAR(BodyOf(m_Ball)->GetPosition().x, 5.0f, 1e-3f) << "the body never heard of the move";
}

TEST_F(ScriptWritesReachTheJoltBodyTest, ABodyTypeWrittenFromLuaReachesTheBodyAndAStaticBodyRefusesIt)
{
    Run(R"(
local s = {}
local frame = 0
function s.OnUpdate(id, ts)
    frame = frame + 1
    if frame == 1 then
        entity_utils.get_component(4201, "Rigidbody3DComponent").type = 2 -- Kinematic
        entity_utils.get_component(4202, "Rigidbody3DComponent").type = 1 -- Dynamic: created Static, so refused
    end
end
return s
)");
    RunFrames(30);
    EXPECT_TRUE(BodyOf(m_Ball)->IsKinematic()) << "the type change never reached Jolt";
    EXPECT_GT(m_Ball.GetComponent<TransformComponent>().Translation.y, 0.99f) << "a kinematic ball kept falling";
    EXPECT_TRUE(BodyOf(m_Post)->IsStatic()) << "a body created Static moved";
    EXPECT_EQ(m_Post.GetComponent<Rigidbody3DComponent>().m_Type, BodyType3D::Static)
        << "the refused change left the component claiming a type the body does not have";
}
