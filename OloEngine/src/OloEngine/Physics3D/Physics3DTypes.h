#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Physics3D/ColliderMaterial.h"
#include <glm/glm.hpp>
#include <functional>
#include <vector>

namespace OloEngine {

	// Forward declarations
	class Entity;

	// Type aliases for scene queries
	using ExcludedEntityMap = std::vector<UUID>;

	// Character controller contact callback function type
	using ContactCallbackFn = std::function<void(Entity entity, Entity otherEntity)>;

	enum class EForceMode : u8
	{
		Force = 0,
		Impulse,
		VelocityChange,
		Acceleration
	};

	enum class EActorAxis : u32
	{
		None = 0,
		TranslationX = OLO_BIT(0), TranslationY = OLO_BIT(1), TranslationZ = OLO_BIT(2), Translation = TranslationX | TranslationY | TranslationZ,
		RotationX = OLO_BIT(3), RotationY = OLO_BIT(4), RotationZ = OLO_BIT(5), Rotation = RotationX | RotationY | RotationZ
	};

	// Mask covering all defined axis bits (OLO_BIT(0) through OLO_BIT(5))
	constexpr u32 AxisMask = OLO_BIT(0) | OLO_BIT(1) | OLO_BIT(2) | OLO_BIT(3) | OLO_BIT(4) | OLO_BIT(5);

	enum class ECollisionDetectionType : u32
	{
		Discrete,
		Continuous
	};

	enum class EBodyType { Static, Dynamic, Kinematic };

	enum class EFalloffMode { Constant, Linear };

	enum class ShapeType { Box, Sphere, Capsule, ConvexMesh, TriangleMesh, CompoundShape, MutableCompoundShape, LAST }; // sentinel: not a valid ShapeType — MUST remain last

	namespace ShapeUtils {
		constexpr sizet MaxShapeTypes = (sizet)ShapeType::LAST; // depends on LAST sentinel value — do not add entries after LAST

		inline const char* ShapeTypeToString(ShapeType type)
		{
			switch (type)
			{
				case ShapeType::CompoundShape: return "CompoundShape";
				case ShapeType::MutableCompoundShape: return "MutableCompoundShape";
				case ShapeType::Box: return "Box";
				case ShapeType::Sphere: return "Sphere";
				case ShapeType::Capsule: return "Capsule";
				case ShapeType::ConvexMesh: return "ConvexMesh";
				case ShapeType::TriangleMesh: return "TriangleMesh";
			}

			OLO_CORE_ASSERT(false, "Unknown ShapeType");
			return "";
		}
	}

	// Contact event types
	enum class ContactType
	{
		None = 0,
		ContactAdded,
		ContactPersisted,
		ContactRemoved
	};

	// Collision filtering
	namespace CollisionLayers
	{
		constexpr u32 Default = 0;
		constexpr u32 Static = 1;
		constexpr u32 Dynamic = 2;
		constexpr u32 Kinematic = 3;
		constexpr u32 Trigger = 4;
		constexpr u32 Character = 5;
		constexpr u32 Water = 6;
		constexpr u32 Debris = 7;
	}

	/**
	 * @brief Converts a layer index to a bitmask for physics layer operations.
	 * @param layerIndex The physics layer index (valid range: 0-31)
	 * @return Bitmask with the corresponding bit set, or 0 if layerIndex >= 32
	 * @note For indices >= 32, returns 0 to prevent undefined behavior from bit shifting
	 */
	constexpr inline u32 ToLayerMask(u32 layerIndex)
	{
		return (layerIndex >= 32) ? 0 : OLO_BIT(layerIndex);
	}

	// Bitwise operators for EActorAxis
	constexpr inline EActorAxis operator|(EActorAxis a, EActorAxis b)
	{
		return static_cast<EActorAxis>(static_cast<u32>(a) | static_cast<u32>(b));
	}

	constexpr inline EActorAxis operator&(EActorAxis a, EActorAxis b)
	{
		return static_cast<EActorAxis>(static_cast<u32>(a) & static_cast<u32>(b));
	}

	constexpr inline EActorAxis operator^(EActorAxis a, EActorAxis b)
	{
		return static_cast<EActorAxis>(static_cast<u32>(a) ^ static_cast<u32>(b));
	}

	constexpr inline EActorAxis operator~(EActorAxis a)
	{
		return static_cast<EActorAxis>(~static_cast<u32>(a) & AxisMask);
	}

	constexpr inline EActorAxis& operator|=(EActorAxis& a, EActorAxis b)
	{
		return a = a | b;
	}

	constexpr inline EActorAxis& operator&=(EActorAxis& a, EActorAxis b)
	{
		return a = a & b;
	}

	constexpr inline EActorAxis& operator^=(EActorAxis& a, EActorAxis b)
	{
		return a = a ^ b;
	}

}