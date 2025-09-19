#include "OloEnginePCH.h"
#include "PhysicsLayer.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine {

	void PhysicsLayerManager::RebuildLayerIndexMap()
	{
		s_LayerIndexMap.clear();
		for (sizet i = 0; i < s_Layers.size(); ++i)
		{
			s_LayerIndexMap[s_Layers[i].m_LayerID] = i;
		}
	}

	u32 PhysicsLayerManager::AddLayer(const std::string& name, bool setCollisions)
	{
		// Enforce Jolt Physics 32-layer limit - check before any allocation or mutation
		if (s_Layers.size() >= MAX_PHYSICS_LAYERS)
		{
			OLO_CORE_ERROR("PhysicsLayerManager: Cannot add layer '{}' - maximum of {} layers already reached", name, MAX_PHYSICS_LAYERS);
			return INVALID_LAYER_ID;
		}

		for (const auto& layer : s_Layers)
		{
			if (layer.m_Name == name)
				return layer.m_LayerID;
		}

		u32 layerId = GetNextLayerID();
		PhysicsLayer layer = { layerId, name, static_cast<i32>(BIT(layerId)), static_cast<i32>(BIT(layerId)) };
		s_Layers.insert(s_Layers.begin() + layerId, layer);
		s_LayerNames[layerId] = name;

		// Rebuild index map after modifying s_Layers
		RebuildLayerIndexMap();

		if (setCollisions)
		{
			for (const auto& layer2 : s_Layers)
			{
				SetLayerCollision(layer.m_LayerID, layer2.m_LayerID, true);
			}
		}

		return layer.m_LayerID;
	}

	void PhysicsLayerManager::RemoveLayer(u32 layerId)
	{
		PhysicsLayer& layerInfo = GetLayer(layerId);

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

		// Remove from layers
		auto layerIt = std::find_if(s_Layers.begin(), s_Layers.end(), 
			[layerId](const PhysicsLayer& layer) { return layer.m_LayerID == layerId; });
		if (layerIt != s_Layers.end())
		{
			s_Layers.erase(layerIt);
			// Rebuild index map after modifying s_Layers
			RebuildLayerIndexMap();
		}
	}

	void PhysicsLayerManager::UpdateLayerName(u32 layerId, const std::string& newName)
	{
		// Check if name already exists
		for (const auto& pair : s_LayerNames)
		{
			if (pair.second == newName)
				return;
		}

		PhysicsLayer& layer = GetLayer(layerId);
		
		// Update name in map directly by key
		s_LayerNames[layerId] = newName;
		layer.m_Name = newName;
	}

	void PhysicsLayerManager::SetLayerCollision(u32 layerId, u32 otherLayer, bool shouldCollide)
	{
		PhysicsLayer& layerInfo = GetLayer(layerId);
		PhysicsLayer& otherLayerInfo = GetLayer(otherLayer);

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

	void PhysicsLayerManager::GetLayerCollisions(u32 layerId, std::vector<PhysicsLayer>& outLayers)
	{
		const PhysicsLayer& layer = GetLayer(layerId);

		outLayers.clear();
		for (const auto& otherLayer : s_Layers)
		{
			if (otherLayer.m_LayerID == layerId)
				continue;

			if (layer.m_CollidesWith & otherLayer.m_BitValue)
				outLayers.push_back(otherLayer);
		}
	}

	PhysicsLayer& PhysicsLayerManager::GetLayer(u32 layerId)
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

	PhysicsLayer& PhysicsLayerManager::GetLayer(const std::string& layerName)
	{
		for (auto& layer : s_Layers)
		{
			if (layer.m_Name == layerName)
				return layer;
		}

		return s_NullLayer;
	}

	std::vector<std::string> PhysicsLayerManager::GetLayerNames()
	{
		std::vector<std::string> names;
		names.reserve(s_LayerNames.size());
		for (const auto& pair : s_LayerNames)
		{
			names.push_back(pair.second);
		}
		return names;
	}

	bool PhysicsLayerManager::ShouldCollide(u32 layer1, u32 layer2)
	{
		return GetLayer(layer1).m_CollidesWith & GetLayer(layer2).m_BitValue;
	}

	bool PhysicsLayerManager::IsLayerValid(u32 layerId)
	{
		const PhysicsLayer& layer = GetLayer(layerId);
		return layer.m_LayerID != s_NullLayer.m_LayerID && layer.IsValid();
	}

	bool PhysicsLayerManager::IsLayerValid(const std::string& layerName)
	{
		const PhysicsLayer& layer = GetLayer(layerName);
		return layer.m_LayerID != s_NullLayer.m_LayerID && layer.IsValid();
	}

	void PhysicsLayerManager::ClearLayers()
	{
		s_Layers.clear();
		s_LayerNames.clear();
	}

	u32 PhysicsLayerManager::GetNextLayerID()
	{
		i32 lastId = NO_PREVIOUS_LAYER_ID;

		for (const auto& layer : s_Layers)
		{
			if (lastId != NO_PREVIOUS_LAYER_ID && static_cast<i32>(layer.m_LayerID) != lastId + 1)
				return static_cast<u32>(lastId + 1);

			lastId = static_cast<i32>(layer.m_LayerID);
		}

		return static_cast<u32>(s_Layers.size());
	}

	std::vector<PhysicsLayer> PhysicsLayerManager::s_Layers;
	std::unordered_map<u32, std::string> PhysicsLayerManager::s_LayerNames;
	std::unordered_map<u32, sizet> PhysicsLayerManager::s_LayerIndexMap;
	PhysicsLayer PhysicsLayerManager::s_NullLayer = { INVALID_LAYER_ID, "NULL", NO_COLLISION_BITS, NO_COLLISION_BITS };

}