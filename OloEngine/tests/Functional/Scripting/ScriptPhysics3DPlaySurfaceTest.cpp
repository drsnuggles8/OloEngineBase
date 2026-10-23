#include "OloEnginePCH.h"
#include "TestTempDir.h"

// OLO_TEST_LAYER: Functional

// =============================================================================
// ScriptPhysics3DPlaySurfaceTest — Functional Test (issue #1559).
//
// Cross-subsystem seam under test:
//   Scene tick × LuaScriptGlue × Physics3DScriptApi × JoltScene (bodies,
//   contact drain, carried bodies) × Scene::AttachEntity / DetachEntity ×
//   PropagateWorldTransforms × Input.
//
// The play scenes throw a ball for a dog, hear it land, carry it in the dog's
// mouth and let it go. Each of those is a script call that has to reach the
// LIVE Jolt body, and before #1559 none did: scripts had no 3D impulse or
// velocity, no contacts, no reparenting, and the body never heard of a script's
// type change or teleport (the fence wrote the body pose back over it).
//
// Every call here is made from Lua through Scene::OnUpdateRuntime. The C# glue
// is a pass-through to the same Physics3DScriptApi function (Mono is not
// initialised in this binary), so the API cases below are its contract too.
// Each behaviour has a negative control: a static body refuses an impulse and a
// type change, a far pair never shows up as a contact, a static body cannot be
// carried.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Core/Input.h"
#include "OloEngine/Physics3D/JoltBody.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/Physics3DScriptApi.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    constexpr u64 kGroundId = 3001;
    constexpr u64 kBallId = 3002;
    constexpr u64 kPostId = 3003;
    constexpr u64 kFarBoxId = 3004;
    constexpr u64 kCarrierId = 3005;
    constexpr u64 kObserverId = 3006;

    std::string Substitute(std::string text)
    {
        const std::pair<const char*, u64> ids[] = {
            { "$GROUND", kGroundId }, { "$BALL", kBallId }, { "$POST", kPostId }, { "$FAR", kFarBoxId },
            { "$CARRIER", kCarrierId }, { "$OBSERVER", kObserverId },
        };
        for (const auto& [token, id] : ids)
        {
            for (sizet at = text.find(token); at != std::string::npos; at = text.find(token))
            {
                text.replace(at, std::strlen(token), std::to_string(id));
            }
        }
        return text;
    }
} // namespace

class ScriptPhysics3DPlaySurfaceTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        Scene& scene = GetScene();

        m_Ground = scene.CreateEntityWithUUID(UUID{ kGroundId }, "Ground");
        m_Ground.GetComponent<TransformComponent>().Translation = { 0.0f, -0.1f, 0.0f };
        BoxCollider3DComponent groundBox;
        groundBox.m_HalfExtents = { 10.0f, 0.1f, 10.0f };
        m_Ground.AddComponent<BoxCollider3DComponent>(groundBox);
        m_Ground.AddComponent<Rigidbody3DComponent>(MakeBody(BodyType3D::Static, false));

        m_Ball = scene.CreateEntityWithUUID(UUID{ kBallId }, "Ball");
        m_Ball.GetComponent<TransformComponent>().Translation = { 0.0f, 1.0f, 0.0f };
        SphereCollider3DComponent ballShape;
        ballShape.m_Radius = 0.1f;
        m_Ball.AddComponent<SphereCollider3DComponent>(ballShape);
        m_Ball.AddComponent<Rigidbody3DComponent>(MakeBody(BodyType3D::Dynamic, true));

        m_Post = scene.CreateEntityWithUUID(UUID{ kPostId }, "Post");
        m_Post.GetComponent<TransformComponent>().Translation = { -4.0f, 0.5f, 0.0f };
        BoxCollider3DComponent postBox;
        postBox.m_HalfExtents = { 0.1f, 0.5f, 0.1f };
        m_Post.AddComponent<BoxCollider3DComponent>(postBox);
        m_Post.AddComponent<Rigidbody3DComponent>(MakeBody(BodyType3D::Static, false));

        m_FarBox = scene.CreateEntityWithUUID(UUID{ kFarBoxId }, "FarBox");
        m_FarBox.GetComponent<TransformComponent>().Translation = { 8.0f, 0.5f, 8.0f };
        BoxCollider3DComponent farBox;
        farBox.m_HalfExtents = { 0.5f, 0.5f, 0.5f };
        m_FarBox.AddComponent<BoxCollider3DComponent>(farBox);
        m_FarBox.AddComponent<Rigidbody3DComponent>(MakeBody(BodyType3D::Static, false));

        m_Carrier = scene.CreateEntityWithUUID(UUID{ kCarrierId }, "Carrier");
        m_Carrier.GetComponent<TransformComponent>().Translation = { 2.0f, 1.0f, 0.0f };

        m_Observer = scene.CreateEntityWithUUID(UUID{ kObserverId }, "Observer");

        EnablePhysics3D();
        EnableLua();
    }

    void TearDown() override
    {
        FunctionalTest::TearDown();
        for (const auto& path : m_Scripts)
        {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }

    static Rigidbody3DComponent MakeBody(BodyType3D type, bool gravity)
    {
        Rigidbody3DComponent body;
        body.m_Type = type;
        body.m_Mass = 1.0f;
        body.m_LinearDrag = 0.0f;
        body.m_AngularDrag = 0.0f;
        body.m_DisableGravity = !gravity;
        return body;
    }

    // Install `source` (with $NAME entity tokens substituted) on the observer.
    void RunScript(const std::string& source, const char* stem)
    {
        const auto path = OloEngine::Tests::TempFile(std::string("olo_play_surface_") + stem + ".lua");
        std::ofstream(path, std::ios::binary | std::ios::trunc) << Substitute(source);
        m_Scripts.push_back(path);
        RegisterLuaScript(m_Observer, path);
    }

    [[nodiscard]] glm::vec3 Observed() const
    {
        return m_Observer.GetComponent<TransformComponent>().Translation;
    }

    [[nodiscard]] Ref<JoltBody> BodyOf(Entity entity)
    {
        return GetScene().GetPhysicsScene()->GetBodyByEntityID(entity.GetUUID());
    }

    Entity m_Ground;
    Entity m_Ball;
    Entity m_Post;
    Entity m_FarBox;
    Entity m_Carrier;
    Entity m_Observer;
    std::vector<std::filesystem::path> m_Scripts;
};

TEST_F(ScriptPhysics3DPlaySurfaceTest, AnImpulseFromLuaLaunchesTheLiveBodyAndAStaticBodyRefusesIt)
{
    // The impulse is along +z; gravity acts on y only, and the ball is still
    // in the air when its velocity is read (a 2 cm fall in four frames).
    RunScript(R"(
local s = {}
local frame = 0
function s.OnUpdate(id, ts)
    frame = frame + 1
    local ball = entity_utils.get_component($BALL, "Rigidbody3DComponent")
    local post = entity_utils.get_component($POST, "Rigidbody3DComponent")
    if frame == 1 then
        local okBall = ball:applyLinearImpulse(vec3.new(0, 0, 2))
        local okPost = post:applyLinearImpulse(vec3.new(0, 0, 2))
        entity_utils.set_translation(id, vec3.new(okBall and 1 or 0, okPost and 1 or 0, 0))
    elseif frame == 4 then
        local t = entity_utils.get_translation(id)
        entity_utils.set_translation(id, vec3.new(t.x, t.y, ball.linearVelocity.z))
    end
end
return s
)",
              "impulse");

    RunFrames(6);

    const glm::vec3 observed = Observed();
    EXPECT_FLOAT_EQ(observed.x, 1.0f) << "applyLinearImpulse on a dynamic body reported a refusal";
    EXPECT_FLOAT_EQ(observed.y, 0.0f) << "applyLinearImpulse on a STATIC body reported success";
    // 2 N*s on 1 kg, no drag, no gravity: 2 m/s along +z.
    EXPECT_NEAR(observed.z, 2.0f, 0.02f) << "linearVelocity did not read the live body back";
    EXPECT_GT(m_Ball.GetComponent<TransformComponent>().Translation.z, 0.1f) << "the impulse did not move the ball";
    EXPECT_NEAR(m_Post.GetComponent<TransformComponent>().Translation.z, 0.0f, 1e-5f) << "the static post moved";
}

TEST_F(ScriptPhysics3DPlaySurfaceTest, VelocityAndBodyTypeWrittenFromLuaReachTheLiveBody)
{
    RunScript(R"(
local s = {}
local frame = 0
function s.OnUpdate(id, ts)
    frame = frame + 1
    local ball = entity_utils.get_component($BALL, "Rigidbody3DComponent")
    if frame == 1 then
        ball.type = 2 -- Kinematic: gravity no longer moves it
        local post = entity_utils.get_component($POST, "Rigidbody3DComponent")
        post.type = 1 -- Dynamic: a body created Static cannot become one
    elseif frame == 3 then
        ball.linearVelocity = vec3.new(1, 0, 0) -- a kinematic body moves at its velocity
    end
end
return s
)",
              "velocity_type");

    RunFrames(31);

    EXPECT_TRUE(BodyOf(m_Ball)->IsKinematic()) << "the authored type change never reached Jolt";
    const glm::vec3 ball = m_Ball.GetComponent<TransformComponent>().Translation;
    EXPECT_GT(ball.y, 0.99f) << "a kinematic ball kept falling";
    // 28 frames at 1 m/s.
    EXPECT_GT(ball.x, 0.3f) << "the velocity written from Lua never moved the body";

    EXPECT_TRUE(BodyOf(m_Post)->IsStatic()) << "a body created Static moved";
    EXPECT_EQ(m_Post.GetComponent<Rigidbody3DComponent>().m_Type, BodyType3D::Static)
        << "the refused type change left the component claiming a type the body does not have";
}

TEST_F(ScriptPhysics3DPlaySurfaceTest, ContactsHearTheBallLandAndAFarPairStaysSilent)
{
    m_Ball.GetComponent<TransformComponent>().Translation = { 0.0f, 0.4f, 0.0f };
    BodyOf(m_Ball)->SetTransform({ 0.0f, 0.4f, 0.0f }, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));

    // x: the frame the ground was first heard, y: frames the far box was heard
    // touching the ball or the ground, z: frames the post was heard at all.
    RunScript(R"(
local s = {}
local frame = 0
local heard = 0
local wrong = 0
local post = 0
local function has(list, id)
    for _, v in ipairs(list) do if v == id then return true end end
    return false
end
function s.OnUpdate(id, ts)
    frame = frame + 1
    local ballContacts = Physics.GetContacts($BALL)
    if heard == 0 and has(ballContacts, $GROUND) then heard = frame end
    if has(ballContacts, $FAR) or has(Physics.GetContacts($FAR), $GROUND) then wrong = wrong + 1 end
    if #Physics.GetContacts($POST) > 0 then post = post + 1 end
    entity_utils.set_translation(id, vec3.new(heard, wrong, post))
end
return s
)",
              "contacts");

    // The same query the C# Physics.GetContacts makes, read the tick the script
    // first heard the landing. (Once the ball has rested long enough to sleep,
    // Jolt drops its contacts, so "touching now" is a moving-body answer.)
    const bool heard = TickUntil([this] { return Observed().x > 0.0f; }, 1.0f);
    ASSERT_TRUE(heard) << "the ball never reported touching the ground within a second of a 0.3 m drop";
    TArray<UUID> contacts;
    EXPECT_EQ(Physics3DScriptApi::GetContacts(GetScene(), UUID{ kBallId }, contacts), Physics3DScriptApi::Result::Ok);
    bool groundListed = false;
    for (const UUID other : contacts)
    {
        groundListed = groundListed || other == UUID{ kGroundId };
    }
    EXPECT_TRUE(groundListed) << "the API does not list the ground the script just heard";

    RunFrames(90);
    const glm::vec3 observed = Observed();
    EXPECT_FLOAT_EQ(observed.y, 0.0f) << "a pair that never touches was reported as a contact";
    EXPECT_FLOAT_EQ(observed.z, 0.0f) << "the post touches nothing that moves, yet reported contacts";
}

TEST_F(ScriptPhysics3DPlaySurfaceTest, ACarriedBodyFollowsItsCarrierAndLeavesAtTheSpeedItWasCarried)
{
    // The carrier walks +x at 2 m/s. The ball rides 0.5 m above it from frame 2,
    // and is let go on frame 40.
    RunScript(R"(
local s = {}
local frame = 0
function s.OnUpdate(id, ts)
    frame = frame + 1
    local c = entity_utils.get_translation($CARRIER)
    entity_utils.set_translation($CARRIER, vec3.new(c.x + 2.0 * ts, c.y, c.z))
    if frame == 2 then
        local okBall = entity_utils.attach_to($BALL, $CARRIER, "", vec3.new(0, 0.5, 0))
        local okPost = entity_utils.attach_to($POST, $CARRIER, "", vec3.new(0, 0, 0))
        entity_utils.set_translation(id, vec3.new(okBall and 1 or 0, okPost and 1 or 0, 0))
    elseif frame == 40 then
        local t = entity_utils.get_translation(id)
        entity_utils.set_translation(id, vec3.new(t.x, t.y, entity_utils.detach($BALL) and 1 or 0))
    end
end
return s
)",
              "carry");

    RunFrames(39);

    glm::vec3 observed = Observed();
    ASSERT_FLOAT_EQ(observed.x, 1.0f) << "attach_to refused a dynamic ball";
    EXPECT_FLOAT_EQ(observed.y, 0.0f) << "attach_to accepted a STATIC body, which Jolt cannot move";
    EXPECT_FALSE(m_Post.GetParent()) << "the refused attach still reparented the post";

    // Carried: kinematic, the transform is the authored LOCAL offset (the
    // fence did not write a world pose into it), and the world pose rides the
    // carrier.
    const Ref<JoltBody> ball = BodyOf(m_Ball);
    EXPECT_TRUE(ball->IsKinematic());
    EXPECT_NEAR(m_Ball.GetComponent<TransformComponent>().Translation.y, 0.5f, 1e-5f)
        << "the physics fence wrote a world pose into a parent-relative transform";
    const glm::vec3 carrier = m_Carrier.GetComponent<TransformComponent>().Translation;
    const glm::vec3 world = glm::vec3(GetScene().GetWorldTransform(static_cast<entt::entity>(m_Ball))[3]);
    EXPECT_NEAR(world.x, carrier.x, 1e-3f);
    EXPECT_NEAR(world.y, carrier.y + 0.5f, 1e-3f);
    // The body follows the drawn pose, at most one tick behind (2 m/s / 60 Hz).
    EXPECT_NEAR(ball->GetPosition().x, world.x, 2.0f / 60.0f + 1e-3f);

    RunFrames(1); // frame 40: let go
    observed = Observed();
    ASSERT_FLOAT_EQ(observed.z, 1.0f) << "detach refused";
    EXPECT_FALSE(m_Ball.GetParent()) << "a released ball is still parented";
    EXPECT_TRUE(ball->IsDynamic()) << "a released ball did not get its motion type back";
    EXPECT_NEAR(ball->GetLinearVelocity().x, 2.0f, 0.1f) << "the ball did not leave at the speed it was carried at";

    const glm::vec3 released = m_Ball.GetComponent<TransformComponent>().Translation;
    RunFrames(20);
    const glm::vec3 later = m_Ball.GetComponent<TransformComponent>().Translation;
    EXPECT_GT(later.x - released.x, 0.5f) << "the released ball stopped instead of flying on";
    EXPECT_LT(later.y, released.y - 0.1f) << "the released ball does not fall";
}

TEST_F(ScriptPhysics3DPlaySurfaceTest, AnAttachedEntityReadsItsWorldPositionFromLua)
{
    // get_translation is parent-relative: a ball carried 0.5 m above the
    // carrier reads (0, 0.5, 0). get_world_translation reads where it is.
    RunScript(R"(
local s = {}
local frame = 0
function s.OnUpdate(id, ts)
    frame = frame + 1
    if frame == 1 then
        entity_utils.attach_to($BALL, $CARRIER, "", vec3.new(0, 0.5, 0))
    elseif frame == 3 then
        entity_utils.set_translation(id, entity_utils.get_world_translation($BALL))
        entity_utils.set_scale(id, entity_utils.get_translation($BALL))
    end
end
return s
)",
              "world_translation");

    RunFrames(4);

    ASSERT_EQ(m_Ball.GetParent(), m_Carrier) << "attach_to refused the ball";
    const glm::vec3 carrier = m_Carrier.GetComponent<TransformComponent>().Translation;
    const glm::vec3 world = Observed();
    EXPECT_NEAR(world.x, carrier.x, 1e-4f) << "get_world_translation did not compose the parent";
    EXPECT_NEAR(world.y, carrier.y + 0.5f, 1e-4f) << "get_world_translation did not compose the parent";
    EXPECT_NEAR(world.z, carrier.z, 1e-4f);
    // The negative control: the local read is the offset, not the world position.
    EXPECT_NEAR(m_Observer.GetComponent<TransformComponent>().Scale.x, 0.0f, 1e-4f);
    EXPECT_NEAR(m_Observer.GetComponent<TransformComponent>().Scale.y, 0.5f, 1e-4f);
}

TEST_F(ScriptPhysics3DPlaySurfaceTest, AnEntityAttachedInPlaceStaysPutThenRidesItsCarrier)
{
    // On frame 2 the ball is moved and attached where it now is (before the
    // scene has composed the move); the post is asked to ride a bone the
    // carrier does not have. From frame 3 the carrier walks +x at 2 m/s.
    RunScript(R"(
local s = {}
local frame = 0
function s.OnUpdate(id, ts)
    frame = frame + 1
    if frame >= 3 then
        local c = entity_utils.get_translation($CARRIER)
        entity_utils.set_translation($CARRIER, vec3.new(c.x + 2.0 * ts, c.y, c.z))
    end
    if frame == 2 then
        entity_utils.set_translation($BALL, vec3.new(0.5, 2.0, -0.5))
        local okBall = entity_utils.attach_in_place($BALL, $CARRIER, "")
        local okPost = entity_utils.attach_in_place($POST, $CARRIER, "no_such_bone")
        entity_utils.set_translation(id, vec3.new(okBall and 1 or 0, okPost and 1 or 0, 0))
    end
end
return s
)",
              "attach_in_place");

    const glm::vec3 before(0.5f, 2.0f, -0.5f);
    RunFrames(2); // frame 2: moved, and attached in place
    ASSERT_FLOAT_EQ(Observed().x, 1.0f) << "attach_in_place refused the ball";
    EXPECT_FLOAT_EQ(Observed().y, 0.0f) << "attach_in_place accepted a bone the carrier does not have";
    EXPECT_FALSE(m_Post.GetParent()) << "the refused attach still reparented the post";
    ASSERT_EQ(m_Ball.GetParent(), m_Carrier);
    const glm::vec3 carrierAt = m_Carrier.GetComponent<TransformComponent>().Translation;
    const glm::vec3 attached = glm::vec3(GetScene().GetWorldTransform(static_cast<entt::entity>(m_Ball))[3]);
    // In place: where the script put it, not where it was last composed.
    EXPECT_NEAR(glm::length(attached - before), 0.0f, 1e-4f) << "attaching moved the ball";

    RunFrames(30);
    const glm::vec3 carrier = m_Carrier.GetComponent<TransformComponent>().Translation;
    const glm::vec3 world = glm::vec3(GetScene().GetWorldTransform(static_cast<entt::entity>(m_Ball))[3]);
    EXPECT_NEAR(world.x - attached.x, carrier.x - carrierAt.x, 1e-3f) << "the ball did not ride its carrier";
    EXPECT_NEAR(world.y, attached.y, 1e-3f) << "a carried ball kept falling";
}

TEST_F(ScriptPhysics3DPlaySurfaceTest, TheMouseWheelReachesLuaForTheFrameItTurnedIn)
{
    RunScript(R"(
local s = {}
function s.OnUpdate(id, ts)
    local x, y = Input.GetMouseScrollDelta()
    entity_utils.set_translation(id, vec3.new(x, y, 0))
end
return s
)",
              "scroll");

    Input::OnMouseScrolled(0.0f, 1.0f);
    Input::OnMouseScrolled(0.0f, 2.0f);
    Input::Update();
    RunFrames(1);
    EXPECT_FLOAT_EQ(Observed().y, 3.0f) << "the wheel's notches in one frame were not summed";

    Input::Update();
    RunFrames(1);
    EXPECT_FLOAT_EQ(Observed().y, 0.0f) << "a wheel turn was reported again on the next frame";
}

TEST_F(ScriptPhysics3DPlaySurfaceTest, TheSharedApiSaysWhyACallDidNothing)
{
    using Physics3DScriptApi::Result;
    Scene& scene = GetScene();
    glm::vec3 velocity;
    EXPECT_EQ(Physics3DScriptApi::GetLinearVelocity(scene, UUID{ 999999 }, velocity), Result::NoEntity);
    EXPECT_EQ(Physics3DScriptApi::GetLinearVelocity(scene, UUID{ kCarrierId }, velocity), Result::NoBody);
    EXPECT_EQ(Physics3DScriptApi::ApplyLinearImpulse(scene, UUID{ kPostId }, { 0.0f, 1.0f, 0.0f }), Result::NotDynamic);
    EXPECT_EQ(Physics3DScriptApi::ApplyLinearImpulse(scene, UUID{ kBallId }, { std::nanf(""), 0.0f, 0.0f }), Result::NotFinite);
    EXPECT_EQ(Physics3DScriptApi::SetBodyType(scene, UUID{ kPostId }, EBodyType::Dynamic), Result::Unsupported);
    EXPECT_EQ(Physics3DScriptApi::SetBodyType(scene, UUID{ kBallId }, EBodyType::Kinematic), Result::Ok);
    EXPECT_TRUE(BodyOf(m_Ball)->IsKinematic());
    EXPECT_EQ(Physics3DScriptApi::Detach(scene, UUID{ kBallId }), Result::InvalidArgument) << "detached an entity nothing carries";
}
