#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include <unordered_set>
#include <vector>

namespace OloEngine {

	// @brief High-performance entity exclusion utility using O(1) lookup
	// 
	// This utility provides efficient entity exclusion checking using std::unordered_set
	// for O(1) average-case lookup performance. It includes adapter functions to work
	// with vector-based exclusion lists without repeated linear scans.
	class ExcludedEntitySet
	{
	public:
		// @brief Default constructor - no entities excluded
		ExcludedEntitySet() = default;

		// @brief Constructor from vector of excluded entities
		// @param excludedEntities Vector of entity UUIDs to exclude
		explicit ExcludedEntitySet(const std::vector<UUID>& excludedEntities);

		// @brief Constructor with single excluded entity
		// @param excludedEntity Single entity UUID to exclude
		explicit ExcludedEntitySet(UUID excludedEntity);

		// @brief Copy constructor
		ExcludedEntitySet(const ExcludedEntitySet& other) = default;

		// @brief Move constructor
		ExcludedEntitySet(ExcludedEntitySet&& other) noexcept = default;

		// @brief Copy assignment operator
		ExcludedEntitySet& operator=(const ExcludedEntitySet& other) = default;

		// @brief Move assignment operator
		ExcludedEntitySet& operator=(ExcludedEntitySet&& other) noexcept = default;

		// @brief Check if an entity is excluded (O(1) average case)
		// @param entityID UUID to check
		// @return true if entity is excluded, false otherwise
		bool IsEntityExcluded(UUID entityID) const noexcept;

		// @brief Add an entity to the exclusion set
		// @param entityID UUID of entity to exclude
		void AddExcludedEntity(UUID entityID);

		// @brief Remove an entity from the exclusion set
		// @param entityID UUID of entity to remove from exclusion
		void RemoveExcludedEntity(UUID entityID);

		// @brief Clear all excluded entities
		void Clear() noexcept;

		// @brief Check if the exclusion set is empty
		// @return true if no entities are excluded, false otherwise
		[[nodiscard]] bool Empty() const noexcept;

		// @brief Get the number of excluded entities
		// @return Number of excluded entities
		[[nodiscard]] sizet Size() const noexcept;

		// @brief Get a vector containing all excluded entities
		// @return Vector of excluded entity UUIDs
		[[nodiscard]] std::vector<UUID> ToVector() const;

		// @brief Update the exclusion set with new entities from a vector
		// @param excludedEntities Vector of entity UUIDs to exclude
		void UpdateFromVector(const std::vector<UUID>& excludedEntities);

	private:
		std::unordered_set<UUID> m_ExcludedEntities;
	};

	// Namespace-level utility functions for backward compatibility and convenience

	namespace EntityExclusionUtils {

		// @brief Check if an entity is excluded using an ExcludedEntitySet (O(1) average case)
		// @param excludedEntitySet The exclusion set to check against
		// @param entityID UUID to check
		// @return true if entity is excluded, false otherwise
		inline bool IsEntityExcluded(const ExcludedEntitySet& excludedEntitySet, UUID entityID)
		{
			return excludedEntitySet.IsEntityExcluded(entityID);
		}

		// @brief Check if an entity is excluded using a vector (O(n) - use sparingly)
		// @param excludedEntities Vector of excluded entity UUIDs
		// @param entityID UUID to check
		// @return true if entity is excluded, false otherwise
		// @note This function is provided for backward compatibility but has O(n) performance.
		//       Consider converting to ExcludedEntitySet for better performance.
		bool IsEntityExcluded(const std::vector<UUID>& excludedEntities, UUID entityID) noexcept;

		// @brief Create an ExcludedEntitySet from a vector for efficient repeated lookups
		// @param excludedEntities Vector of entity UUIDs to exclude
		// @return ExcludedEntitySet for O(1) lookups
		// @note Use this when you need to perform multiple exclusion checks with the same vector
		inline ExcludedEntitySet CreateExclusionSet(const std::vector<UUID>& excludedEntities)
		{
			return ExcludedEntitySet{excludedEntities};
		}

		// @brief Create an ExcludedEntitySet with a single entity
		// @param excludedEntity Single entity UUID to exclude
		// @return ExcludedEntitySet for O(1) lookups
		inline ExcludedEntitySet CreateExclusionSet(UUID excludedEntity)
		{
			return ExcludedEntitySet{excludedEntity};
		}

	} // namespace EntityExclusionUtils

} // namespace OloEngine
