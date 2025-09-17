#pragma once

#include "OloEngine/Core/Base.h"
#include <string>
#include <vector>

namespace OloEngine {

	struct PhysicsLayer
	{
		u32 LayerID;
		std::string Name;
		i32 BitValue;
		i32 CollidesWith = 0;
		bool CollidesWithSelf = true;

		bool IsValid() const
		{
			return !Name.empty() && BitValue > 0;
		}
	};

	class PhysicsLayerManager
	{
	public:
		static u32 AddLayer(const std::string& name, bool setCollisions = true);
		static void RemoveLayer(u32 layerId);

		static void UpdateLayerName(u32 layerId, const std::string& newName);

		static void SetLayerCollision(u32 layerId, u32 otherLayer, bool shouldCollide);
		static std::vector<PhysicsLayer> GetLayerCollisions(u32 layerId);

		static const std::vector<PhysicsLayer>& GetLayers() { return s_Layers; }
		static const std::vector<std::string>& GetLayerNames() { return s_LayerNames; }

		static PhysicsLayer& GetLayer(u32 layerId);
		static PhysicsLayer& GetLayer(const std::string& layerName);
		static u32 GetLayerCount() { return static_cast<u32>(s_Layers.size()); }

		static bool ShouldCollide(u32 layer1, u32 layer2);
		static bool IsLayerValid(u32 layerId);
		static bool IsLayerValid(const std::string& layerName);

		static void ClearLayers();

	private:
		static u32 GetNextLayerID();

	private:
		static std::vector<PhysicsLayer> s_Layers;
		static std::vector<std::string> s_LayerNames;
		static PhysicsLayer s_NullLayer;
	};

}