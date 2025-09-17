#include "OloEnginePCH.h"
#include "JoltShapes.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Scene/Components.h"

#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/MutableCompoundShape.h>

namespace OloEngine {

	bool JoltShapes::s_Initialized = false;
	std::unordered_map<std::string, JPH::Ref<JPH::Shape>> JoltShapes::s_ShapeCache;

	void JoltShapes::Initialize()
	{
		if (s_Initialized)
			return;

		OLO_CORE_INFO("Initializing JoltShapes system");
		s_ShapeCache.clear();
		s_Initialized = true;
	}

	void JoltShapes::Shutdown()
	{
		if (!s_Initialized)
			return;

		OLO_CORE_INFO("Shutting down JoltShapes system");
		ClearShapeCache();
		s_Initialized = false;
	}

	JPH::Ref<JPH::Shape> JoltShapes::CreateBoxShape(const BoxCollider3DComponent& component, const glm::vec3& scale)
	{
		glm::vec3 scaledHalfExtents = ApplyScaleToBoxExtents(component.HalfExtents, scale);
		
		if (!ValidateBoxDimensions(scaledHalfExtents))
		{
			OLO_CORE_ERROR("Invalid box dimensions: {0}, {1}, {2}", scaledHalfExtents.x, scaledHalfExtents.y, scaledHalfExtents.z);
			return nullptr;
		}

		return CreateBoxShapeInternal(scaledHalfExtents);
	}

	JPH::Ref<JPH::Shape> JoltShapes::CreateSphereShape(const SphereCollider3DComponent& component, const glm::vec3& scale)
	{
		f32 scaledRadius = ApplyScaleToSphereRadius(component.Radius, scale);
		
		if (!ValidateSphereDimensions(scaledRadius))
		{
			OLO_CORE_ERROR("Invalid sphere radius: {0}", scaledRadius);
			return nullptr;
		}

		return CreateSphereShapeInternal(scaledRadius);
	}

	JPH::Ref<JPH::Shape> JoltShapes::CreateCapsuleShape(const CapsuleCollider3DComponent& component, const glm::vec3& scale)
	{
		f32 scaledRadius = component.Radius;
		f32 scaledHalfHeight = component.HalfHeight;
		ApplyScaleToCapsule(scaledRadius, scaledHalfHeight, scale);
		
		if (!ValidateCapsuleDimensions(scaledRadius, scaledHalfHeight))
		{
			OLO_CORE_ERROR("Invalid capsule dimensions: radius={0}, halfHeight={1}", scaledRadius, scaledHalfHeight);
			return nullptr;
		}

		return CreateCapsuleShapeInternal(scaledRadius, scaledHalfHeight);
	}

	JPH::Ref<JPH::Shape> JoltShapes::CreateCompoundShape(Entity entity, bool isMutable)
	{
		if (!entity)
		{
			OLO_CORE_ERROR("Cannot create compound shape for invalid entity");
			return nullptr;
		}

		std::vector<JPH::Ref<JPH::Shape>> shapes;
		std::vector<glm::vec3> offsets;

		// Collect all collider components
		if (entity.HasComponent<BoxCollider3DComponent>())
		{
			const auto& component = entity.GetComponent<BoxCollider3DComponent>();
			auto shape = CreateBoxShape(component);
			if (shape)
			{
				shapes.push_back(shape);
				offsets.push_back(component.Offset);
			}
		}

		if (entity.HasComponent<SphereCollider3DComponent>())
		{
			const auto& component = entity.GetComponent<SphereCollider3DComponent>();
			auto shape = CreateSphereShape(component);
			if (shape)
			{
				shapes.push_back(shape);
				offsets.push_back(component.Offset);
			}
		}

		if (entity.HasComponent<CapsuleCollider3DComponent>())
		{
			const auto& component = entity.GetComponent<CapsuleCollider3DComponent>();
			auto shape = CreateCapsuleShape(component);
			if (shape)
			{
				shapes.push_back(shape);
				offsets.push_back(component.Offset);
			}
		}

		if (shapes.empty())
		{
			OLO_CORE_WARN("No valid shapes found for compound shape creation");
			return nullptr;
		}

		// If only one shape, return it directly (no need for compound)
		if (shapes.size() == 1)
		{
			return shapes[0];
		}

		// Create compound shape
		JPH::StaticCompoundShapeSettings compoundSettings;
		for (sizet i = 0; i < shapes.size(); ++i)
		{
			JPH::Vec3 joltOffset = JoltUtils::ToJoltVector(offsets[i]);
			compoundSettings.AddShape(joltOffset, JPH::Quat::sIdentity(), shapes[i]);
		}

		JPH::Shape::ShapeResult result = compoundSettings.Create();
		if (result.HasError())
		{
			OLO_CORE_ERROR("Failed to create compound shape: {0}", result.GetError().c_str());
			return nullptr;
		}

		return result.Get();
	}

	JPH::Ref<JPH::Shape> JoltShapes::CreateShapeForEntity(Entity entity)
	{
		if (!entity)
		{
			OLO_CORE_ERROR("Cannot create shape for invalid entity");
			return nullptr;
		}

		i32 colliderCount = 0;
		JPH::Ref<JPH::Shape> singleShape = nullptr;

		// Check for box collider
		if (entity.HasComponent<BoxCollider3DComponent>())
		{
			const auto& component = entity.GetComponent<BoxCollider3DComponent>();
			auto shape = CreateBoxShape(component);
			if (shape)
			{
				singleShape = shape;
				colliderCount++;
			}
		}

		// Check for sphere collider
		if (entity.HasComponent<SphereCollider3DComponent>())
		{
			const auto& component = entity.GetComponent<SphereCollider3DComponent>();
			auto shape = CreateSphereShape(component);
			if (shape)
			{
				if (colliderCount == 0)
					singleShape = shape;
				colliderCount++;
			}
		}

		// Check for capsule collider
		if (entity.HasComponent<CapsuleCollider3DComponent>())
		{
			const auto& component = entity.GetComponent<CapsuleCollider3DComponent>();
			auto shape = CreateCapsuleShape(component);
			if (shape)
			{
				if (colliderCount == 0)
					singleShape = shape;
				colliderCount++;
			}
		}

		// If no colliders found, create a default box
		if (colliderCount == 0)
		{
			OLO_CORE_WARN("No colliders found on entity {0}, creating default box shape", (u64)entity.GetUUID());
			return CreateBoxShapeInternal(glm::vec3(0.5f)); // 1x1x1 box
		}

		// If only one collider, return its shape directly
		if (colliderCount == 1)
		{
			return singleShape;
		}

		// Multiple colliders, create compound shape
		return CreateCompoundShape(entity, false);
	}

	JPH::Ref<JPH::Shape> JoltShapes::GetOrCreateCachedShape(const std::string& cacheKey, std::function<JPH::Ref<JPH::Shape>()> createFunc)
	{
		auto it = s_ShapeCache.find(cacheKey);
		if (it != s_ShapeCache.end())
		{
			return it->second;
		}

		auto shape = createFunc();
		if (shape)
		{
			s_ShapeCache[cacheKey] = shape;
		}
		return shape;
	}

	void JoltShapes::ClearShapeCache()
	{
		s_ShapeCache.clear();
	}

	glm::vec3 JoltShapes::CalculateShapeLocalCenterOfMass(Entity entity)
	{
		// For now, return zero. This could be enhanced to calculate actual COM
		return glm::vec3(0.0f);
	}

	f32 JoltShapes::CalculateShapeVolume(const JPH::Shape* shape)
	{
		if (!shape)
			return 0.0f;

		return shape->GetVolume();
	}

	bool JoltShapes::IsShapeValid(const JPH::Shape* shape)
	{
		return shape != nullptr;
	}

	ShapeType JoltShapes::GetShapeType(const JPH::Shape* shape)
	{
		if (!shape)
			return ShapeType::Box; // Default

		// Use dynamic casting to determine shape type since enum constants may not be accessible
		if (dynamic_cast<const JPH::BoxShape*>(shape))
			return ShapeType::Box;
		else if (dynamic_cast<const JPH::SphereShape*>(shape))
			return ShapeType::Sphere;
		else if (dynamic_cast<const JPH::CapsuleShape*>(shape))
			return ShapeType::Capsule;
		else if (dynamic_cast<const JPH::MeshShape*>(shape))
			return ShapeType::TriangleMesh;
		else if (dynamic_cast<const JPH::ConvexHullShape*>(shape))
			return ShapeType::ConvexMesh;
		else if (dynamic_cast<const JPH::StaticCompoundShape*>(shape))
			return ShapeType::CompoundShape;
		else if (dynamic_cast<const JPH::MutableCompoundShape*>(shape))
			return ShapeType::MutableCompoundShape;
		else
			return ShapeType::Box; // Default fallback
	}

	const char* JoltShapes::GetShapeTypeName(const JPH::Shape* shape)
	{
		return ShapeUtils::ShapeTypeToString(GetShapeType(shape));
	}

	// Private helper implementations

	JPH::Ref<JPH::Shape> JoltShapes::CreateBoxShapeInternal(const glm::vec3& halfExtents)
	{
		JPH::Vec3 joltHalfExtents = JoltUtils::ToJoltVector(halfExtents);
		return new JPH::BoxShape(joltHalfExtents);
	}

	JPH::Ref<JPH::Shape> JoltShapes::CreateSphereShapeInternal(f32 radius)
	{
		return new JPH::SphereShape(radius);
	}

	JPH::Ref<JPH::Shape> JoltShapes::CreateCapsuleShapeInternal(f32 radius, f32 halfHeight)
	{
		return new JPH::CapsuleShape(halfHeight, radius);
	}

	bool JoltShapes::ValidateBoxDimensions(const glm::vec3& halfExtents)
	{
		return halfExtents.x >= MinShapeSize && halfExtents.x <= MaxShapeSize &&
			   halfExtents.y >= MinShapeSize && halfExtents.y <= MaxShapeSize &&
			   halfExtents.z >= MinShapeSize && halfExtents.z <= MaxShapeSize;
	}

	bool JoltShapes::ValidateSphereDimensions(f32 radius)
	{
		return radius >= MinShapeSize && radius <= MaxShapeSize;
	}

	bool JoltShapes::ValidateCapsuleDimensions(f32 radius, f32 halfHeight)
	{
		return radius >= MinShapeSize && radius <= MaxShapeSize &&
			   halfHeight >= MinShapeSize && halfHeight <= MaxShapeSize &&
			   halfHeight >= radius; // Capsule half-height must be at least as large as radius
	}

	glm::vec3 JoltShapes::ApplyScaleToBoxExtents(const glm::vec3& halfExtents, const glm::vec3& scale)
	{
		return halfExtents * glm::abs(scale); // Take absolute value to handle negative scales
	}

	f32 JoltShapes::ApplyScaleToSphereRadius(f32 radius, const glm::vec3& scale)
	{
		// For sphere, use the maximum component of the scale
		return radius * glm::max(glm::max(glm::abs(scale.x), glm::abs(scale.y)), glm::abs(scale.z));
	}

	void JoltShapes::ApplyScaleToCapsule(f32& radius, f32& halfHeight, const glm::vec3& scale)
	{
		// For capsule, radius is affected by X and Z, height by Y
		f32 radiusScale = glm::max(glm::abs(scale.x), glm::abs(scale.z));
		f32 heightScale = glm::abs(scale.y);
		
		radius *= radiusScale;
		halfHeight *= heightScale;
		
		// Ensure half-height is at least as large as radius
		if (halfHeight < radius)
			halfHeight = radius;
	}

}