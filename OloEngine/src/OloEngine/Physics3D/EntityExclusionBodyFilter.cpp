#include "OloEnginePCH.h"
#include "EntityExclusionBodyFilter.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <algorithm>

namespace OloEngine {

	EntityExclusionBodyFilter::EntityExclusionBodyFilter(const std::vector<UUID>& excludedEntities)
		: m_ExcludedEntities(excludedEntities.begin(), excludedEntities.end())
	{
	}

	EntityExclusionBodyFilter::EntityExclusionBodyFilter(UUID excludedEntity)
	{
		m_ExcludedEntities.insert(excludedEntity);
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
		// Insert will automatically handle duplicates (won't insert if already present)
		m_ExcludedEntities.insert(entityID);
	}

	void EntityExclusionBodyFilter::RemoveExcludedEntity(UUID entityID)
	{
		m_ExcludedEntities.erase(entityID);
	}

	void EntityExclusionBodyFilter::ClearExcludedEntities()
	{
		m_ExcludedEntities.clear();
	}

	bool EntityExclusionBodyFilter::IsEntityExcluded(UUID entityID) const
	{
		return m_ExcludedEntities.find(entityID) != m_ExcludedEntities.end();
	}

	std::vector<UUID> EntityExclusionBodyFilter::GetExcludedEntities() const
	{
		return std::vector<UUID>(m_ExcludedEntities.begin(), m_ExcludedEntities.end());
	}

}