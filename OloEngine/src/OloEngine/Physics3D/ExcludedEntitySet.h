#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"

#include <initializer_list>
#include <unordered_set>

namespace OloEngine
{
    // @brief The entities a physics scene query ignores, with O(1) lookup.
    //
    // Carried by RayCastInfo / ShapeCastInfo / ShapeOverlapInfo and read by
    // EntityExclusionBodyFilter while Jolt walks the candidate bodies.
    class ExcludedEntitySet
    {
      public:
        ExcludedEntitySet() = default;
        explicit ExcludedEntitySet(UUID excludedEntity)
            : m_ExcludedEntities{ excludedEntity } {}
        ExcludedEntitySet(std::initializer_list<UUID> excludedEntities)
            : m_ExcludedEntities(excludedEntities) {}

        [[nodiscard]] bool IsEntityExcluded(UUID entityID) const noexcept
        {
            return m_ExcludedEntities.contains(entityID);
        }

        void AddExcludedEntity(UUID entityID)
        {
            m_ExcludedEntities.insert(entityID);
        }

        void RemoveExcludedEntity(UUID entityID)
        {
            m_ExcludedEntities.erase(entityID);
        }

        void Clear() noexcept
        {
            m_ExcludedEntities.clear();
        }

        [[nodiscard]] bool Empty() const noexcept
        {
            return m_ExcludedEntities.empty();
        }

        [[nodiscard]] sizet Size() const noexcept
        {
            return m_ExcludedEntities.size();
        }

        auto operator==(const ExcludedEntitySet&) const -> bool = default;

      private:
        std::unordered_set<UUID> m_ExcludedEntities;
    };

} // namespace OloEngine
