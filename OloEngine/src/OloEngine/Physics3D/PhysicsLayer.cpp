#include "OloEnginePCH.h"
#include "PhysicsLayer.h"
#include "JoltUtils.h"
#include "Physics3DTypes.h"
#include "OloEngine/Core/Log.h"
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace OloEngine {

	void PhysicsLayerManager::RebuildLayerIndexMap()
	{
		std::unique_lock<std::shared_mutex> lock(s_LayersMutex);
		
		s_LayerIndexMap.clear();
		for (sizet i = 0; i < s_Layers.size(); ++i)
		{
			// Skip invalid layers to prevent map corruption from duplicate keys
			if (s_Layers[i].m_LayerID == INVALID_LAYER_ID)
				continue;
				
			s_LayerIndexMap[s_Layers[i].m_LayerID] = i;
		}
	}

	u32 PhysicsLayerManager::ToLayerMask(u32 layerId) noexcept
	{
		// Validate layer ID bounds - prevent undefined behavior from bit shift
		if (layerId >= JoltUtils::kMaxJoltLayers) // u32 can only have 32 bits
		{
			OLO_CORE_ERROR("PhysicsLayerManager::ToLayerMask: Layer ID {} exceeds maximum bit position (31)", layerId);
			return 0; // Return empty mask for invalid layer ID
		}
		
		return 1u << layerId;
	}

	u32 PhysicsLayerManager::AddLayer(const std::string& name, bool setCollisions)
	{
		std::unique_lock<std::shared_mutex> lock(s_LayersMutex);
		
		// Enforce Jolt Physics 32-layer limit - check before any allocation or mutation
		if (s_Layers.size() >= JoltUtils::kMaxJoltLayers)
		{
			OLO_CORE_ERROR("PhysicsLayerManager: Cannot add layer '{}' - maximum of {} layers already reached", name, JoltUtils::kMaxJoltLayers);
			return INVALID_LAYER_ID;
		}

		for (const auto& layer : s_Layers)
		{
			if (layer.m_Name == name)
				return layer.m_LayerID;
		}

		u32 layerId = GetNextLayerID();
		
		PhysicsLayer layer = { layerId, name, ToLayerMask(layerId), ToLayerMask(layerId) };
		
		// Determine the actual vector index where the layer will be placed
		sizet newLayerIndex;
		
		// Place the layer at the found free slot (could be a gap or at the end)
		if (layerId < s_Layers.size())
		{
			// Fill an existing gap - use layerId as the index
			s_Layers[layerId] = layer;
			newLayerIndex = layerId;
		}
		else
		{
			// Append to the end (no gaps found)
			s_Layers.push_back(layer);
			newLayerIndex = s_Layers.size() - 1;
		}
		
		s_LayerNames[layerId] = name;

		// Update index map with the actual vector index
		s_LayerIndexMap[layerId] = newLayerIndex;

		if (setCollisions)
		{
			// Inline collision bit update logic to avoid re-entrant locking
			for (sizet i = 0; i < s_Layers.size(); ++i)
			{
				// Skip invalid layers and the new layer itself
				if (s_Layers[i].m_LayerID == INVALID_LAYER_ID || s_Layers[i].m_LayerID == s_Layers[newLayerIndex].m_LayerID)
					continue;
					
				// Set bidirectional collision between the new layer and existing valid layers
				s_Layers[newLayerIndex].m_CollidesWith |= s_Layers[i].m_BitValue;
				s_Layers[i].m_CollidesWith |= s_Layers[newLayerIndex].m_BitValue;
			}
		}
		
		// Clear the self-collision bit (don't collide with self by default)
		s_Layers[newLayerIndex].m_CollidesWith &= ~s_Layers[newLayerIndex].m_BitValue;
		
		// Always sync the m_CollidesWithSelf member with the actual collision mask
		s_Layers[newLayerIndex].m_CollidesWithSelf = (s_Layers[newLayerIndex].m_CollidesWith & s_Layers[newLayerIndex].m_BitValue) != 0;

		return layer.m_LayerID;
	}

	void PhysicsLayerManager::RemoveLayer(u32 layerId)
	{
		std::unique_lock<std::shared_mutex> lock(s_LayersMutex);
		
		// Check if the layer exists before attempting removal
		auto layerIndexIt = s_LayerIndexMap.find(layerId);
		if (layerIndexIt == s_LayerIndexMap.end())
		{
			// Layer doesn't exist, nothing to remove
			return;
		}
		
		PhysicsLayer& layerInfo = GetLayerMutableUnsafe(layerId);

		for (auto& otherLayer : s_Layers)
		{
			if (otherLayer.m_LayerID == layerId)
				continue;

			if (otherLayer.m_CollidesWith & layerInfo.m_BitValue)
			{
				otherLayer.m_CollidesWith &= ~layerInfo.m_BitValue;
			}
		}

		// Remove from layer names
		s_LayerNames.erase(layerId);

		// Mark the layer as invalid (create a gap that can be reused)
		auto layerIt = std::find_if(s_Layers.begin(), s_Layers.end(), 
			[layerId](const PhysicsLayer& layer) { return layer.m_LayerID == layerId; });
		if (layerIt != s_Layers.end())
		{
			// Mark as invalid to create a reusable gap
			layerIt->m_LayerID = INVALID_LAYER_ID;
			layerIt->m_Name.clear();
			layerIt->m_BitValue = 0;
			layerIt->m_CollidesWith = 0;
			layerIt->m_CollidesWithSelf = false;
			
			// Update index map to remove the mapping
			s_LayerIndexMap.erase(layerId);
		}
	}

	void PhysicsLayerManager::UpdateLayerName(u32 layerId, const std::string& newName)
	{
		std::unique_lock<std::shared_mutex> lock(s_LayersMutex);
		
		// Early validation to prevent mutating s_NullLayer for invalid IDs
		auto indexIt = s_LayerIndexMap.find(layerId);
		if (indexIt == s_LayerIndexMap.end())
			return; // Invalid layer ID
		
		sizet index = indexIt->second;
		if (index >= s_Layers.size() || s_Layers[index].m_LayerID != layerId)
			return; // Invalid or corrupted layer mapping
		
		// Check if name already exists in a different layer
		for (const auto& pair : s_LayerNames)
		{
			if (pair.first != layerId && pair.second == newName)
				return;
		}

		PhysicsLayer& layer = GetLayerMutableUnsafe(layerId);
		
		// Update name in map directly by key
		s_LayerNames[layerId] = newName;
		layer.m_Name = newName;
	}

	void PhysicsLayerManager::SetLayerCollision(u32 layerId, u32 otherLayer, bool shouldCollide)
	{
		std::unique_lock<std::shared_mutex> lock(s_LayersMutex);
		
		PhysicsLayer& layerInfo = GetLayerMutableUnsafe(layerId);
		PhysicsLayer& otherLayerInfo = GetLayerMutableUnsafe(otherLayer);
		
		// Validate both layers before modification - directly check for invalid layer IDs
		if (layerInfo.m_LayerID == INVALID_LAYER_ID || otherLayerInfo.m_LayerID == INVALID_LAYER_ID)
		{
			OLO_CORE_WARN("PhysicsLayerManager::SetLayerCollision: Invalid layer ID(s) - layerId: {}, otherLayer: {}", layerId, otherLayer);
			return;
		}

		if (shouldCollide)
		{
			layerInfo.m_CollidesWith |= otherLayerInfo.m_BitValue;
			otherLayerInfo.m_CollidesWith |= layerInfo.m_BitValue;
		}
		else
		{
			layerInfo.m_CollidesWith &= ~otherLayerInfo.m_BitValue;
			otherLayerInfo.m_CollidesWith &= ~layerInfo.m_BitValue;
		}
	}

	void PhysicsLayerManager::SetLayerSelfCollision(u32 layerId, bool shouldCollide)
	{
		std::unique_lock<std::shared_mutex> lock(s_LayersMutex);
		
		PhysicsLayer& layerInfo = GetLayerMutableUnsafe(layerId);
		if (layerInfo.IsValid())
		{
			// Update the global collision bitmask
			if (shouldCollide)
			{
				layerInfo.m_CollidesWith |= layerInfo.m_BitValue;
			}
			else
			{
				layerInfo.m_CollidesWith &= ~layerInfo.m_BitValue;
			}
			
			// Update the flag to match the bitmask state
			layerInfo.m_CollidesWithSelf = shouldCollide;
		}
	}

	void PhysicsLayerManager::GetLayerCollisions(u32 layerId, std::vector<PhysicsLayer>& outLayers)
	{
		std::shared_lock<std::shared_mutex> lock(s_LayersMutex);
		
		const PhysicsLayer& layer = GetLayerUnsafe(layerId);

		outLayers.clear();
		for (const auto& otherLayer : s_Layers)
		{
			if (otherLayer.m_LayerID == INVALID_LAYER_ID)
				continue;
				
			if (otherLayer.m_LayerID == layerId)
				continue;

			if (layer.m_CollidesWith & otherLayer.m_BitValue)
				outLayers.push_back(otherLayer);
		}
	}

	PhysicsLayer PhysicsLayerManager::GetLayer(u32 layerId)
	{
		std::shared_lock<std::shared_mutex> lock(s_LayersMutex);
		return GetLayerImpl(layerId);
	}

	PhysicsLayer PhysicsLayerManager::GetLayer(const std::string& layerName)
	{
		std::shared_lock<std::shared_mutex> lock(s_LayersMutex);
		
		for (const auto& layer : s_Layers)
		{
			if (layer.m_Name == layerName)
				return layer;
		}

		return s_NullLayer;
	}

	PhysicsLayer PhysicsLayerManager::GetLayer(const std::string_view& layerName)
	{
		std::shared_lock<std::shared_mutex> lock(s_LayersMutex);
		
		for (const auto& layer : s_Layers)
		{
			if (layer.m_Name == layerName)
				return layer;
		}

		return s_NullLayer;
	}

std::vector<PhysicsLayer> PhysicsLayerManager::GetLayers()
{
	std::shared_lock<std::shared_mutex> lock(s_LayersMutex);
	return s_Layers; // Return a copy for thread safety
}

u32 PhysicsLayerManager::GetLayerCount()
{
	std::shared_lock<std::shared_mutex> lock(s_LayersMutex);
	
	// Count only valid layers (exclude those with INVALID_LAYER_ID)
	u32 validCount = 0;
	for (const auto& layer : s_Layers)
	{
		if (layer.m_LayerID != INVALID_LAYER_ID)
		{
			validCount++;
		}
	}
	return validCount;
}

std::vector<std::string> PhysicsLayerManager::GetLayerNames()
	{
		std::shared_lock<std::shared_mutex> lock(s_LayersMutex);
		
		// Return names in stable order based on s_Layers insertion order (deterministic)
		std::vector<std::string> names;
		names.reserve(s_Layers.size());
		for (const auto& layer : s_Layers)
		{
			// Skip invalid layers that have been removed
			if (layer.m_LayerID != INVALID_LAYER_ID)
			{
				names.push_back(layer.m_Name);
			}
		}
		return names;
	}

	bool PhysicsLayerManager::ShouldCollide(u32 layer1, u32 layer2) noexcept
	{
		std::shared_lock<std::shared_mutex> lock(s_LayersMutex);
		
		// Validate both layer IDs first
		const PhysicsLayer& layerA = GetLayerUnsafe(layer1);
		const PhysicsLayer& layerB = GetLayerUnsafe(layer2);
		
		// Return false if either layer is invalid (returns s_NullLayer)
		if (layerA.m_LayerID == s_NullLayer.m_LayerID || layerB.m_LayerID == s_NullLayer.m_LayerID)
			return false;
		
		// Handle same-layer collision queries
		if (layer1 == layer2)
		{
			return layerA.m_CollidesWithSelf;
		}
		
		// Perform bitmask collision test for different layers
		return (layerA.m_CollidesWith & layerB.m_BitValue) != 0;
	}

	bool PhysicsLayerManager::IsLayerValid(u32 layerId) noexcept
	{
		std::shared_lock<std::shared_mutex> lock(s_LayersMutex);
		
		const PhysicsLayer& layer = GetLayerUnsafe(layerId);
		return layer.IsValid();
	}

	bool PhysicsLayerManager::IsLayerValid(const std::string& layerName)
	{
		std::shared_lock<std::shared_mutex> lock(s_LayersMutex);
		
		const PhysicsLayer& layer = GetLayerUnsafe(layerName);
		return layer.IsValid();
	}

	bool PhysicsLayerManager::IsLayerValid(const std::string_view& layerName)
	{
		std::shared_lock<std::shared_mutex> lock(s_LayersMutex);
		
		// For string_view, we need to iterate directly since GetLayerUnsafe expects std::string
		for (const auto& layer : s_Layers)
		{
			if (layer.m_Name == layerName)
				return layer.IsValid();
		}
		return false;
	}

	void PhysicsLayerManager::ClearLayers()
	{
		std::unique_lock<std::shared_mutex> lock(s_LayersMutex);
		
		s_Layers.clear();
		s_LayerNames.clear();
		s_LayerIndexMap.clear();
	}

	u32 PhysicsLayerManager::GetNextLayerID()
	{
		// PRECONDITION: Caller must hold the s_LayersMutex lock
		// Scan for the first gap (INVALID_LAYER_ID) in the layer vector
		for (sizet i = 0; i < s_Layers.size(); ++i)
		{
			if (s_Layers[i].m_LayerID == INVALID_LAYER_ID)
			{
				// Check for overflow before casting to u32
				if (i > std::numeric_limits<u32>::max())
				{
					OLO_CORE_ERROR("PhysicsLayerManager::GetNextLayerID: Layer index {} exceeds u32 maximum", i);
					return INVALID_LAYER_ID;
				}
				return static_cast<u32>(i);
			}
		}

		// No gaps found, return the next index (size)
		sizet nextId = s_Layers.size();
		
		// Check for overflow before casting to u32
		if (nextId > std::numeric_limits<u32>::max())
		{
			OLO_CORE_ERROR("PhysicsLayerManager::GetNextLayerID: Next layer ID {} exceeds u32 maximum", nextId);
			return INVALID_LAYER_ID;
		}
		
		return static_cast<u32>(nextId);
	}

	// Internal unsafe methods - assume caller holds appropriate lock
	const PhysicsLayer& PhysicsLayerManager::GetLayerUnsafe(u32 layerId)
	{
		return GetLayerImpl(layerId);
	}

	// Internal mutable accessor for modification operations - use with caution
	PhysicsLayer& PhysicsLayerManager::GetLayerMutableUnsafe(u32 layerId)
	{
		// Perform single lookup in s_LayerIndexMap
		auto indexIt = s_LayerIndexMap.find(layerId);
		if (indexIt == s_LayerIndexMap.end())
		{
			// Invalid layer ID access - this is a programming error
			OLO_CORE_ERROR("PhysicsLayerManager::GetLayerMutableUnsafe: Invalid layer ID {} accessed", layerId);
			OLO_CORE_ASSERT(false, "Invalid layer ID accessed in GetLayerMutableUnsafe");
			
			// Throw exception for recoverable error handling instead of terminating
			throw std::out_of_range("PhysicsLayerManager: Invalid layer ID " + std::to_string(layerId) + " in GetLayerMutableUnsafe");
		}
		
		sizet index = indexIt->second;
		
		// Debug assert for bounds checking and consistency
		OLO_CORE_ASSERT(index < s_Layers.size() && s_Layers[index].m_LayerID == layerId, 
						"PhysicsLayerManager index map corruption: layerId {} maps to index {} "
						"but s_Layers[{}].m_LayerID is {}", 
						layerId, index, index, (index < s_Layers.size()) ? s_Layers[index].m_LayerID : INVALID_LAYER_ID);
		
		return s_Layers[index];
	}

	const PhysicsLayer& PhysicsLayerManager::GetLayerUnsafe(const std::string& layerName)
	{
		for (const auto& layer : s_Layers)
		{
			if (layer.m_Name == layerName)
				return layer;
		}

		return s_NullLayer;
	}

	// Internal helper for shared lookup logic - assumes caller holds appropriate lock
	const PhysicsLayer& PhysicsLayerManager::GetLayerImpl(u32 layerId)
	{
		// O(1) lookup using index map
		auto indexIt = s_LayerIndexMap.find(layerId);
		if (indexIt != s_LayerIndexMap.end())
		{
			sizet index = indexIt->second;
			// Bounds check for safety
			if (index < s_Layers.size())
			{
				if (s_Layers[index].m_LayerID == layerId)
				{
					return s_Layers[index];
				}
				else
				{
					// Index map corruption detected - log the inconsistency
					OLO_CORE_ERROR("PhysicsLayerManager: Index map corruption detected! "
								  "Queried layerId: {}, found index: {}, actual layerId at index: {}, "
								  "layers size: {}, index map size: {}",
								  layerId, index, s_Layers[index].m_LayerID, 
								  s_Layers.size(), s_LayerIndexMap.size());
					
					OLO_CORE_ASSERT(false, "PhysicsLayerManager index map corruption: layerId {} maps to index {} "
									       "but s_Layers[{}].m_LayerID is {}", 
									       layerId, index, index, s_Layers[index].m_LayerID);
				}
			}
		}

		return s_NullLayer;
	}

	std::vector<PhysicsLayer> PhysicsLayerManager::s_Layers;
	std::unordered_map<u32, std::string> PhysicsLayerManager::s_LayerNames;
	std::unordered_map<u32, sizet> PhysicsLayerManager::s_LayerIndexMap;
	const PhysicsLayer PhysicsLayerManager::s_NullLayer = { INVALID_LAYER_ID, "NULL", NO_COLLISION_BITS, NO_COLLISION_BITS };
	std::shared_mutex PhysicsLayerManager::s_LayersMutex;

}
