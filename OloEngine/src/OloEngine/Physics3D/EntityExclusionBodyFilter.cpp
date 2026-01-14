#include "OloEnginePCH.h"
#include "EntityExclusionBodyFilter.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <algorithm>
#include <atomic>

namespace OloEngine
{

    // File-static atomic flag to warn only once about null user data
    static std::atomic<bool> s_NullUserDataWarned{ false };

    EntityExclusionBodyFilter::EntityExclusionBodyFilter(const std::vector<UUID>& excludedEntities)
        : m_ExcludedEntities(excludedEntities)
    {
    }

    EntityExclusionBodyFilter::EntityExclusionBodyFilter(const ExcludedEntitySet& excludedEntitySet)
        : m_ExcludedEntities(excludedEntitySet)
    {
    }

    EntityExclusionBodyFilter::EntityExclusionBodyFilter(UUID excludedEntity)
        : m_ExcludedEntities(excludedEntity)
    {
    }

    bool EntityExclusionBodyFilter::ShouldCollide(const JPH::BodyID& inBodyID) const
    {
        // This method is exception-safe and will not throw
        // Always allow initial filter check at the broad phase level
        // The actual filtering happens in ShouldCollideLocked when we have access to the body data
        (void)inBodyID; // Suppress unused parameter warning
        return true;
    }

    bool EntityExclusionBodyFilter::ShouldCollideLocked(const JPH::Body& inBody) const
    {
        // This method is exception-safe and will not throw
        // Get raw user data and validate it before using
        JPH::uint64 rawUserData = inBody.GetUserData();
        if (rawUserData == 0)
        {
            // No valid entity ID - allow collision by default
            // Use atomic test-and-set to warn only once to avoid spamming hot query paths
            if (!s_NullUserDataWarned.exchange(true, std::memory_order_relaxed))
            {
                OLO_CORE_WARN("Physics body has null user data, allowing collision (further warnings suppressed)");
            }
            return true;
        }

        // Extract entity UUID from validated user data
        UUID entityID = static_cast<UUID>(rawUserData);

        // Return false if this entity should be excluded (i.e., don't collide with excluded entities)
        return !IsEntityExcluded(entityID);
    }

    void EntityExclusionBodyFilter::AddExcludedEntity(UUID entityID)
    {
        TUniqueLock<FSharedMutex> lock(m_ExclusionMutex);
        m_ExcludedEntities.AddExcludedEntity(entityID);
    }

    void EntityExclusionBodyFilter::RemoveExcludedEntity(UUID entityID)
    {
        TUniqueLock<FSharedMutex> lock(m_ExclusionMutex);
        m_ExcludedEntities.RemoveExcludedEntity(entityID);
    }

    void EntityExclusionBodyFilter::ClearExcludedEntities()
    {
        TUniqueLock<FSharedMutex> lock(m_ExclusionMutex);
        m_ExcludedEntities.Clear();
    }

    [[nodiscard]] bool EntityExclusionBodyFilter::IsEntityExcluded(UUID entityID) const
    {
        TSharedLock<FSharedMutex> lock(m_ExclusionMutex);
        return m_ExcludedEntities.IsEntityExcluded(entityID);
    }

    [[nodiscard]] std::vector<UUID> EntityExclusionBodyFilter::GetExcludedEntities() const
    {
        TSharedLock<FSharedMutex> lock(m_ExclusionMutex);
        return m_ExcludedEntities.ToVector();
    }

} // namespace OloEngine
