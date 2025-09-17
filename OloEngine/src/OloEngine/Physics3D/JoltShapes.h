#pragma once

#include "Physics3DTypes.h"
#include "JoltUtils.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "MeshColliderCache.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/MutableCompoundShape.h>

#include <unordered_map>
#include <vector>

namespace OloEngine {

	class JoltShapes
	{
	public:
		static void Initialize();
		static void Shutdown();

		// Create shapes from components
		static JPH::Ref<JPH::Shape> CreateBoxShape(const BoxCollider3DComponent& component, const glm::vec3& scale = glm::vec3(1.0f));
		static JPH::Ref<JPH::Shape> CreateSphereShape(const SphereCollider3DComponent& component, const glm::vec3& scale = glm::vec3(1.0f));
		static JPH::Ref<JPH::Shape> CreateCapsuleShape(const CapsuleCollider3DComponent& component, const glm::vec3& scale = glm::vec3(1.0f));
		static JPH::Ref<JPH::Shape> CreateMeshShape(const MeshCollider3DComponent& component, const glm::vec3& scale = glm::vec3(1.0f));
		static JPH::Ref<JPH::Shape> CreateConvexMeshShape(const ConvexMeshCollider3DComponent& component, const glm::vec3& scale = glm::vec3(1.0f));
		static JPH::Ref<JPH::Shape> CreateTriangleMeshShape(const TriangleMeshCollider3DComponent& component, const glm::vec3& scale = glm::vec3(1.0f));
		
		// Create compound shapes
		static JPH::Ref<JPH::Shape> CreateCompoundShape(Entity entity, bool isMutable = false);

		// Create shapes from entity (analyzes all collider components)
		static JPH::Ref<JPH::Shape> CreateShapeForEntity(Entity entity);

		// Shape caching for performance (optional)
		static JPH::Ref<JPH::Shape> GetOrCreateCachedShape(const std::string& cacheKey, std::function<JPH::Ref<JPH::Shape>()> createFunc);
		static void ClearShapeCache();

		// Helper functions
		static glm::vec3 CalculateShapeLocalCenterOfMass(Entity entity);
		static f32 CalculateShapeVolume(const JPH::Shape* shape);
		static bool IsShapeValid(const JPH::Shape* shape);

		// Shape type utilities
		static ShapeType GetShapeType(const JPH::Shape* shape);
		static const char* GetShapeTypeName(const JPH::Shape* shape);

	private:
		// Internal shape creation helpers
		static JPH::Ref<JPH::Shape> CreateBoxShapeInternal(const glm::vec3& halfExtents);
		static JPH::Ref<JPH::Shape> CreateSphereShapeInternal(f32 radius);
		static JPH::Ref<JPH::Shape> CreateCapsuleShapeInternal(f32 radius, f32 halfHeight);
		static JPH::Ref<JPH::Shape> CreateMeshShapeInternal(AssetHandle meshAsset, bool useComplexAsSimple, const glm::vec3& scale);
		static JPH::Ref<JPH::Shape> CreateConvexMeshShapeInternal(AssetHandle meshAsset, f32 convexRadius, const glm::vec3& scale);
		static JPH::Ref<JPH::Shape> CreateTriangleMeshShapeInternal(AssetHandle meshAsset, const glm::vec3& scale);

		// Shape validation helpers
		static bool ValidateBoxDimensions(const glm::vec3& halfExtents);
		static bool ValidateSphereDimensions(f32 radius);
		static bool ValidateCapsuleDimensions(f32 radius, f32 halfHeight);
		static bool ValidateMeshAsset(AssetHandle meshAsset);

		// Scaling helpers
		static glm::vec3 ApplyScaleToBoxExtents(const glm::vec3& halfExtents, const glm::vec3& scale);
		static f32 ApplyScaleToSphereRadius(f32 radius, const glm::vec3& scale);
		static void ApplyScaleToCapsule(f32& radius, f32& halfHeight, const glm::vec3& scale);

	private:
		static bool s_Initialized;
		static std::unordered_map<std::string, JPH::Ref<JPH::Shape>> s_ShapeCache;
		
		// Constants for shape validation
		static constexpr f32 MinShapeSize = 0.001f;
		static constexpr f32 MaxShapeSize = 10000.0f;
	};

}