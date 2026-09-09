// OLO_TEST_LAYER: unit

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "Automation/AutomationEntityCommands.h"
#include "Automation/AutomationHost.h"
#include "Automation/AutomationRegistry.h"
#include "MCP/McpServer.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetComponents.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "UndoRedo/EditorCommand.h"

#include <chrono>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using namespace OloEngine;
    using namespace OloEngine::Automation;
    using Json = nlohmann::json;

    std::string Id(Entity entity)
    {
        return std::to_string(static_cast<u64>(entity.GetUUID()));
    }

    std::vector<u64> Children(Entity entity)
    {
        std::vector<u64> result;
        for (UUID id : entity.Children())
            result.push_back(static_cast<u64>(id));
        return result;
    }

    class EntityAuthoringHost final : public IAutomationHost
    {
      public:
        explicit EntityAuthoringHost(MCP::EditorMcpContext context)
            : m_Context(std::move(context))
        {
        }

        [[nodiscard]] const MCP::EditorMcpContext& Context() const override
        {
            return m_Context;
        }

        [[nodiscard]] bool IsCurrentCallCancelled() const override
        {
            return false;
        }

        [[nodiscard]] bool PublishArtifact(AutomationArtifact /*artifact*/) override
        {
            return false;
        }

        [[nodiscard]] bool IsMarshalling() const
        {
            return m_Marshalling;
        }

        [[nodiscard]] int MarshalCount() const
        {
            return m_MarshalCount;
        }

      protected:
        Json MarshalReadOnMainThread(const std::function<Json()>& readJob, std::chrono::milliseconds /*timeout*/) override
        {
            // The real command runs intact; only scheduling is synchronous in this
            // one-thread fixture. Both ECS and history hooks assert this boundary.
            m_Marshalling = true;
            ++m_MarshalCount;
            const Json result = readJob();
            m_Marshalling = false;
            return result;
        }

        void EmitProgressUpdate(f64 /*progress*/, f64 /*total*/, const std::string& /*message*/) const override
        {
        }

      private:
        MCP::EditorMcpContext m_Context;
        bool m_Marshalling = false;
        int m_MarshalCount = 0;
    };

    class McpAutomationEntities : public ::testing::Test
    {
      protected:
        McpAutomationEntities()
            : m_Scene(Ref<Scene>::Create()), m_Host(MakeContext())
        {
            RegisterEntityAuthoringCommands(m_Registry);
        }

        MCP::EditorMcpContext MakeContext()
        {
            MCP::EditorMcpContext context;
            context.GetActiveScene = [this]()
            {
                EXPECT_TRUE(m_Host.IsMarshalling());
                return m_Scene;
            };
            context.GetCommandHistory = [this]() -> CommandHistory*
            {
                EXPECT_TRUE(m_Host.IsMarshalling());
                return m_HistoryAvailable ? &m_History : nullptr;
            };
            context.InvalidateEntityReferences = [this]()
            {
                // Editor callbacks must run while every held entity is still valid.
                EXPECT_GT(EntityCount(), 0u);
                ++m_SelectionClears;
            };
            return context;
        }

        AutomationInvocation Invoke(const std::string& command, const Json& args = Json::object())
        {
            return m_Registry.Invoke(m_Host, command, args, AutomationWriteConsent::Granted);
        }

        [[nodiscard]] Entity Find(const std::string& id) const
        {
            const auto entity = m_Scene->TryGetEntityWithUUID(UUID(std::stoull(id)));
            return entity ? *entity : Entity{};
        }

        [[nodiscard]] sizet EntityCount() const
        {
            return m_Scene->GetAllEntitiesWith<IDComponent>().size();
        }

        Ref<Scene> m_Scene;
        CommandHistory m_History;
        AutomationRegistry m_Registry;
        bool m_HistoryAvailable = true;
        int m_SelectionClears = 0;
        EntityAuthoringHost m_Host;
    };
} // namespace

TEST_F(McpAutomationEntities, ConsentAndEditModeAreRequiredBeforeMutating)
{
    const auto denied = m_Registry.Invoke(m_Host, "olo_entity_create", Json::object());
    EXPECT_EQ(denied.Outcome, AutomationInvocation::Status::WriteConsentWithheld);
    EXPECT_EQ(m_Host.MarshalCount(), 0);
    EXPECT_EQ(EntityCount(), 0u);
    EXPECT_FALSE(m_History.CanUndo());

    m_HistoryAvailable = false;
    const auto playing = Invoke("olo_entity_create");
    ASSERT_TRUE(playing.Ran()) << playing.Message;
    EXPECT_TRUE(playing.Result.IsError);
    EXPECT_EQ(EntityCount(), 0u);
    EXPECT_FALSE(m_History.IsDirty());

    m_HistoryAvailable = true;
    m_Scene = nullptr;
    const auto noScene = Invoke("olo_entity_create");
    ASSERT_TRUE(noScene.Ran()) << noScene.Message;
    EXPECT_TRUE(noScene.Result.IsError);
    EXPECT_FALSE(m_History.CanUndo());
}

TEST_F(McpAutomationEntities, CreateIsOneUndoStepAndRedoRetainsIdentityAndRelationshipAbsence)
{
    Entity parent = m_Scene->CreateEntity("Parent");
    ASSERT_FALSE(parent.HasComponent<RelationshipComponent>());
    const auto created = Invoke("olo_entity_create", Json{ { "name", "Child" }, { "parent", Id(parent) } });
    ASSERT_TRUE(created.Ran()) << created.Message;
    ASSERT_FALSE(created.Result.IsError) << created.Result.Content.dump();
    const auto uuid = created.Result.StructuredContent.at("entity").get<std::string>();
    EXPECT_EQ(created.Result.StructuredContent.at("parent"), Id(parent));
    ASSERT_TRUE(Find(uuid));
    EXPECT_EQ(Find(uuid).GetName(), "Child");
    EXPECT_EQ(static_cast<u64>(parent.GetParentUUID()), 0u);
    EXPECT_EQ(Children(parent), std::vector<u64>({ std::stoull(uuid) }));
    EXPECT_TRUE(m_History.IsDirty());

    m_History.Undo();
    EXPECT_FALSE(Find(uuid));
    EXPECT_FALSE(parent.HasComponent<RelationshipComponent>());
    EXPECT_FALSE(m_History.CanUndo());
    EXPECT_FALSE(m_History.IsDirty());
    EXPECT_EQ(m_SelectionClears, 1);

    m_History.Redo();
    ASSERT_TRUE(Find(uuid));
    EXPECT_EQ(Id(Find(uuid)), uuid);
    EXPECT_EQ(Id(Find(uuid).GetParent()), Id(parent));
    EXPECT_EQ(static_cast<u64>(parent.GetParentUUID()), 0u);
    EXPECT_TRUE(m_History.IsDirty());
}

TEST_F(McpAutomationEntities, CreateInsertsAtRequestedSiblingPositionAndUndoPreservesOrder)
{
    Entity parent = m_Scene->CreateEntity("Parent");
    Entity first = m_Scene->CreateEntity("First");
    Entity last = m_Scene->CreateEntity("Last");
    first.SetParent(parent);
    last.SetParent(parent);
    const auto original = Children(parent);
    const auto created = Invoke("olo_entity_create", Json{ { "parent", Id(parent) }, { "siblingIndex", 1 } });
    ASSERT_TRUE(created.Ran()) << created.Message;
    ASSERT_FALSE(created.Result.IsError) << created.Result.Content.dump();
    const auto uuid = created.Result.StructuredContent.at("entity").get<std::string>();
    const std::vector<u64> inserted{ static_cast<u64>(first.GetUUID()), std::stoull(uuid), static_cast<u64>(last.GetUUID()) };
    EXPECT_EQ(Children(parent), inserted);
    m_History.Undo();
    EXPECT_EQ(Children(parent), original);
    m_History.Redo();
    EXPECT_EQ(Children(parent), inserted);
}

TEST_F(McpAutomationEntities, DestroyRestoresOrderedHierarchyAndComponentsOutsideOldDeleteList)
{
    Entity parent = m_Scene->CreateEntity("Parent");
    Entity first = m_Scene->CreateEntity("First");
    Entity target = m_Scene->CreateEntity("Target");
    Entity last = m_Scene->CreateEntity("Last");
    first.SetParent(parent);
    target.SetParent(parent);
    last.SetParent(parent);
    Entity child = m_Scene->CreateEntity("Surviving child");
    child.SetParent(target);
    const std::string uuid = Id(target);
    const auto originalOrder = Children(parent);
    target.AddComponent<MorphTargetComponent>().Weights["smile"] = 0.75f;
    target.AddComponent<ParticleSystemComponent>().System.Emitter.RateOverTime = 137.0f;

    const auto destroyed = Invoke("olo_entity_destroy", Json{ { "entity", uuid } });
    ASSERT_TRUE(destroyed.Ran()) << destroyed.Message;
    ASSERT_FALSE(destroyed.Result.IsError) << destroyed.Result.Content.dump();
    EXPECT_EQ(destroyed.Result.StructuredContent.at("entity"), uuid);
    EXPECT_FALSE(Find(uuid));
    EXPECT_EQ(static_cast<u64>(child.GetParentUUID()), 0u);
    EXPECT_EQ(EntityCount(), 4u);

    m_History.Undo();
    ASSERT_TRUE(Find(uuid));
    target = Find(uuid);
    ASSERT_TRUE(target.HasComponent<MorphTargetComponent>());
    EXPECT_FLOAT_EQ(target.GetComponent<MorphTargetComponent>().Weights.at("smile"), 0.75f);
    ASSERT_TRUE(target.HasComponent<ParticleSystemComponent>());
    EXPECT_FLOAT_EQ(target.GetComponent<ParticleSystemComponent>().System.Emitter.RateOverTime, 137.0f);
    EXPECT_EQ(Children(parent), originalOrder);
    EXPECT_EQ(Id(child.GetParent()), uuid);
    EXPECT_FALSE(m_History.CanUndo());
    EXPECT_FALSE(m_History.IsDirty());

    m_History.Redo();
    EXPECT_FALSE(Find(uuid));
    EXPECT_EQ(static_cast<u64>(child.GetParentUUID()), 0u);
    EXPECT_EQ(EntityCount(), 4u);
}

TEST_F(McpAutomationEntities, DeleteUndoRestoresAbsentRelationshipAndEmptyAuthoredName)
{
    Entity target = m_Scene->CreateEntity("Target");
    const std::string uuid = Id(target);
    target.GetComponent<TagComponent>().Tag.clear();
    m_Scene->UpdateEntityName(static_cast<entt::entity>(target), "Target", "");
    ASSERT_FALSE(target.HasComponent<RelationshipComponent>());
    const auto deleted = Invoke("olo_entity_destroy", Json{ { "entity", uuid } });
    ASSERT_TRUE(deleted.Ran()) << deleted.Message;
    ASSERT_FALSE(deleted.Result.IsError) << deleted.Result.Content.dump();
    m_History.Undo();
    ASSERT_TRUE(Find(uuid));
    EXPECT_TRUE(Find(uuid).GetName().empty());
    EXPECT_FALSE(Find(uuid).HasComponent<RelationshipComponent>());
}

TEST_F(McpAutomationEntities, DuplicatePreservesCloneUuidAndDataWithoutCloningChildren)
{
    Entity source = m_Scene->CreateEntity("Source");
    Entity child = m_Scene->CreateEntity("Original child");
    child.SetParent(source);
    source.AddComponent<MorphTargetComponent>().Weights["blink"] = 0.5f;
    source.AddComponent<CameraComponent>().Primary = true;
    source.GetComponent<TransformComponent>().Translation.x = 12.5f;
    const auto duplicated = Invoke("olo_entity_duplicate", Json{ { "entity", Id(source) }, { "name", "Copy" } });
    ASSERT_TRUE(duplicated.Ran()) << duplicated.Message;
    ASSERT_FALSE(duplicated.Result.IsError) << duplicated.Result.Content.dump();
    const auto uuid = duplicated.Result.StructuredContent.at("entity").get<std::string>();
    EXPECT_NE(uuid, Id(source));
    EXPECT_EQ(EntityCount(), 3u);
    ASSERT_TRUE(Find(uuid));
    EXPECT_EQ(Find(uuid).GetName(), "Copy");
    EXPECT_TRUE(Find(uuid).Children().empty());
    EXPECT_FALSE(Find(uuid).GetParent());
    EXPECT_FALSE(Find(uuid).GetComponent<CameraComponent>().Primary);
    EXPECT_EQ(Id(child.GetParent()), Id(source));

    m_History.Undo();
    EXPECT_FALSE(Find(uuid));
    EXPECT_EQ(EntityCount(), 2u);
    EXPECT_FALSE(m_History.CanUndo());
    m_History.Redo();
    ASSERT_TRUE(Find(uuid));
    EXPECT_EQ(Id(Find(uuid)), uuid);
    EXPECT_EQ(Find(uuid).GetName(), "Copy");
    EXPECT_FLOAT_EQ(Find(uuid).GetComponent<MorphTargetComponent>().Weights.at("blink"), 0.5f);
    EXPECT_FLOAT_EQ(Find(uuid).GetComponent<TransformComponent>().Translation.x, 12.5f);
    EXPECT_TRUE(Find(uuid).Children().empty());
    EXPECT_FALSE(Find(uuid).GetComponent<CameraComponent>().Primary);
}

TEST_F(McpAutomationEntities, ReparentUndoRestoresOriginalSiblingIndexAndNewParentAbsence)
{
    Entity oldParent = m_Scene->CreateEntity("Old parent");
    Entity newParent = m_Scene->CreateEntity("New parent");
    Entity first = m_Scene->CreateEntity("First");
    Entity target = m_Scene->CreateEntity("Target");
    Entity last = m_Scene->CreateEntity("Last");
    first.SetParent(oldParent);
    target.SetParent(oldParent);
    last.SetParent(oldParent);
    target.GetComponent<TransformComponent>().Translation.x = -3.25f;
    const auto original = Children(oldParent);
    const auto moved = Invoke("olo_entity_reparent", Json{ { "entity", Id(target) }, { "parent", Id(newParent) } });
    ASSERT_TRUE(moved.Ran()) << moved.Message;
    ASSERT_FALSE(moved.Result.IsError) << moved.Result.Content.dump();
    EXPECT_EQ(Id(target.GetParent()), Id(newParent));
    EXPECT_FLOAT_EQ(target.GetComponent<TransformComponent>().Translation.x, -3.25f);
    m_History.Undo();
    EXPECT_EQ(Children(oldParent), original);
    EXPECT_FALSE(newParent.HasComponent<RelationshipComponent>());
    EXPECT_EQ(Id(target.GetParent()), Id(oldParent));
    EXPECT_FALSE(m_History.CanUndo());
    m_History.Redo();
    EXPECT_EQ(Id(target.GetParent()), Id(newParent));
}

TEST_F(McpAutomationEntities, ReparentOfUnrelatedRootsUndoRemovesBothNewRelationships)
{
    Entity parent = m_Scene->CreateEntity("Parent");
    Entity target = m_Scene->CreateEntity("Target");
    const auto moved = Invoke("olo_entity_reparent", Json{ { "entity", Id(target) }, { "parent", Id(parent) } });
    ASSERT_TRUE(moved.Ran()) << moved.Message;
    ASSERT_FALSE(moved.Result.IsError) << moved.Result.Content.dump();
    m_History.Undo();
    EXPECT_FALSE(parent.HasComponent<RelationshipComponent>());
    EXPECT_FALSE(target.HasComponent<RelationshipComponent>());
    m_History.Redo();
    EXPECT_EQ(Id(target.GetParent()), Id(parent));
}

TEST_F(McpAutomationEntities, SameParentReorderIsUndoableAndNoOpPreservesRedo)
{
    Entity parent = m_Scene->CreateEntity("Parent");
    Entity first = m_Scene->CreateEntity("First");
    Entity second = m_Scene->CreateEntity("Second");
    first.SetParent(parent);
    second.SetParent(parent);
    const auto original = Children(parent);
    const auto moved = Invoke("olo_entity_reparent", Json{ { "entity", Id(first) }, { "parent", Id(parent) }, { "siblingIndex", 1 } });
    ASSERT_TRUE(moved.Ran()) << moved.Message;
    ASSERT_FALSE(moved.Result.IsError) << moved.Result.Content.dump();
    EXPECT_EQ(Children(parent), std::vector<u64>({ static_cast<u64>(second.GetUUID()), static_cast<u64>(first.GetUUID()) }));
    m_History.Undo();
    EXPECT_EQ(Children(parent), original);
    ASSERT_TRUE(m_History.CanRedo());

    const auto unchanged = Invoke("olo_entity_reparent", Json{ { "entity", Id(first) }, { "parent", Id(parent) }, { "siblingIndex", 0 } });
    ASSERT_TRUE(unchanged.Ran()) << unchanged.Message;
    ASSERT_FALSE(unchanged.Result.IsError);
    EXPECT_EQ(unchanged.Result.StructuredContent.at("changed"), false);
    EXPECT_EQ(unchanged.Result.StructuredContent.at("undoable"), false);
    EXPECT_FALSE(m_History.CanUndo());
    EXPECT_FALSE(m_History.IsDirty());
    EXPECT_TRUE(m_History.CanRedo());
}

TEST_F(McpAutomationEntities, InvalidIdentitiesParentsCyclesAndIndicesLeaveSceneAndHistoryUntouched)
{
    Entity parent = m_Scene->CreateEntity("Parent");
    Entity child = m_Scene->CreateEntity("Child");
    child.SetParent(parent);
    const auto original = Children(parent);
    const std::vector<std::pair<std::string, Json>> invalid{
        { "olo_entity_create", { { "parent", "18446744073709551615" } } },
        { "olo_entity_create", { { "siblingIndex", 0 } } },
        { "olo_entity_create", { { "parent", Id(parent) }, { "siblingIndex", 2 } } },
        { "olo_entity_destroy", { { "entity", "0" } } },
        { "olo_entity_destroy", { { "entity", "18446744073709551616" } } },
        { "olo_entity_destroy", { { "entity", "12suffix" } } },
        { "olo_entity_duplicate", { { "entity", "-1" } } },
        { "olo_entity_duplicate", { { "entity", Id(child) }, { "parent", "missing" } } },
        { "olo_entity_reparent", { { "entity", Id(parent) }, { "parent", Id(child) } } },
        { "olo_entity_reparent", { { "entity", Id(child) }, { "parent", Id(child) } } },
        { "olo_entity_reparent", { { "entity", Id(child) }, { "parent", Id(parent) }, { "siblingIndex", 1 } } },
    };
    for (const auto& [command, args] : invalid)
    {
        SCOPED_TRACE(command + " " + args.dump());
        const auto result = Invoke(command, args);
        ASSERT_TRUE(result.Ran()) << result.Message;
        EXPECT_TRUE(result.Result.IsError);
        EXPECT_EQ(EntityCount(), 2u);
        EXPECT_EQ(Children(parent), original);
        EXPECT_EQ(Id(child.GetParent()), Id(parent));
        EXPECT_FALSE(m_History.CanUndo());
        EXPECT_FALSE(m_History.IsDirty());
    }
    const auto unknownField = Invoke("olo_entity_create", Json{ { "unexpected", true } });
    EXPECT_EQ(unknownField.Outcome, AutomationInvocation::Status::InvalidArguments);
    EXPECT_EQ(EntityCount(), 2u);
    EXPECT_FALSE(m_History.CanUndo());
}
