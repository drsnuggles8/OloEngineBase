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
		TranslationX = BIT(0), TranslationY = BIT(1), TranslationZ = BIT(2), Translation = TranslationX | TranslationY | TranslationZ,
		RotationX = BIT(3), RotationY = BIT(4), RotationZ = BIT(5), Rotation = RotationX | RotationY | RotationZ
	};

	enum class ECollisionDetectionType : u32
	{
		Discrete,
		Continuous
	};

	enum class EBodyType { Static, Dynamic, Kinematic };

	enum class EFalloffMode { Constant, Linear };

	enum class ShapeType { Box, Sphere, Capsule, ConvexMesh, TriangleMesh, CompoundShape, MutableCompoundShape, LAST };

	namespace ShapeUtils {
		constexpr sizet MaxShapeTypes = (sizet)ShapeType::LAST;

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

	// Bitwise operators for EActorAxis
	inline EActorAxis operator|(EActorAxis a, EActorAxis b)
	{
		return static_cast<EActorAxis>(static_cast<u32>(a) | static_cast<u32>(b));
	}

	inline EActorAxis operator&(EActorAxis a, EActorAxis b)
	{
		return static_cast<EActorAxis>(static_cast<u32>(a) & static_cast<u32>(b));
	}

	inline EActorAxis operator^(EActorAxis a, EActorAxis b)
	{
		return static_cast<EActorAxis>(static_cast<u32>(a) ^ static_cast<u32>(b));
	}

	inline EActorAxis operator~(EActorAxis a)
	{
		return static_cast<EActorAxis>(~static_cast<u32>(a));
	}

	inline EActorAxis& operator|=(EActorAxis& a, EActorAxis b)
	{
		return a = a | b;
	}

	inline EActorAxis& operator&=(EActorAxis& a, EActorAxis b)
	{
		return a = a & b;
	}

	inline EActorAxis& operator^=(EActorAxis& a, EActorAxis b)
	{
		return a = a ^ b;
	}

}