// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "MCP/McpPhysics2D.h"
#include "MCP/McpToolsCommon.h"

using namespace OloEngine;
namespace P2 = OloEngine::MCP::Physics2D;

namespace
{
    class PhysicsHost final : public Automation::IAutomationHost
    {
      public:
        MCP::EditorMcpContext Editor;
        const MCP::EditorMcpContext& Context() const override
        {
            return Editor;
        }
        bool IsCurrentCallCancelled() const override
        {
            return false;
        }
        bool PublishArtifact(Automation::AutomationArtifact) override
        {
            return false;
        }

      protected:
        MCP::Json MarshalReadOnMainThread(const std::function<MCP::Json()>& job, std::chrono::milliseconds) override
        {
            return job();
        }
        void EmitProgressUpdate(f64, f64, const std::string&) const override {}
    };
} // namespace

TEST(McpPhysics2D, RealCommandsValidateInputsAndJoltDoesNotSilentlyHide2D)
{
    Automation::AutomationRegistry registry;
    MCP::RegisterPhysicsTools(registry);
    PhysicsHost host;
    const auto noScene = registry.Invoke(host, "olo_physics2d_list_bodies", MCP::Json::object());
    ASSERT_TRUE(noScene.Ran());
    EXPECT_TRUE(noScene.Result.IsError);
    auto scene = Ref<Scene>::Create();
    host.Editor.GetActiveScene = [&]
    { return scene; };
    scene->CreateEntity("2D").AddComponent<Rigidbody2DComponent>();
    auto result = registry.Invoke(host, "olo_physics_list_colliders", MCP::Json::object());
    ASSERT_TRUE(result.Ran());
    EXPECT_FALSE(result.Result.IsError);
    EXPECT_EQ(result.Result.StructuredContent["total"], 0);
    EXPECT_TRUE(result.Result.StructuredContent["hasAuthored2D"].get<bool>());
    EXPECT_TRUE(result.Result.StructuredContent.contains("scopeNote"));
    result = registry.Invoke(host, "olo_physics2d_list_bodies", { { "page", std::numeric_limits<u64>::max() } });
    EXPECT_EQ(result.Outcome, Automation::AutomationInvocation::Status::InvalidArguments);
    result = registry.Invoke(host, "olo_physics2d_list_bodies", { { "pageSize", 0 } });
    EXPECT_EQ(result.Outcome, Automation::AutomationInvocation::Status::InvalidArguments);
    result = registry.Invoke(host, "olo_physics2d_raycast", { { "origin", { 0, 0 } }, { "translation", { 0, 0 } } });
    ASSERT_TRUE(result.Ran());
    EXPECT_TRUE(result.Result.IsError);
    result = registry.Invoke(host, "olo_physics2d_raycast", { { "origin", { 0, "bad" } }, { "translation", { 1, 0 } } });
    EXPECT_EQ(result.Outcome, Automation::AutomationInvocation::Status::InvalidArguments);
    result = registry.Invoke(host, "olo_physics2d_raycast", { { "origin", { 0, 0 } }, { "translation", { 1e30, 0 } } });
    ASSERT_TRUE(result.Ran());
    EXPECT_TRUE(result.Result.IsError);
}

TEST(McpPhysics2D, AuthoredMixedAndEmptyScenesHaveExplicitScope)
{
    auto scene = Ref<Scene>::Create();
    auto entity = scene->CreateEntity("Box");
    entity.AddComponent<Rigidbody2DComponent>().Type = Rigidbody2DComponent::BodyType::Dynamic;
    entity.AddComponent<BoxCollider2DComponent>().Friction = 0.23f;
    entity.AddComponent<Rigidbody3DComponent>();
    auto orphan = scene->CreateEntity("Collider without body");
    orphan.AddComponent<CircleCollider2DComponent>();
    const auto bodies = P2::List(*scene, 0, 50, false);
    EXPECT_EQ(bodies["total"], 1);
    EXPECT_FALSE(bodies["physicsRunning"].get<bool>());
    EXPECT_EQ(bodies["bodies"][0]["bodyType"], "Dynamic");
    EXPECT_TRUE(bodies["bodies"][0]["live"].is_null());
    EXPECT_EQ(P2::List(*scene, 0, 50, true)["total"], 2);
    EXPECT_TRUE(P2::Raycast(*scene, { 0, 2 }, { 0, -4 }).contains("error"));
    OloEngine::MCP::Json j;
    P2::DescribeJoltScope(j, *scene);
    EXPECT_EQ(j["dimension"], 3);
    EXPECT_TRUE(j.contains("scopeNote"));
    auto empty = Ref<Scene>::Create();
    EXPECT_EQ(P2::List(*empty, 0, 50, false)["total"], 0);
    EXPECT_FALSE(P2::HasAuthored(*empty));
    empty->CreateEntity().AddComponent<Rigidbody3DComponent>();
    EXPECT_FALSE(P2::HasAuthored(*empty));
    EXPECT_EQ(P2::List(*scene, std::numeric_limits<long long>::max(), 50, false)["returned"], 0);
}

TEST(McpPhysics2D, LiveReadUsesBox2DStateAndRayHitsTheEntity)
{
    auto scene = Ref<Scene>::Create();
    auto e = scene->CreateEntity("Live box");
    auto& rb = e.AddComponent<Rigidbody2DComponent>();
    rb.Type = Rigidbody2DComponent::BodyType::Dynamic;
    e.AddComponent<BoxCollider2DComponent>().Friction = 0.23f;
    scene->OnPhysics2DStart();
    b2Body_SetTransform(rb.RuntimeBody, { 3, 4 }, b2MakeRot(0.3f));
    b2Body_SetLinearVelocity(rb.RuntimeBody, { 7, 8 });
    const auto result = P2::List(*scene, 0, 50, true);
    ASSERT_EQ(result["returned"], 1);
    const auto& live = result["colliders"][0]["live"];
    EXPECT_EQ(live["bodyType"], "Dynamic");
    EXPECT_FLOAT_EQ(live["position"][0].get<float>(), 3.0f);
    EXPECT_FLOAT_EQ(live["linearVelocity"][1].get<float>(), 8.0f);
    EXPECT_FLOAT_EQ(live["colliders"][0]["friction"].get<float>(), 0.23f);
    const auto hit = P2::Raycast(*scene, { 3, 7 }, { 0, -6 });
    EXPECT_TRUE(hit["hit"].get<bool>());
    EXPECT_EQ(hit["entity"], std::to_string(static_cast<u64>(e.GetUUID())));
    EXPECT_FALSE(P2::Raycast(*scene, { 30, 7 }, { 0, -6 })["hit"].get<bool>());
    scene->OnPhysics2DStop();
    EXPECT_FALSE(P2::List(*scene, 0, 50, true)["physicsRunning"].get<bool>());
}

TEST(McpPhysics2D, RealCommandsDistinguishBothWorldsInEditAndLiveMixedOrEmptyScenes)
{
    Automation::AutomationRegistry registry;
    MCP::RegisterPhysicsTools(registry);
    for (int mask = 0; mask < 4; ++mask)
    {
        auto scene = Ref<Scene>::Create();
        if (mask & 1)
        {
            auto e = scene->CreateEntity("Box2D");
            e.AddComponent<Rigidbody2DComponent>();
            e.AddComponent<BoxCollider2DComponent>();
        }
        if (mask & 2)
        {
            auto e = scene->CreateEntity("Jolt");
            e.AddComponent<Rigidbody3DComponent>();
            e.AddComponent<BoxCollider3DComponent>();
        }
        PhysicsHost host;
        host.Editor.GetActiveScene = [scene]
        { return scene; };
        for (bool live : { false, true })
        {
            if (live)
            {
                scene->OnPhysics2DStart();
                scene->OnPhysics3DStart();
            }
            const auto box = registry.Invoke(host, "olo_physics2d_list_colliders", MCP::Json::object());
            ASSERT_FALSE(box.Result.IsError);
            EXPECT_EQ(box.Result.StructuredContent["total"], mask & 1 ? 1 : 0);
            EXPECT_EQ(box.Result.StructuredContent["physicsRunning"], live);
            const auto jolt = registry.Invoke(host, "olo_physics_list_colliders", MCP::Json::object());
            ASSERT_FALSE(jolt.Result.IsError);
            EXPECT_EQ(jolt.Result.StructuredContent["total"], mask & 2 ? 1 : 0);
            EXPECT_EQ(jolt.Result.StructuredContent["hasAuthored2D"], (mask & 1) != 0);
            EXPECT_EQ(jolt.Result.StructuredContent["dimension"], 3);
        }
        scene->OnPhysics3DStop();
        scene->OnPhysics2DStop();
    }
}
