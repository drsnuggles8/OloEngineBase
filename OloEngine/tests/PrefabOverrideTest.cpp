#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Prefab.h"

#include <string>
#include <unordered_set>

using namespace OloEngine; // NOLINT(google-build-using-namespace)

// Helper to access protected SetHandle for testing
class TestPrefab : public Prefab
{
  public:
    void SetTestHandle(AssetHandle handle) { SetHandle(handle); }
};

static Ref<Prefab> CreateTestPrefab()
{
    auto prefab = Ref<TestPrefab>::Create();
    prefab->SetTestHandle(UUID{});
    return prefab;
}

// =============================================================================
// Test fixture for Prefab overrides and nesting
// =============================================================================

class PrefabOverrideTest : public ::testing::Test
{
  protected:
    Ref<Scene> m_Scene;

    void SetUp() override
    {
        m_Scene = Scene::Create();
    }

    void TearDown() override
    {
        m_Scene = nullptr;
    }
};

// =============================================================================
// PrefabComponent override tracking
// =============================================================================

TEST_F(PrefabOverrideTest, PrefabComponent_DefaultHasNoOverrides)
{
    PrefabComponent pc;
    EXPECT_FALSE(pc.HasAnyOverrides());
    EXPECT_TRUE(pc.OverriddenComponents.empty());
    EXPECT_TRUE(pc.AddedComponents.empty());
    EXPECT_TRUE(pc.RemovedComponents.empty());
}

TEST_F(PrefabOverrideTest, PrefabComponent_MarkAndQueryOverride)
{
    PrefabComponent pc{UUID{}, UUID{}};
    pc.MarkComponentOverridden("TransformComponent");
    EXPECT_TRUE(pc.IsComponentOverridden("TransformComponent"));
    EXPECT_FALSE(pc.IsComponentOverridden("MeshComponent"));
    EXPECT_TRUE(pc.HasAnyOverrides());
}

TEST_F(PrefabOverrideTest, PrefabComponent_ClearSingleOverride)
{
    PrefabComponent pc{UUID{}, UUID{}};
    pc.MarkComponentOverridden("TransformComponent");
    pc.MarkComponentOverridden("MeshComponent");
    pc.ClearComponentOverride("TransformComponent");
    EXPECT_FALSE(pc.IsComponentOverridden("TransformComponent"));
    EXPECT_TRUE(pc.IsComponentOverridden("MeshComponent"));
}

TEST_F(PrefabOverrideTest, PrefabComponent_ClearAllOverrides)
{
    PrefabComponent pc{UUID{}, UUID{}};
    pc.MarkComponentOverridden("TransformComponent");
    pc.AddedComponents.insert("ScriptComponent");
    pc.RemovedComponents.insert("CameraComponent");
    EXPECT_TRUE(pc.HasAnyOverrides());

    pc.ClearAllOverrides();
    EXPECT_FALSE(pc.HasAnyOverrides());
}

TEST_F(PrefabOverrideTest, PrefabComponent_AddedAndRemoved)
{
    PrefabComponent pc{UUID{}, UUID{}};
    pc.AddedComponents.insert("ScriptComponent");
    EXPECT_TRUE(pc.IsComponentAdded("ScriptComponent"));
    EXPECT_FALSE(pc.IsComponentRemoved("ScriptComponent"));

    pc.RemovedComponents.insert("CameraComponent");
    EXPECT_TRUE(pc.IsComponentRemoved("CameraComponent"));
    EXPECT_FALSE(pc.IsComponentAdded("CameraComponent"));
}

// =============================================================================
// Prefab creation and instantiation (basic)
// =============================================================================

TEST_F(PrefabOverrideTest, Prefab_CreateFromEntity)
{
    Entity source = m_Scene->CreateEntity("TestEntity");
    source.GetComponent<TransformComponent>().Translation = glm::vec3(1.0f, 2.0f, 3.0f);
    source.AddComponent<SpriteRendererComponent>().Color = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);

    Ref<Prefab> prefab = CreateTestPrefab();
    prefab->Create(source, false);

    Entity root = prefab->GetRootEntity();
    ASSERT_TRUE(static_cast<bool>(root));
    EXPECT_EQ(root.GetComponent<TagComponent>().Tag, "TestEntity");

    auto& t = root.GetComponent<TransformComponent>();
    EXPECT_FLOAT_EQ(t.Translation.x, 1.0f);
    EXPECT_FLOAT_EQ(t.Translation.y, 2.0f);
    EXPECT_FLOAT_EQ(t.Translation.z, 3.0f);

    EXPECT_TRUE(root.HasComponent<SpriteRendererComponent>());
    EXPECT_FLOAT_EQ(root.GetComponent<SpriteRendererComponent>().Color.r, 1.0f);
}

TEST_F(PrefabOverrideTest, Prefab_InstantiateCreatesNewEntity)
{
    Entity source = m_Scene->CreateEntity("Cube");
    source.GetComponent<TransformComponent>().Scale = glm::vec3(2.0f);

    Ref<Prefab> prefab = CreateTestPrefab();
    prefab->Create(source, false);

    Ref<Scene> targetScene = Scene::Create();
    Entity instance = prefab->Instantiate(*targetScene);

    ASSERT_TRUE(static_cast<bool>(instance));
    EXPECT_EQ(instance.GetComponent<TagComponent>().Tag, "Cube");
    EXPECT_FLOAT_EQ(instance.GetComponent<TransformComponent>().Scale.x, 2.0f);
}

// =============================================================================
// Nested prefab creation (hierarchy)
// =============================================================================

TEST_F(PrefabOverrideTest, Prefab_CreatePreservesHierarchy)
{
    Entity parent = m_Scene->CreateEntity("Parent");
    parent.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f);

    Entity child = m_Scene->CreateEntity("Child");
    child.GetComponent<TransformComponent>().Translation = glm::vec3(1.0f, 0.0f, 0.0f);
    child.SetParent(parent);

    Entity grandchild = m_Scene->CreateEntity("Grandchild");
    grandchild.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 1.0f, 0.0f);
    grandchild.SetParent(child);

    Ref<Prefab> prefab = CreateTestPrefab();
    prefab->Create(parent, false);

    // The prefab's internal scene should have 3 entities
    int entityCount = 0;
    prefab->GetScene()->GetAllEntitiesWith<IDComponent>().each(
        [&](auto) { entityCount++; });
    EXPECT_EQ(entityCount, 3);

    // Root should have children
    Entity root = prefab->GetRootEntity();
    EXPECT_FALSE(root.Children().empty());
}

TEST_F(PrefabOverrideTest, Prefab_InstantiatePreservesHierarchy)
{
    Entity parent = m_Scene->CreateEntity("Parent");
    Entity child = m_Scene->CreateEntity("Child");
    child.SetParent(parent);
    child.GetComponent<TransformComponent>().Translation = glm::vec3(5.0f, 0.0f, 0.0f);

    Ref<Prefab> prefab = CreateTestPrefab();
    prefab->Create(parent, false);

    Ref<Scene> targetScene = Scene::Create();
    Entity instance = prefab->Instantiate(*targetScene);

    ASSERT_TRUE(static_cast<bool>(instance));
    EXPECT_FALSE(instance.Children().empty());

    // Get the instantiated child
    UUID childUUID = instance.Children()[0];
    auto childOpt = targetScene->TryGetEntityWithUUID(childUUID);
    ASSERT_TRUE(childOpt.has_value());

    Entity instChild = *childOpt;
    EXPECT_EQ(instChild.GetComponent<TagComponent>().Tag, "Child");
    EXPECT_FLOAT_EQ(instChild.GetComponent<TransformComponent>().Translation.x, 5.0f);
}

// =============================================================================
// Override detection
// =============================================================================

TEST_F(PrefabOverrideTest, Prefab_DetectOverrides_Added)
{
    Entity source = m_Scene->CreateEntity("Source");

    Ref<Prefab> prefab = CreateTestPrefab();
    prefab->Create(source, false);

    // Instantiate and add a component not in the prefab
    Ref<Scene> targetScene = Scene::Create();
    Entity instance = prefab->Instantiate(*targetScene);
    instance.AddComponent<SpriteRendererComponent>();

    std::unordered_set<std::string> overridden, added, removed;
    prefab->DetectOverrides(instance, overridden, added, removed);

    EXPECT_TRUE(added.contains("SpriteRendererComponent"));
    EXPECT_TRUE(removed.empty());
}

TEST_F(PrefabOverrideTest, Prefab_DetectOverrides_Removed)
{
    Entity source = m_Scene->CreateEntity("Source");
    source.AddComponent<SpriteRendererComponent>();

    Ref<Prefab> prefab = CreateTestPrefab();
    prefab->Create(source, false);

    // Instantiate and remove a component that's in the prefab
    Ref<Scene> targetScene = Scene::Create();
    Entity instance = prefab->Instantiate(*targetScene);
    ASSERT_TRUE(instance.HasComponent<SpriteRendererComponent>());
    instance.RemoveComponent<SpriteRendererComponent>();

    std::unordered_set<std::string> overridden, added, removed;
    prefab->DetectOverrides(instance, overridden, added, removed);

    EXPECT_TRUE(removed.contains("SpriteRendererComponent"));
    EXPECT_TRUE(added.empty());
}

// =============================================================================
// Revert component
// =============================================================================

TEST_F(PrefabOverrideTest, Prefab_RevertComponent)
{
    Entity source = m_Scene->CreateEntity("Source");
    source.GetComponent<TransformComponent>().Translation = glm::vec3(1.0f, 2.0f, 3.0f);

    Ref<Prefab> prefab = CreateTestPrefab();
    prefab->Create(source, false);

    Ref<Scene> targetScene = Scene::Create();
    Entity instance = prefab->Instantiate(*targetScene);

    // Modify the instance
    instance.GetComponent<TransformComponent>().Translation = glm::vec3(99.0f, 99.0f, 99.0f);
    EXPECT_FLOAT_EQ(instance.GetComponent<TransformComponent>().Translation.x, 99.0f);

    // Revert
    bool reverted = prefab->RevertComponent(instance, "TransformComponent");
    EXPECT_TRUE(reverted);
    EXPECT_FLOAT_EQ(instance.GetComponent<TransformComponent>().Translation.x, 1.0f);
    EXPECT_FLOAT_EQ(instance.GetComponent<TransformComponent>().Translation.y, 2.0f);
    EXPECT_FLOAT_EQ(instance.GetComponent<TransformComponent>().Translation.z, 3.0f);
}

TEST_F(PrefabOverrideTest, Prefab_RevertComponent_UnknownReturnsFalse)
{
    Entity source = m_Scene->CreateEntity("Source");

    Ref<Prefab> prefab = CreateTestPrefab();
    prefab->Create(source, false);

    Ref<Scene> targetScene = Scene::Create();
    Entity instance = prefab->Instantiate(*targetScene);

    bool reverted = prefab->RevertComponent(instance, "NonExistentComponent");
    EXPECT_FALSE(reverted);
}

// =============================================================================
// Apply to prefab
// =============================================================================

TEST_F(PrefabOverrideTest, Prefab_ApplyComponentToPrefab)
{
    Entity source = m_Scene->CreateEntity("Source");
    source.GetComponent<TransformComponent>().Translation = glm::vec3(1.0f);

    Ref<Prefab> prefab = CreateTestPrefab();
    prefab->Create(source, false);

    Ref<Scene> targetScene = Scene::Create();
    Entity instance = prefab->Instantiate(*targetScene);

    // Modify on instance
    instance.GetComponent<TransformComponent>().Translation = glm::vec3(42.0f);

    // Apply back to prefab
    bool applied = prefab->ApplyComponentToPrefab(instance, "TransformComponent");
    EXPECT_TRUE(applied);

    // Verify the prefab root was updated
    Entity prefabRoot = prefab->GetRootEntity();
    EXPECT_FLOAT_EQ(prefabRoot.GetComponent<TransformComponent>().Translation.x, 42.0f);
}

// =============================================================================
// Update non-overridden components
// =============================================================================

TEST_F(PrefabOverrideTest, Prefab_UpdateInstanceFromPrefab_NonOverridden)
{
    Entity source = m_Scene->CreateEntity("Source");
    source.GetComponent<TransformComponent>().Translation = glm::vec3(1.0f);
    source.AddComponent<SpriteRendererComponent>().Color = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);

    Ref<Prefab> prefab = CreateTestPrefab();
    prefab->Create(source, false);

    Ref<Scene> targetScene = Scene::Create();
    Entity instance = prefab->Instantiate(*targetScene);
    instance.AddOrReplaceComponent<PrefabComponent>(prefab->GetHandle(), instance.GetUUID());

    // Override the transform on the instance (mark it)
    instance.GetComponent<TransformComponent>().Translation = glm::vec3(99.0f);
    auto& pc = instance.GetComponent<PrefabComponent>();
    pc.MarkComponentOverridden("TransformComponent");

    // Now modify the prefab's SpriteRendererComponent
    Entity prefabRoot = prefab->GetRootEntity();
    prefabRoot.GetComponent<SpriteRendererComponent>().Color = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);

    // Update instance from prefab
    prefab->UpdateInstanceFromPrefab(instance);

    // Transform should NOT be changed (it's overridden)
    EXPECT_FLOAT_EQ(instance.GetComponent<TransformComponent>().Translation.x, 99.0f);

    // SpriteRendererComponent should be updated (not overridden)
    EXPECT_FLOAT_EQ(instance.GetComponent<SpriteRendererComponent>().Color.g, 1.0f);
}

// =============================================================================
// Cycle detection for nested prefabs
// =============================================================================

TEST_F(PrefabOverrideTest, Prefab_CycleDetection_SelfReference)
{
    UUID handle = UUID();
    std::unordered_set<AssetHandle> visited;
    EXPECT_TRUE(Prefab::WouldCreateCycle(handle, handle, visited));
}

TEST_F(PrefabOverrideTest, Prefab_HasNestedPrefabs_EmptyPrefab)
{
    Entity source = m_Scene->CreateEntity("Source");

    Ref<Prefab> prefab = CreateTestPrefab();
    prefab->Create(source, false);

    // A simple prefab with no nested prefab references should return false
    // The root entity has its own PrefabComponent pointing to this prefab,
    // so HasNestedPrefabs checks for PrefabComponents referencing OTHER prefabs
    EXPECT_FALSE(prefab->HasNestedPrefabs());
}

// =============================================================================
// Scene-level prefab management
// =============================================================================

TEST_F(PrefabOverrideTest, Scene_MarkPrefabComponentOverridden)
{
    Entity entity = m_Scene->CreateEntity("TestEntity");
    entity.AddComponent<PrefabComponent>(UUID{}, entity.GetUUID());

    m_Scene->MarkPrefabComponentOverridden(entity, "TransformComponent");

    auto& pc = entity.GetComponent<PrefabComponent>();
    EXPECT_TRUE(pc.IsComponentOverridden("TransformComponent"));
}

TEST_F(PrefabOverrideTest, Scene_MarkPrefabOverridden_NoPrefabComponent)
{
    Entity entity = m_Scene->CreateEntity("TestEntity");
    // Should not crash when entity has no PrefabComponent
    m_Scene->MarkPrefabComponentOverridden(entity, "TransformComponent");
    EXPECT_FALSE(entity.HasComponent<PrefabComponent>());
}
