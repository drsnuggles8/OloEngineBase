#include "EntityExclusionUtils.h"
#include <algorithm>

namespace OloEngine {

	ExcludedEntitySet::ExcludedEntitySet(const std::vector<UUID>& excludedEntities)
		: m_ExcludedEntities(excludedEntities.begin(), excludedEntities.end())
	{
	}

	ExcludedEntitySet::ExcludedEntitySet(UUID excludedEntity)
		: m_ExcludedEntities{excludedEntity}
	{
	}

	bool ExcludedEntitySet::IsEntityExcluded(UUID entityID) const noexcept
	{
		return m_ExcludedEntities.contains(entityID);
	}

	void ExcludedEntitySet::AddExcludedEntity(UUID entityID)
	{
		m_ExcludedEntities.insert(entityID);
	}

	void ExcludedEntitySet::RemoveExcludedEntity(UUID entityID)
	{
		m_ExcludedEntities.erase(entityID);
	}

	void ExcludedEntitySet::Clear() noexcept
	{
		m_ExcludedEntities.clear();
	}

	[[nodiscard]] bool ExcludedEntitySet::Empty() const noexcept
	{
		return m_ExcludedEntities.empty();
	}

	[[nodiscard]] sizet ExcludedEntitySet::Size() const noexcept
	{
		return m_ExcludedEntities.size();
	}

	[[nodiscard]] std::vector<UUID> ExcludedEntitySet::ToVector() const
	{
		return std::vector<UUID>(m_ExcludedEntities.begin(), m_ExcludedEntities.end());
	}

	void ExcludedEntitySet::UpdateFromVector(const std::vector<UUID>& excludedEntities)
	{
		// Construct temporary set from the vector - if this throws, m_ExcludedEntities remains unchanged
		std::unordered_set<UUID> tempSet;
		tempSet.reserve(excludedEntities.size()); // Reserve space to avoid rehashes during bulk insert
		tempSet.insert(excludedEntities.begin(), excludedEntities.end());
		// Swap is noexcept and provides strong exception safety
		m_ExcludedEntities.swap(tempSet);
	}

	namespace EntityExclusionUtils {

		// WARNING: O(n) linear search performance - use ExcludedEntitySet for frequent lookups
		// This function is provided for backward compatibility but performs a linear search.
		// For multiple lookups with the same vector, convert to ExcludedEntitySet first:
		//   auto exclusionSet = CreateExclusionSet(excludedEntities);
		//   exclusionSet.IsEntityExcluded(entityID); // O(1) instead of O(n)
		bool IsEntityExcluded(const std::vector<UUID>& excludedEntities, UUID entityID) noexcept
		{
			return std::find(excludedEntities.begin(), excludedEntities.end(), entityID) != excludedEntities.end();
		}

	} // namespace EntityExclusionUtils

} // namespace OloEngine
