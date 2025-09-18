#include "OloEnginePCH.h"
#include "EntityExclusionBodyFilter.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>

namespace OloEngine {

	EntityExclusionBodyFilter::EntityExclusionBodyFilter(const std::vector<UUID>& excludedEntities)
		: m_ExcludedEntities(excludedEntities)
	{
	}

	EntityExclusionBodyFilter::EntityExclusionBodyFilter(UUID excludedEntity)
	{
		m_ExcludedEntities.push_back(excludedEntity);
	}

	bool EntityExclusionBodyFilter::ShouldCollide(const JPH::BodyID& inBodyID) const
	{
		// Always allow initial filter check at the broad phase level
		// The actual filtering happens in ShouldCollideLocked when we have access to the body data
		(void)inBodyID; // Suppress unused parameter warning
		return true;
	}

	bool EntityExclusionBodyFilter::ShouldCollideLocked(const JPH::Body& inBody) const
	{
		// Extract entity UUID from body user data
		UUID entityID = static_cast<UUID>(inBody.GetUserData());
		
		// Return false if this entity should be excluded (i.e., don't collide with excluded entities)
		return !IsEntityExcluded(entityID);
	}

	void EntityExclusionBodyFilter::AddExcludedEntity(UUID entityID)
	{
		// Only add if not already present
		if (!IsEntityExcluded(entityID))
		{
			m_ExcludedEntities.push_back(entityID);
		}
	}

	void EntityExclusionBodyFilter::RemoveExcludedEntity(UUID entityID)
	{
		auto it = std::find(m_ExcludedEntities.begin(), m_ExcludedEntities.end(), entityID);
		if (it != m_ExcludedEntities.end())
		{
			m_ExcludedEntities.erase(it);
		}
	}

	void EntityExclusionBodyFilter::ClearExcludedEntities()
	{
		m_ExcludedEntities.clear();
	}

	bool EntityExclusionBodyFilter::IsEntityExcluded(UUID entityID) const
	{
		return std::find(m_ExcludedEntities.begin(), m_ExcludedEntities.end(), entityID) != m_ExcludedEntities.end();
	}

}