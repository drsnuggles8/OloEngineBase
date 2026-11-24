#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
#include "Functional/FunctionalTest.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <array>

namespace OloEngine::Functional
{
    class BuildPlacementIdentityTest : public FunctionalTest
    {
      protected:
        void BuildScene() override {}
    };

    TEST_F(BuildPlacementIdentityTest, TenDeferredPlacementsRestoreExactEntityIdsAndTransforms)
    {
        struct Placement
        {
            UUID Id{ 0 };
            glm::vec3 Center{ 0.0f };
            glm::vec3 Size{ 1.0f };
        };
        std::array<Placement, 10> placements{};
        constexpr glm::vec4 color{ 0.1f, 0.6f, 0.5f, 1.0f };
        const auto initialCount = GetScene().GetAllEntitiesWith<IDComponent>().size();
        const auto queue = [&](const Placement& placement)
        {
            return GetScene().ScriptCreateBuildBox(placement.Id, "Building", placement.Center, placement.Size, color);
        };
        const auto checkEntities = [&]
        {
            EXPECT_EQ(GetScene().GetAllEntitiesWith<IDComponent>().size(), initialCount + placements.size());
            for (const auto& placement : placements)
            {
                const auto entity = GetScene().TryGetEntityWithUUID(placement.Id);
                ASSERT_TRUE(entity.has_value());
                const auto& transform = entity->GetComponent<TransformComponent>();
                EXPECT_TRUE(Math::BitwiseEqual(transform.Translation, placement.Center));
                EXPECT_TRUE(Math::BitwiseEqual(transform.Scale, placement.Size));
                ASSERT_TRUE(entity->HasComponent<MaterialComponent>());
                EXPECT_TRUE(Math::BitwiseEqual(entity->GetComponent<MaterialComponent>().m_Material.GetBaseColorFactor(), color));
                EXPECT_FALSE(GetScene().IsRuntimeOwned(*entity));
            }
        };

        for (sizet i = 0; i < placements.size(); ++i)
        {
            auto& placement = placements[i];
            placement.Size = { 8.0f + static_cast<f32>(i % 5), 6.0f, 10.0f };
            placement.Center = { 40.0f * static_cast<f32>(i), 6.0f, 8.0f };
            placement.Id = queue(placement);
            ASSERT_NE(static_cast<u64>(placement.Id), 0u);
            EXPECT_FALSE(GetScene().TryGetEntityWithUUID(placement.Id).has_value());
        }
        GetScene().FlushPendingEntityCommands();
        checkEntities();

        for (const auto& placement : placements)
            GetScene().ScriptDestroyEntity(placement.Id);
        GetScene().FlushPendingEntityCommands();
        EXPECT_EQ(GetScene().GetAllEntitiesWith<IDComponent>().size(), initialCount);
        for (const auto& placement : placements)
        {
            EXPECT_FALSE(GetScene().TryGetEntityWithUUID(placement.Id).has_value());
            EXPECT_EQ(static_cast<u64>(queue(placement)), static_cast<u64>(placement.Id));
            EXPECT_EQ(static_cast<u64>(queue(placement)), 0u) << "duplicate pending identity";
        }
        GetScene().FlushPendingEntityCommands();
        checkEntities();
    }

    TEST_F(BuildPlacementIdentityTest, PlacementPreviewIsRuntimeOwned)
    {
        const auto id = GetScene().ScriptCreateBuildBox(UUID(0), "Preview", glm::vec3(0.0f),
                                                        glm::vec3(1.0f), glm::vec4(1.0f), true);
        ASSERT_NE(static_cast<u64>(id), 0u);
        GetScene().FlushPendingEntityCommands();
        const auto preview = GetScene().TryGetEntityWithUUID(id);
        ASSERT_TRUE(preview.has_value());
        EXPECT_TRUE(GetScene().IsRuntimeOwned(*preview));
    }
} // namespace OloEngine::Functional
