#include "EntityExclusionUtils.h"
#include <algorithm>

namespace OloEngine {

	ExcludedEntitySet::ExcludedEntitySet(const std::vector<UUID>& excludedEntities)
		: m_ExcludedEntities(excludedEntities.begin(), excludedEntities.end())
	{
	}

	ExcludedEntitySet::ExcludedEntitySet(UUID excludedEntity)
	{
		m_ExcludedEntities.insert(excludedEntity);
	}

	bool ExcludedEntitySet::IsEntityExcluded(UUID entityID) const
	{
		return m_ExcludedEntities.find(entityID) != m_ExcludedEntities.end();
	}

	void ExcludedEntitySet::AddExcludedEntity(UUID entityID)
	{
		m_ExcludedEntities.insert(entityID);
	}

	void ExcludedEntitySet::RemoveExcludedEntity(UUID entityID)
	{
		m_ExcludedEntities.erase(entityID);
	}

	void ExcludedEntitySet::Clear()
	{
		m_ExcludedEntities.clear();
	}

	bool ExcludedEntitySet::Empty() const
	{
		return m_ExcludedEntities.empty();
	}

	sizet ExcludedEntitySet::Size() const
	{
		return m_ExcludedEntities.size();
	}

	std::vector<UUID> ExcludedEntitySet::ToVector() const
	{
		return std::vector<UUID>(m_ExcludedEntities.begin(), m_ExcludedEntities.end());
	}

	void ExcludedEntitySet::UpdateFromVector(const std::vector<UUID>& excludedEntities)
	{
		m_ExcludedEntities.clear();
		m_ExcludedEntities.insert(excludedEntities.begin(), excludedEntities.end());
	}

	namespace EntityExclusionUtils {

		bool IsEntityExcluded(const std::vector<UUID>& excludedEntities, UUID entityID)
		{
			return std::find(excludedEntities.begin(), excludedEntities.end(), entityID) != excludedEntities.end();
		}

	} // namespace EntityExclusionUtils

} // namespace OloEngine