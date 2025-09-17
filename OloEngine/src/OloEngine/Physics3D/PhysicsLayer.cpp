#include "OloEnginePCH.h"
#include "PhysicsLayer.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine {

	u32 PhysicsLayerManager::AddLayer(const std::string& name, bool setCollisions)
	{
		for (const auto& layer : s_Layers)
		{
			if (layer.Name == name)
				return layer.LayerID;
		}

		u32 layerId = GetNextLayerID();
		PhysicsLayer layer = { layerId, name, static_cast<i32>(BIT(layerId)), static_cast<i32>(BIT(layerId)) };
		s_Layers.insert(s_Layers.begin() + layerId, layer);
		s_LayerNames.insert(s_LayerNames.begin() + layerId, name);

		if (setCollisions)
		{
			for (const auto& layer2 : s_Layers)
			{
				SetLayerCollision(layer.LayerID, layer2.LayerID, true);
			}
		}

		return layer.LayerID;
	}

	void PhysicsLayerManager::RemoveLayer(u32 layerId)
	{
		PhysicsLayer& layerInfo = GetLayer(layerId);

		for (auto& otherLayer : s_Layers)
		{
			if (otherLayer.LayerID == layerId)
				continue;

			if (otherLayer.CollidesWith & layerInfo.BitValue)
			{
				otherLayer.CollidesWith &= ~layerInfo.BitValue;
			}
		}

		// Remove from layer names
		auto nameIt = std::find(s_LayerNames.begin(), s_LayerNames.end(), layerInfo.Name);
		if (nameIt != s_LayerNames.end())
			s_LayerNames.erase(nameIt);

		// Remove from layers
		auto layerIt = std::find_if(s_Layers.begin(), s_Layers.end(), 
			[layerId](const PhysicsLayer& layer) { return layer.LayerID == layerId; });
		if (layerIt != s_Layers.end())
			s_Layers.erase(layerIt);
	}

	void PhysicsLayerManager::UpdateLayerName(u32 layerId, const std::string& newName)
	{
		for (const auto& layerName : s_LayerNames)
		{
			if (layerName == newName)
				return;
		}

		PhysicsLayer& layer = GetLayer(layerId);
		
		// Remove old name
		auto nameIt = std::find(s_LayerNames.begin(), s_LayerNames.end(), layer.Name);
		if (nameIt != s_LayerNames.end())
			s_LayerNames.erase(nameIt);
		
		// Insert new name at correct position
		s_LayerNames.insert(s_LayerNames.begin() + layerId, newName);
		layer.Name = newName;
	}

	void PhysicsLayerManager::SetLayerCollision(u32 layerId, u32 otherLayer, bool shouldCollide)
	{
		if (ShouldCollide(layerId, otherLayer) && shouldCollide)
			return;

		PhysicsLayer& layerInfo = GetLayer(layerId);
		PhysicsLayer& otherLayerInfo = GetLayer(otherLayer);

		if (shouldCollide)
		{
			layerInfo.CollidesWith |= otherLayerInfo.BitValue;
			otherLayerInfo.CollidesWith |= layerInfo.BitValue;
		}
		else
		{
			layerInfo.CollidesWith &= ~otherLayerInfo.BitValue;
			otherLayerInfo.CollidesWith &= ~layerInfo.BitValue;
		}
	}

	std::vector<PhysicsLayer> PhysicsLayerManager::GetLayerCollisions(u32 layerId)
	{
		const PhysicsLayer& layer = GetLayer(layerId);

		std::vector<PhysicsLayer> layers;
		for (const auto& otherLayer : s_Layers)
		{
			if (otherLayer.LayerID == layerId)
				continue;

			if (layer.CollidesWith & otherLayer.BitValue)
				layers.push_back(otherLayer);
		}

		return layers;
	}

	PhysicsLayer& PhysicsLayerManager::GetLayer(u32 layerId)
	{
		return layerId >= s_Layers.size() ? s_NullLayer : s_Layers[layerId];
	}

	PhysicsLayer& PhysicsLayerManager::GetLayer(const std::string& layerName)
	{
		for (auto& layer : s_Layers)
		{
			if (layer.Name == layerName)
				return layer;
		}

		return s_NullLayer;
	}

	bool PhysicsLayerManager::ShouldCollide(u32 layer1, u32 layer2)
	{
		return GetLayer(layer1).CollidesWith & GetLayer(layer2).BitValue;
	}

	bool PhysicsLayerManager::IsLayerValid(u32 layerId)
	{
		const PhysicsLayer& layer = GetLayer(layerId);
		return layer.LayerID != s_NullLayer.LayerID && layer.IsValid();
	}

	bool PhysicsLayerManager::IsLayerValid(const std::string& layerName)
	{
		const PhysicsLayer& layer = GetLayer(layerName);
		return layer.LayerID != s_NullLayer.LayerID && layer.IsValid();
	}

	void PhysicsLayerManager::ClearLayers()
	{
		s_Layers.clear();
		s_LayerNames.clear();
	}

	u32 PhysicsLayerManager::GetNextLayerID()
	{
		i32 lastId = -1;

		for (const auto& layer : s_Layers)
		{
			if (lastId != -1 && static_cast<i32>(layer.LayerID) != lastId + 1)
				return static_cast<u32>(lastId + 1);

			lastId = static_cast<i32>(layer.LayerID);
		}

		return static_cast<u32>(s_Layers.size());
	}

	std::vector<PhysicsLayer> PhysicsLayerManager::s_Layers;
	std::vector<std::string> PhysicsLayerManager::s_LayerNames;
	PhysicsLayer PhysicsLayerManager::s_NullLayer = { static_cast<u32>(-1), "NULL", -1, -1 };

}