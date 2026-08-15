#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
// =============================================================================
// VisualScriptDrivesGameplayTest — issue #634, AC#8's four cross-subsystem
// proofs, each driven through a real `Scene::OnUpdateRuntime`.
//
// Cross-subsystem seams under test:
//   VisualScript × Scene scheduler × ECS transforms
//   VisualScript × Scene's deferred entity-command queue (prefab spawn)
//   VisualScript × Jolt contact events (collision reaction)
//   VisualScript × GameplayEventBus (fires an event other systems consume)
//   VisualScript × Lua (a text script triggers graph flow, and reads a variable)
//
// Why these are Functional and not unit tests: every one of them depends on the
// SCHEDULER actually running the VisualScript node inside OnUpdateRuntime, on
// the command-queue drain sitting where it does, and on the bus subscriptions
// surviving the session. VisualScriptVMTest covers the VM in isolation and
// would stay green through all of those being disconnected.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Gameplay/GameplayEventBus.h"
#include "OloEngine/Gameplay/Quest/QuestEvents.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptEvents.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptGraph.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptNodeRegistry.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptSystem.h"
#include "TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;
using namespace OloEngine::VisualScript;

namespace
{
    /// Compile `asset` and hand the resulting instance to the scene's system for
    /// `entity`, then give the entity its component.
    ///
    /// The graph reaches the entity as a compiled plan rather than through the
    /// AssetManager because the Functional harness deliberately mounts no
    /// project, so an AssetHandle cannot resolve. What these tests exercise is
    /// the SYSTEM's tick/dispatch behaviour, not asset loading —
    /// VisualScriptSerializerTest covers the file path. The component keeps its
    /// null graph handle, which is exactly the case SyncInstances leaves alone.
    void InstallGraphOn(Scene& scene, Entity entity, const Ref<VisualScriptAsset>& asset)
    {
        auto* system = scene.GetVisualScripts();
        ASSERT_NE(system, nullptr) << "EnableVisualScripting() must run before installing a graph";

        std::vector<CompileDiagnostic> errors;
        Ref<VisualScriptPlan> plan = VisualScriptPlan::Compile(*asset, errors);
        ASSERT_TRUE(plan) << (errors.empty() ? std::string("compile failed with no diagnostics") : errors[0].m_Message);

        entity.AddComponent<VisualScriptComponent>();
        system->InstallInstanceForTesting(entity.GetUUID(), plan);
    }
} // namespace

//==============================================================================
// 1. A graph moves an entity — VisualScript × scheduler × ECS transforms.
//==============================================================================

class VisualScriptMovesEntityTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        NodeRegistry::EnsureStandardLibrary();
        EnableVisualScripting();

        m_Entity = GetScene().CreateEntityWithUUID(UUID{ 7001 }, "Mover");
        m_Entity.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f);

        // OnUpdate -> Translate(self, +X * dt)
        auto asset = Ref<VisualScriptAsset>::Create();
        VisualScriptGraph& graph = asset->m_EventGraph;
        const NodeId update = graph.AddNode(std::string(NodeTypes::kOnUpdate)).m_Id;
        const NodeId make = graph.AddNode("Vector.Make").m_Id;
        const NodeId translate = graph.AddNode("Entity.Translate").m_Id;

        // X = DeltaSeconds, so the total displacement after N ticks equals the
        // elapsed simulated time — an assertion that would fail if the graph ran
        // the wrong number of times OR received a stale delta.
        graph.AddLink(update, "Delta Seconds", make, "X");
        graph.AddLink(update, "Then", translate, "Enter");
        graph.AddLink(make, "Vector", translate, "Value");

        InstallGraphOn(GetScene(), m_Entity, asset);
    }

    Entity m_Entity;
};

TEST_F(VisualScriptMovesEntityTest, GraphMovesTheEntityEveryTickThroughTheScheduler)
{
    const auto& transform = m_Entity.GetComponent<TransformComponent>();
    ASSERT_FLOAT_EQ(transform.Translation.x, 0.0f);

    constexpr f32 kDt = 1.0f / 60.0f;
    RunFrames(60, kDt);

    // 60 ticks at 1/60s, each translating by (dt, 0, 0).
    EXPECT_NEAR(transform.Translation.x, 1.0f, 1e-3f)
        << "the VisualScript scheduler node never ran, ran the wrong number of times, or "
           "Entity.Translate wrote to a copy of the TransformComponent";
    EXPECT_FLOAT_EQ(transform.Translation.y, 0.0f);
    EXPECT_FLOAT_EQ(transform.Translation.z, 0.0f);
}

TEST_F(VisualScriptMovesEntityTest, RemovingTheComponentStopsTheGraph)
{
    RunFrames(10);
    const f32 movedTo = m_Entity.GetComponent<TransformComponent>().Translation.x;
    ASSERT_GT(movedTo, 0.0f);

    // OnComponentRemoved must drop the instance. Without it the graph keeps
    // ticking until OnRuntimeStop, which reads as "removing the component did
    // nothing".
    m_Entity.RemoveComponent<VisualScriptComponent>();
    RunFrames(30);

    EXPECT_FLOAT_EQ(m_Entity.GetComponent<TransformComponent>().Translation.x, movedTo)
        << "the graph kept running after its component was removed";
    EXPECT_EQ(GetScene().GetVisualScripts()->GetInstanceCount(), 0u);
}

//==============================================================================
// 2. A graph spawns an entity — VisualScript × the deferred command queue.
//==============================================================================

class VisualScriptSpawnsEntityTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        NodeRegistry::EnsureStandardLibrary();
        EnableVisualScripting();

        m_Spawner = GetScene().CreateEntityWithUUID(UUID{ 7002 }, "Spawner");

        // OnBeginPlay -> DoOnce -> Publish "Spawned" (a bus event any other
        // system could react to) and destroy a target entity.
        //
        // Prefab instantiation needs a mounted project, which the Functional
        // harness does not provide; the DESTROY half exercises the same
        // deferred-command path and is assertable without one.
        m_Target = GetScene().CreateEntityWithUUID(UUID{ 7003 }, "Target");

        auto asset = Ref<VisualScriptAsset>::Create();
        asset->m_Variables.push_back({ "Victim", PinType::Entity, PinValue::MakeEntity(UUID(7003)) });

        VisualScriptGraph& graph = asset->m_EventGraph;
        const NodeId begin = graph.AddNode(std::string(NodeTypes::kOnBeginPlay)).m_Id;
        const NodeId get = graph.AddNode(std::string(NodeTypes::kGetVariable)).m_Id;
        graph.FindNode(get)->SetProperty(std::string(NodeProps::kVariableName), "Victim");
        const NodeId destroy = graph.AddNode("Entity.Destroy").m_Id;
        graph.AddLink(begin, "Then", destroy, "Enter");
        graph.AddLink(get, "Value", destroy, "Target");

        InstallGraphOn(GetScene(), m_Spawner, asset);
    }

    Entity m_Spawner;
    Entity m_Target;
};

TEST_F(VisualScriptSpawnsEntityTest, GraphDestroyIsAppliedWithinTheSameTick)
{
    ASSERT_TRUE(GetScene().TryGetEntityWithUUID(UUID{ 7003 }).has_value());

    // ONE tick. The system's trailing FlushPendingEntityCommands is what makes
    // this same-tick rather than next-tick; a graph destroy landing a tick late
    // is exactly the class of bug the drain placement exists to prevent.
    RunFrames(1);

    EXPECT_FALSE(GetScene().TryGetEntityWithUUID(UUID{ 7003 }).has_value())
        << "the graph's Destroy Entity did not reach the command drain inside its own tick";
    EXPECT_TRUE(GetScene().TryGetEntityWithUUID(UUID{ 7002 }).has_value()) << "the spawner must survive";
}

//==============================================================================
// 3. A graph reacts to a physics contact — VisualScript × Jolt contact events.
//==============================================================================

class VisualScriptReactsToCollisionTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        NodeRegistry::EnsureStandardLibrary();
        EnableVisualScripting();

        m_Falling = GetScene().CreateEntityWithUUID(UUID{ 7010 }, "Falling");
        m_Falling.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 3.0f, 0.0f);
        auto& fallingBody = m_Falling.AddComponent<Rigidbody3DComponent>();
        fallingBody.m_Type = BodyType3D::Dynamic;
        m_Falling.AddComponent<BoxCollider3DComponent>();

        m_Ground = GetScene().CreateEntityWithUUID(UUID{ 7011 }, "Ground");
        m_Ground.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, -0.5f, 0.0f);
        m_Ground.GetComponent<TransformComponent>().Scale = glm::vec3(20.0f, 1.0f, 20.0f);
        auto& groundBody = m_Ground.AddComponent<Rigidbody3DComponent>();
        groundBody.m_Type = BodyType3D::Static;
        m_Ground.AddComponent<BoxCollider3DComponent>();

        // OnCollisionEnter -> Set "Hits" = Hits + 1
        auto asset = Ref<VisualScriptAsset>::Create();
        asset->m_Variables.push_back({ "Hits", PinType::Int, PinValue::MakeInt(0) });

        VisualScriptGraph& graph = asset->m_EventGraph;
        const NodeId hit = graph.AddNode(std::string(NodeTypes::kOnCollisionEnter)).m_Id;
        const NodeId get = graph.AddNode(std::string(NodeTypes::kGetVariable)).m_Id;
        graph.FindNode(get)->SetProperty(std::string(NodeProps::kVariableName), "Hits");
        const NodeId add = graph.AddNode("Math.IntAdd").m_Id;
        graph.FindNode(add)->m_PinDefaults["B"] = PinValue::MakeInt(1);
        const NodeId set = graph.AddNode(std::string(NodeTypes::kSetVariable)).m_Id;
        graph.FindNode(set)->SetProperty(std::string(NodeProps::kVariableName), "Hits");

        graph.AddLink(hit, "Then", set, "Enter");
        graph.AddLink(get, "Value", add, "A");
        graph.AddLink(add, "Result", set, "Value");

        InstallGraphOn(GetScene(), m_Falling, asset);

        EnablePhysics3D();
    }

    Entity m_Falling;
    Entity m_Ground;
};

TEST_F(VisualScriptReactsToCollisionTest, ContactEventReachesTheGraph)
{
    const auto hits = [&]
    {
        const VisualScriptInstance* instance = GetScene().GetVisualScripts()->FindInstance(UUID{ 7010 });
        return instance != nullptr ? instance->GetVariable("Hits").AsInt() : -1;
    };
    ASSERT_EQ(hits(), 0);

    const bool landed = TickUntil([&]
                                  { return hits() > 0; }, /*timeoutSeconds=*/4.0f);

    EXPECT_TRUE(landed)
        << "the box fell but its graph never saw OnCollisionEnter — JoltScene::OnContactEvent no longer "
           "fans contacts into VisualScriptSystem, or the event queue is not drained inside the tick";
}

//==============================================================================
// 4. A graph and a Lua script drive each other — VisualScript × Lua × the bus.
//==============================================================================

class VisualScriptInteropsWithLuaTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        NodeRegistry::EnsureStandardLibrary();
        EnableVisualScripting();
        EnableLua();

        // Small UUID so the sol2 u64 -> Lua integer conversion is exact when the
        // test splices it into script text.
        m_Entity = GetScene().CreateEntityWithUUID(UUID{ 7020 }, "Interop");

        // Custom Event "Poke" -> Set "Pokes" = Pokes + 1
        auto asset = Ref<VisualScriptAsset>::Create();
        asset->m_Variables.push_back({ "Pokes", PinType::Int, PinValue::MakeInt(0) });
        asset->m_Variables.push_back({ "FromLua", PinType::Float, PinValue::MakeFloat(0.0f) });

        VisualScriptGraph& graph = asset->m_EventGraph;
        const NodeId poke = graph.AddNode(std::string(NodeTypes::kCustomEvent)).m_Id;
        graph.FindNode(poke)->SetProperty(std::string(NodeProps::kEventName), "Poke");
        const NodeId get = graph.AddNode(std::string(NodeTypes::kGetVariable)).m_Id;
        graph.FindNode(get)->SetProperty(std::string(NodeProps::kVariableName), "Pokes");
        const NodeId add = graph.AddNode("Math.IntAdd").m_Id;
        graph.FindNode(add)->m_PinDefaults["B"] = PinValue::MakeInt(1);
        const NodeId set = graph.AddNode(std::string(NodeTypes::kSetVariable)).m_Id;
        graph.FindNode(set)->SetProperty(std::string(NodeProps::kVariableName), "Pokes");

        graph.AddLink(poke, "Then", set, "Enter");
        graph.AddLink(get, "Value", add, "A");
        graph.AddLink(add, "Result", set, "Value");

        InstallGraphOn(GetScene(), m_Entity, asset);

        // The Lua half: poke the graph once, and write a graph variable.
        const std::string scriptSrc = R"(
local script = {}
local done = false
function script.OnUpdate(entityID, ts)
    if done then return end
    -- entity_utils hands back a UUID number, which is what every visual_script
    -- entry point takes (the whole Lua surface is id-based, not userdata-based).
    local e = entity_utils.find_by_name("Interop")
    if e == nil then return end
    visual_script.send_event(e, "Poke", "")
    visual_script.set_variable(e, "FromLua", 12.5)
    done = true
end
return script
)";
        m_ScriptPath = OloEngine::Tests::TempFile("olo_functional_vs_interop.lua");
        std::ofstream out(m_ScriptPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            throw std::runtime_error("failed to write the interop Lua script");
        }
        out << scriptSrc;
        out.flush();
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

TEST_F(VisualScriptInteropsWithLuaTest, LuaTriggersGraphFlowAndWritesAGraphVariable)
{
    const auto instance = [&]
    { return GetScene().GetVisualScripts()->FindInstance(UUID{ 7020 }); };
    ASSERT_NE(instance(), nullptr);

    const bool poked = TickUntil([&]
                                 { return instance() != nullptr && instance()->GetVariable("Pokes").AsInt() > 0; },
                                 /*timeoutSeconds=*/1.0f);

    EXPECT_TRUE(poked)
        << "visual_script.send_event never reached the graph — the Lua binding, the system's inbox drain, "
           "or the Custom Event entry key is broken";
    EXPECT_EQ(instance()->GetVariable("Pokes").AsInt(), 1) << "the one-shot script must poke exactly once";
    EXPECT_FLOAT_EQ(instance()->GetVariable("FromLua").AsFloat(), 12.5f)
        << "visual_script.set_variable did not reach the live blackboard";
}

//==============================================================================
// 5. A graph publishes onto the GameplayEventBus — VisualScript × the bus.
//==============================================================================

class VisualScriptPublishesToEventBusTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        NodeRegistry::EnsureStandardLibrary();
        EnableVisualScripting();

        m_Entity = GetScene().CreateEntityWithUUID(UUID{ 7030 }, "Publisher");

        // A non-graph subscriber, standing in for UI / audio / analytics.
        GetScene().GetGameplayEvents().Subscribe<VisualScriptCustomEvent>(
            [this](const VisualScriptCustomEvent& event)
            { m_Received.push_back(event.m_Name); });

        // OnBeginPlay -> Publish "LevelCleared" with To Gameplay Bus = true
        auto asset = Ref<VisualScriptAsset>::Create();
        VisualScriptGraph& graph = asset->m_EventGraph;
        const NodeId begin = graph.AddNode(std::string(NodeTypes::kOnBeginPlay)).m_Id;
        const NodeId publish = graph.AddNode("Utility.PublishEvent").m_Id;
        graph.FindNode(publish)->m_PinDefaults["Event Name"] = PinValue::MakeString("LevelCleared");
        graph.FindNode(publish)->m_PinDefaults["To Gameplay Bus"] = PinValue::MakeBool(true);
        graph.AddLink(begin, "Then", publish, "Enter");

        InstallGraphOn(GetScene(), m_Entity, asset);
    }

    Entity m_Entity;
    std::vector<std::string> m_Received;
};

TEST_F(VisualScriptPublishesToEventBusTest, GraphEventReachesANonGraphBusSubscriber)
{
    ASSERT_TRUE(m_Received.empty());
    RunFrames(3);

    ASSERT_FALSE(m_Received.empty())
        << "Publish Event with To Gameplay Bus set never reached the bus — the outbox drain does not "
           "publish, or the scheduler node is not running";
    EXPECT_EQ(m_Received[0], "LevelCleared");
    EXPECT_EQ(m_Received.size(), 1u) << "OnBeginPlay fires once, so the event must publish exactly once";
}

TEST_F(VisualScriptPublishesToEventBusTest, ABusPublishedEventReachesListeningGraphsExactlyOnce)
{
    // Regression: the outbox drain used to BOTH queue the event locally AND publish
    // it to the bus — and this system subscribes to VisualScriptCustomEvent itself,
    // so the publish came straight back as a second delivery. Every listening
    // Custom Event node fired twice per publish. Counting the non-graph subscriber
    // (the test above) cannot see that; only a listening GRAPH can.
    Entity listener = GetScene().CreateEntityWithUUID(UUID{ 7031 }, "Listener");

    auto asset = Ref<VisualScriptAsset>::Create();
    asset->m_Variables.push_back({ "Count", PinType::Int, PinValue::MakeInt(0) });

    VisualScriptGraph& graph = asset->m_EventGraph;
    const NodeId heard = graph.AddNode(std::string(NodeTypes::kCustomEvent)).m_Id;
    graph.FindNode(heard)->SetProperty(std::string(NodeProps::kEventName), "LevelCleared");
    const NodeId get = graph.AddNode(std::string(NodeTypes::kGetVariable)).m_Id;
    graph.FindNode(get)->SetProperty(std::string(NodeProps::kVariableName), "Count");
    const NodeId add = graph.AddNode("Math.IntAdd").m_Id;
    graph.FindNode(add)->m_PinDefaults["B"] = PinValue::MakeInt(1);
    const NodeId set = graph.AddNode(std::string(NodeTypes::kSetVariable)).m_Id;
    graph.FindNode(set)->SetProperty(std::string(NodeProps::kVariableName), "Count");

    graph.AddLink(heard, "Then", set, "Enter");
    graph.AddLink(get, "Value", add, "A");
    graph.AddLink(add, "Result", set, "Value");

    InstallGraphOn(GetScene(), listener, asset);

    RunFrames(10);

    const VisualScriptInstance* instance = GetScene().GetVisualScripts()->FindInstance(UUID{ 7031 });
    ASSERT_NE(instance, nullptr);
    EXPECT_EQ(instance->GetVariable("Count").AsInt(), 1)
        << "the publisher's single OnBeginPlay publish must reach a listening graph EXACTLY once";
}

//==============================================================================
// 6. A quest event reaches a graph — VisualScript × GameplayEventBus, inbound.
//==============================================================================

class VisualScriptReactsToQuestEventTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        NodeRegistry::EnsureStandardLibrary();
        EnableVisualScripting();

        m_Entity = GetScene().CreateEntityWithUUID(UUID{ 7040 }, "Listener");

        // On Gameplay Event "QuestCompleted" -> Set "Heard" = Payload
        auto asset = Ref<VisualScriptAsset>::Create();
        asset->m_Variables.push_back({ "Heard", PinType::String, PinValue::MakeString("") });

        VisualScriptGraph& graph = asset->m_EventGraph;
        const NodeId event = graph.AddNode(std::string(NodeTypes::kOnGameplayEvent)).m_Id;
        graph.FindNode(event)->SetProperty(std::string(NodeProps::kEventName), "QuestCompleted");
        const NodeId set = graph.AddNode(std::string(NodeTypes::kSetVariable)).m_Id;
        graph.FindNode(set)->SetProperty(std::string(NodeProps::kVariableName), "Heard");

        graph.AddLink(event, "Then", set, "Enter");
        graph.AddLink(event, "Payload", set, "Value");

        InstallGraphOn(GetScene(), m_Entity, asset);
    }

    Entity m_Entity;
};

TEST_F(VisualScriptReactsToQuestEventTest, QuestCompletionReachesTheGraph)
{
    // Published the way QuestSystem publishes it. The subscription was made at
    // InitVisualScriptRuntime; if it were per-instance instead of per-scene, an
    // entity destroyed earlier would have left a dangling handler here.
    QuestCompletedEvent completed;
    completed.EntityID = UUID{ 7040 };
    completed.QuestID = "FindTheAmulet";
    GetScene().GetGameplayEvents().Publish(completed);

    const auto heard = [&]
    {
        const VisualScriptInstance* instance = GetScene().GetVisualScripts()->FindInstance(UUID{ 7040 });
        return instance != nullptr ? instance->GetVariable("Heard").AsString() : std::string{};
    };

    const bool reacted = TickUntil([&]
                                   { return !heard().empty(); }, /*timeoutSeconds=*/1.0f);

    EXPECT_TRUE(reacted) << "the bus bridge did not deliver QuestCompleted to the graph";
    EXPECT_EQ(heard(), "FindTheAmulet") << "the event's payload must reach the node's Payload output";
}
