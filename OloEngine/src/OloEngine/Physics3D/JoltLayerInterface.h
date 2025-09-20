#pragma once

#include "Physics3DTypes.h"
#include "PhysicsLayer.h"
#include "OloEngine/Core/Base.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>

namespace OloEngine {

	namespace BroadPhaseLayers
	{
		static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
		static constexpr JPH::BroadPhaseLayer MOVING(1);
		static constexpr JPH::uint NUM_LAYERS = 2;
	}

	namespace ObjectLayers
	{
		static constexpr JPH::ObjectLayer NON_MOVING = 0;
		static constexpr JPH::ObjectLayer MOVING = 1;
		static constexpr JPH::ObjectLayer TRIGGER = 2;
		static constexpr JPH::ObjectLayer CHARACTER = 3;
		static constexpr JPH::ObjectLayer DEBRIS = 4;
		static constexpr u32 NUM_LAYERS = 5;
	}

	/// Class that determines if two object layers can collide
	class ObjectLayerPairFilter : public JPH::ObjectLayerPairFilter
	{
	public:
		virtual bool ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const override
		{
			// First check basic layer compatibility (static objects, triggers, etc.)
			bool basicCheck = ShouldCollideBasic(inObject1, inObject2);
			if (!basicCheck)
				return false;

			// If both layers are user-defined physics layers, check the physics layer manager
			if (inObject1 >= ObjectLayers::NUM_LAYERS && inObject2 >= ObjectLayers::NUM_LAYERS)
			{
				u32 layer1 = static_cast<u32>(inObject1) - ObjectLayers::NUM_LAYERS;
				u32 layer2 = static_cast<u32>(inObject2) - ObjectLayers::NUM_LAYERS;
				return PhysicsLayerManager::ShouldCollide(layer1, layer2);
			}

			return basicCheck;
		}

	private:
		bool ShouldCollideBasic(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const
		{
			switch (inObject1)
			{
				case ObjectLayers::NON_MOVING:
					return inObject2 == ObjectLayers::MOVING || inObject2 == ObjectLayers::CHARACTER; // Non moving only collides with moving and characters
				case ObjectLayers::MOVING:
					return true; // Moving collides with everything
				case ObjectLayers::TRIGGER:
					return inObject2 == ObjectLayers::MOVING || inObject2 == ObjectLayers::CHARACTER; // Triggers only collide with moving objects and characters
				case ObjectLayers::CHARACTER:
					return inObject2 == ObjectLayers::NON_MOVING || inObject2 == ObjectLayers::MOVING || inObject2 == ObjectLayers::TRIGGER; // Characters collide with static, moving, and triggers
				case ObjectLayers::DEBRIS:
					return inObject2 == ObjectLayers::NON_MOVING || inObject2 == ObjectLayers::MOVING; // Debris collides with static and moving
				default:
					// For user-defined layers, return true and let the physics layer manager handle it
					return true;
			}
		}
	};

	/// Each broadphase layer results in a separate bounding volume tree in the broad phase. You typically put non-moving objects in a separate tree
	/// BroadPhaseLayerInterface implementation
	class BroadPhaseLayerInterface : public JPH::BroadPhaseLayerInterface
	{
	public:
		BroadPhaseLayerInterface()
		{
			// Create a mapping table from object layer to broad phase layer
			m_ObjectToBroadPhase[ObjectLayers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
			m_ObjectToBroadPhase[ObjectLayers::MOVING] = BroadPhaseLayers::MOVING;
			m_ObjectToBroadPhase[ObjectLayers::TRIGGER] = BroadPhaseLayers::MOVING; // Triggers are moving
			m_ObjectToBroadPhase[ObjectLayers::CHARACTER] = BroadPhaseLayers::MOVING; // Characters are moving
			m_ObjectToBroadPhase[ObjectLayers::DEBRIS] = BroadPhaseLayers::MOVING; // Debris is moving
		}

		virtual u32 GetNumBroadPhaseLayers() const override
		{
			return BroadPhaseLayers::NUM_LAYERS;
		}

		virtual JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override
		{
			OLO_CORE_ASSERT(inLayer < ObjectLayers::NUM_LAYERS, "Invalid object layer");
			return m_ObjectToBroadPhase[inLayer];
		}

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
		virtual const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override
		{
			switch ((JPH::BroadPhaseLayer::Type)inLayer)
			{
				case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::NON_MOVING:	return "NON_MOVING";
				case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::MOVING:		return "MOVING";
				default:													OLO_CORE_ASSERT(false, "Invalid broad phase layer"); return "INVALID";
			}
		}
#endif // JPH_EXTERNAL_PROFILE || JPH_PROFILE_ENABLED

	private:
		JPH::BroadPhaseLayer m_ObjectToBroadPhase[ObjectLayers::NUM_LAYERS];
	};

	/// Class that determines if an object layer can collide with a broadphase layer
	class ObjectVsBroadPhaseLayerFilter : public JPH::ObjectVsBroadPhaseLayerFilter
	{
	public:
		virtual bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override
		{
			switch (inLayer1)
			{
				case ObjectLayers::NON_MOVING:
					return inLayer2 == BroadPhaseLayers::MOVING;
				case ObjectLayers::MOVING:
					return true;
				case ObjectLayers::TRIGGER:
					return inLayer2 == BroadPhaseLayers::MOVING;
				case ObjectLayers::CHARACTER:
					return true;
				case ObjectLayers::DEBRIS:
					return true;
				default:
					OLO_CORE_ASSERT(false, "Unknown object layer");
					return false;
			}
		}
	};

	// Utility functions to map from engine types to Jolt layers
	class JoltLayerInterface
	{
	public:
		static JPH::ObjectLayer GetObjectLayer(EBodyType bodyType, bool isTrigger = false)
		{
			if (isTrigger)
				return ObjectLayers::TRIGGER;

			switch (bodyType)
			{
				case EBodyType::Static:
					return ObjectLayers::NON_MOVING;
				case EBodyType::Dynamic:
				case EBodyType::Kinematic:
					return ObjectLayers::MOVING;
				default:
					OLO_CORE_ASSERT(false, "Unknown body type");
					return ObjectLayers::NON_MOVING;
			}
		}

		static JPH::ObjectLayer GetObjectLayerForCollider(u32 layerID, EBodyType bodyType, bool isTrigger = false)
		{
			// If a valid physics layer is specified, use it (offset by the number of built-in layers)
			if (PhysicsLayerManager::IsLayerValid(layerID))
			{
				return JPH::ObjectLayer(ObjectLayers::NUM_LAYERS + layerID);
			}

			// Fall back to the basic layer mapping for built-in layers
			return GetObjectLayer(bodyType, isTrigger);
		}

		static const ObjectLayerPairFilter& GetObjectLayerPairFilter()
		{
			static ObjectLayerPairFilter s_ObjectLayerPairFilter;
			return s_ObjectLayerPairFilter;
		}

		static const BroadPhaseLayerInterface& GetBroadPhaseLayerInterface()
		{
			static BroadPhaseLayerInterface s_BroadPhaseLayerInterface;
			return s_BroadPhaseLayerInterface;
		}

		static const ObjectVsBroadPhaseLayerFilter& GetObjectVsBroadPhaseLayerFilter()
		{
			static ObjectVsBroadPhaseLayerFilter s_ObjectVsBroadPhaseLayerFilter;
			return s_ObjectVsBroadPhaseLayerFilter;
		}
	};

}