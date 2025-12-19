#pragma once

#include "EntityExclusionUtils.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include <vector>
#include <shared_mutex>
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyFilter.h>

namespace OloEngine
{

    // @brief Body filter for excluding specific entities from physics queries
    //
    // This filter implements Jolt's BodyFilter interface to allow scene queries
    // (raycasts, shape casts, overlaps) to exclude specific entities by their UUID.
    // This is useful for scenarios like:
    // - Player character not hitting themselves when shooting
    // - AI raycast vision not detecting their own body
    // - Preventing objects from casting against their parent entity
    class EntityExclusionBodyFilter : public JPH::BodyFilter
    {
      public:
        // @brief Constructor with excluded entities list
        // @param excludedEntities Vector of entity UUIDs to exclude from queries
        explicit EntityExclusionBodyFilter(const std::vector<UUID>& excludedEntities);

        // @brief Constructor with ExcludedEntitySet (optimized)
        // @param excludedEntitySet Pre-constructed set of entity UUIDs to exclude from queries
        explicit EntityExclusionBodyFilter(const ExcludedEntitySet& excludedEntitySet);

        // @brief Constructor with single excluded entity
        // @param excludedEntity Single entity UUID to exclude from queries
        explicit EntityExclusionBodyFilter(UUID excludedEntity);

        // @brief Default constructor - no entities excluded
        EntityExclusionBodyFilter() = default;

        // Explicitly delete copy and move operations due to shared_mutex member
        EntityExclusionBodyFilter(const EntityExclusionBodyFilter&) = delete;
        EntityExclusionBodyFilter& operator=(const EntityExclusionBodyFilter&) = delete;
        EntityExclusionBodyFilter(EntityExclusionBodyFilter&&) = delete;
        EntityExclusionBodyFilter& operator=(EntityExclusionBodyFilter&&) = delete;

        // @brief Virtual destructor for proper polymorphic destruction
        virtual ~EntityExclusionBodyFilter() = default;

        // JPH::BodyFilter interface implementation
        // Note: These methods are exception-safe and designed not to throw,
        // but cannot be marked noexcept due to base class signature constraints
        virtual bool ShouldCollide(const JPH::BodyID& inBodyID) const override;
        virtual bool ShouldCollideLocked(const JPH::Body& inBody) const override;

        // @brief Add an entity to the exclusion list
        // @param entityID UUID of entity to exclude
        void AddExcludedEntity(UUID entityID);

        // @brief Remove an entity from the exclusion list
        // @param entityID UUID of entity to remove from exclusion
        void RemoveExcludedEntity(UUID entityID);

        // @brief Clear all excluded entities
        void ClearExcludedEntities();

        // @brief Check if an entity is in the exclusion list
        // @param entityID UUID to check
        // @return true if entity is excluded, false otherwise
        [[nodiscard]] bool IsEntityExcluded(UUID entityID) const;

        // @brief Get the list of excluded entities
        // @return Vector containing all excluded entities
        [[nodiscard]] std::vector<UUID> GetExcludedEntities() const;

      private:
        ExcludedEntitySet m_ExcludedEntities;
        mutable std::shared_mutex m_ExclusionMutex; // Protects m_ExcludedEntities for thread-safe access
    };

} // namespace OloEngine
