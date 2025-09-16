#pragma once

#include "OloEngine/Core/Base.h"
#include <glm/glm.hpp>

namespace OloEngine {

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

	struct ColliderMaterial
	{
		f32 StaticFriction = 0.6f;
		f32 DynamicFriction = 0.6f;
		f32 Restitution = 0.0f;
		f32 Density = 1000.0f; // kg/m^3

		ColliderMaterial() = default;
		ColliderMaterial(f32 staticFriction, f32 dynamicFriction, f32 restitution, f32 density = 1000.0f)
			: StaticFriction(staticFriction), DynamicFriction(dynamicFriction), Restitution(restitution), Density(density) {}
	};

	// Contact event types
	enum class ContactType
	{
		None = 0,
		ContactAdded,
		ContactPersisted,
		ContactRemoved
	};

	// Scene query structures
	struct RayCastInfo
	{
		glm::vec3 Origin;
		glm::vec3 Direction;
		f32 MaxDistance = 500.0f;
		u32 LayerMask = 0xFFFFFFFF;
	};

	struct SceneQueryHit
	{
		bool HasHit = false;
		UUID EntityID = 0;
		glm::vec3 Position = glm::vec3(0.0f);
		glm::vec3 Normal = glm::vec3(0.0f);
		f32 Distance = 0.0f;
	};

	struct ShapeCastInfo
	{
		glm::vec3 Origin;
		glm::vec3 Direction;
		f32 MaxDistance = 500.0f;
		u32 LayerMask = 0xFFFFFFFF;
		ShapeType Shape;
		glm::vec3 HalfExtents; // For box shape
		f32 Radius; // For sphere/capsule
		f32 HalfHeight; // For capsule
	};

	struct ShapeOverlapInfo
	{
		glm::vec3 Origin;
		u32 LayerMask = 0xFFFFFFFF;
		ShapeType Shape;
		glm::vec3 HalfExtents; // For box shape
		f32 Radius; // For sphere/capsule
		f32 HalfHeight; // For capsule
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

}