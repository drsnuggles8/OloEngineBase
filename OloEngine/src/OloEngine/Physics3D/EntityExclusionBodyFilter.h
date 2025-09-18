#pragma once

#include "Physics3DTypes.h"
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyFilter.h>

// Forward declarations
namespace JPH {
	class BodyID;
	class Body;
}

namespace OloEngine {

	/**
	 * @brief Body filter for excluding specific entities from physics queries
	 * 
	 * This filter implements Jolt's BodyFilter interface to allow scene queries
	 * (raycasts, shape casts, overlaps) to exclude specific entities by their UUID.
	 * This is useful for scenarios like:
	 * - Player character not hitting themselves when shooting
	 * - AI raycast vision not detecting their own body
	 * - Preventing objects from casting against their parent entity
	 */
	class EntityExclusionBodyFilter : public JPH::BodyFilter
	{
	public:
		/**
		 * @brief Constructor with excluded entities list
		 * @param excludedEntities Vector of entity UUIDs to exclude from queries
		 */
		EntityExclusionBodyFilter(const std::vector<UUID>& excludedEntities);

		/**
		 * @brief Constructor with single excluded entity
		 * @param excludedEntity Single entity UUID to exclude from queries
		 */
		EntityExclusionBodyFilter(UUID excludedEntity);

		/**
		 * @brief Default constructor - no entities excluded
		 */
		EntityExclusionBodyFilter() = default;

		// JPH::BodyFilter interface implementation
		virtual bool ShouldCollide(const JPH::BodyID& inBodyID) const override;
		virtual bool ShouldCollideLocked(const JPH::Body& inBody) const override;

		/**
		 * @brief Add an entity to the exclusion list
		 * @param entityID UUID of entity to exclude
		 */
		void AddExcludedEntity(UUID entityID);

		/**
		 * @brief Remove an entity from the exclusion list
		 * @param entityID UUID of entity to remove from exclusion
		 */
		void RemoveExcludedEntity(UUID entityID);

		/**
		 * @brief Clear all excluded entities
		 */
		void ClearExcludedEntities();

		/**
		 * @brief Check if an entity is in the exclusion list
		 * @param entityID UUID to check
		 * @return true if entity is excluded, false otherwise
		 */
		bool IsEntityExcluded(UUID entityID) const;

		/**
		 * @brief Get the list of excluded entities
		 * @return Const reference to the exclusion list
		 */
		const std::vector<UUID>& GetExcludedEntities() const { return m_ExcludedEntities; }

	private:
		std::vector<UUID> m_ExcludedEntities;
	};

}