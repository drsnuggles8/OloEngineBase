#include "OloEnginePCH.h"
#include "JoltShapes.h"
#include "JoltBinaryStream.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/MeshColliderAsset.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "MeshColliderCache.h"

#include <atomic>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/MutableCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/DecoratedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

namespace OloEngine {

	using ShapeUtils::ShapeTypeToString;

	std::atomic<bool> JoltShapes::s_Initialized = false;
	std::unordered_map<std::string, JPH::Ref<JPH::Shape>> JoltShapes::s_ShapeCache;
	std::shared_mutex JoltShapes::s_ShapeCacheMutex;
	std::atomic<bool> JoltShapes::s_PersistentCacheEnabled = true;
	std::filesystem::path JoltShapes::s_PersistentCacheDirectory = "assets/cache/shapes";

	void JoltShapes::Initialize()
	{
		if (s_Initialized.load(std::memory_order_acquire))
			return;

		OLO_CORE_INFO("Initializing JoltShapes system");
		{
			std::unique_lock<std::shared_mutex> lock(s_ShapeCacheMutex);
			s_ShapeCache.clear();
		}
		
		// Initialize mesh collider cache
		MeshColliderCache::GetInstance().Initialize();
		
		// Create persistent cache directory if enabled
		if (s_PersistentCacheEnabled.load(std::memory_order_relaxed))
		{
			try
			{
				if (!std::filesystem::exists(s_PersistentCacheDirectory))
				{
					std::filesystem::create_directories(s_PersistentCacheDirectory);
				}
				OLO_CORE_INFO("JoltShapes persistent cache directory: {}", s_PersistentCacheDirectory.string());
			}
			catch (const std::exception& e)
			{
				OLO_CORE_ERROR("Failed to create persistent cache directory: {}", e.what());
				s_PersistentCacheEnabled.store(false, std::memory_order_relaxed);
			}
		}
		
		s_Initialized.store(true, std::memory_order_release);
	}

	void JoltShapes::Shutdown()
	{
		if (!s_Initialized.load(std::memory_order_acquire))
			return;

		OLO_CORE_INFO("Shutting down JoltShapes system");
		
		// Shutdown mesh collider cache
		MeshColliderCache::GetInstance().Shutdown();
		
		ClearShapeCache();
		s_Initialized.store(false, std::memory_order_release);
	}

	JPH::Ref<JPH::Shape> JoltShapes::CreateBoxShape(const BoxCollider3DComponent& component, const glm::vec3& scale)
	{
		glm::vec3 scaledHalfExtents = ApplyScaleToBoxExtents(component.m_HalfExtents, scale);
		
		if (!ValidateBoxDimensions(scaledHalfExtents))
		{
			OLO_CORE_ERROR("Invalid box dimensions: {0}, {1}, {2}", scaledHalfExtents.x, scaledHalfExtents.y, scaledHalfExtents.z);
			return nullptr;
		}

		return CreateBoxShapeInternal(scaledHalfExtents);
	}

	JPH::Ref<JPH::Shape> JoltShapes::CreateSphereShape(const SphereCollider3DComponent& component, const glm::vec3& scale)
	{
		f32 scaledRadius = ApplyScaleToSphereRadius(component.m_Radius, scale);
		
		if (!ValidateSphereDimensions(scaledRadius))
		{
			OLO_CORE_ERROR("Invalid sphere radius: {0}", scaledRadius);
			return nullptr;
		}

		return CreateSphereShapeInternal(scaledRadius);
	}

	JPH::Ref<JPH::Shape> JoltShapes::CreateCapsuleShape(const CapsuleCollider3DComponent& component, const glm::vec3& scale)
	{
		auto [scaledRadius, scaledHalfHeight] = ApplyScaleToCapsule(component.m_Radius, component.m_HalfHeight, scale);
		
		if (!ValidateCapsuleDimensions(scaledRadius, scaledHalfHeight))
		{
			OLO_CORE_ERROR("Invalid capsule dimensions: radius={0}, halfHeight={1}", scaledRadius, scaledHalfHeight);
			return nullptr;
		}

		return CreateCapsuleShapeInternal(scaledRadius, scaledHalfHeight);
	}

	JPH::Ref<JPH::Shape> JoltShapes::CreateMeshShape(const MeshCollider3DComponent& component, const glm::vec3& scale)
	{
		if (!ValidateMeshAsset(component.m_ColliderAsset))
		{
			OLO_CORE_ERROR("Invalid mesh collider asset handle: {0}", component.m_ColliderAsset);
			return nullptr;
		}

		return CreateMeshShapeInternal(component.m_ColliderAsset, component.m_UseComplexAsSimple, scale);
	}

	JPH::Ref<JPH::Shape> JoltShapes::CreateConvexMeshShape(const ConvexMeshCollider3DComponent& component, const glm::vec3& scale)
	{
		if (!ValidateMeshAsset(component.m_ColliderAsset))
		{
			OLO_CORE_ERROR("Invalid convex mesh collider asset handle: {0}", component.m_ColliderAsset);
			return nullptr;
		}

		return CreateConvexMeshShapeInternal(component.m_ColliderAsset, component.m_ConvexRadius, scale);
	}

	JPH::Ref<JPH::Shape> JoltShapes::CreateTriangleMeshShape(const TriangleMeshCollider3DComponent& component, const glm::vec3& scale)
	{
		if (!ValidateMeshAsset(component.m_ColliderAsset))
		{
			OLO_CORE_ERROR("Invalid triangle mesh collider asset handle: {0}", component.m_ColliderAsset);
			return nullptr;
		}

		return CreateTriangleMeshShapeInternal(component.m_ColliderAsset, scale);
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

		// Count potential collider components to pre-allocate vectors
		sizet colliderCount = 0;
		if (entity.HasComponent<BoxCollider3DComponent>()) colliderCount++;
		if (entity.HasComponent<SphereCollider3DComponent>()) colliderCount++;
		if (entity.HasComponent<CapsuleCollider3DComponent>()) colliderCount++;
		if (entity.HasComponent<MeshCollider3DComponent>()) colliderCount++;
		if (entity.HasComponent<ConvexMeshCollider3DComponent>()) colliderCount++;
		if (entity.HasComponent<TriangleMeshCollider3DComponent>()) colliderCount++;

		// Pre-allocate vectors to avoid reallocations during population
		shapes.reserve(colliderCount);
		offsets.reserve(colliderCount);

		// Get entity transform for scaling primitive colliders
		const auto& transform = entity.GetTransform();
		const glm::vec3& entityScale = transform.Scale;

		// Collect all collider components
		if (entity.HasComponent<BoxCollider3DComponent>())
		{
			const auto& component = entity.GetComponent<BoxCollider3DComponent>();
			auto shape = CreateBoxShape(component, entityScale);
			if (shape)
			{
				shapes.push_back(shape);
				offsets.push_back(component.m_Offset);
			}
		}

		if (entity.HasComponent<SphereCollider3DComponent>())
		{
			const auto& component = entity.GetComponent<SphereCollider3DComponent>();
			auto shape = CreateSphereShape(component, entityScale);
			if (shape)
			{
				shapes.push_back(shape);
				offsets.push_back(component.m_Offset);
			}
		}

		if (entity.HasComponent<CapsuleCollider3DComponent>())
		{
			const auto& component = entity.GetComponent<CapsuleCollider3DComponent>();
			auto shape = CreateCapsuleShape(component, entityScale);
			if (shape)
			{
				shapes.push_back(shape);
				offsets.push_back(component.m_Offset);
			}
		}

		if (entity.HasComponent<MeshCollider3DComponent>())
		{
			const auto& component = entity.GetComponent<MeshCollider3DComponent>();
			// Combine entity scale with component scale for mesh colliders
			const glm::vec3 combinedScale = entityScale * component.m_Scale;
			auto shape = CreateMeshShape(component, combinedScale);
			if (shape)
			{
				shapes.push_back(shape);
				offsets.push_back(component.m_Offset);
			}
		}

		if (entity.HasComponent<ConvexMeshCollider3DComponent>())
		{
			const auto& component = entity.GetComponent<ConvexMeshCollider3DComponent>();
			// Combine entity scale with component scale for convex mesh colliders
			const glm::vec3 combinedScale = entityScale * component.m_Scale;
			auto shape = CreateConvexMeshShape(component, combinedScale);
			if (shape)
			{
				shapes.push_back(shape);
				offsets.push_back(component.m_Offset);
			}
		}

		if (entity.HasComponent<TriangleMeshCollider3DComponent>())
		{
			const auto& component = entity.GetComponent<TriangleMeshCollider3DComponent>();
			// Combine entity scale with component scale for triangle mesh colliders
			const glm::vec3 combinedScale = entityScale * component.m_Scale;
			auto shape = CreateTriangleMeshShape(component, combinedScale);
			if (shape)
			{
				shapes.push_back(shape);
				offsets.push_back(component.m_Offset);
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

		// Create compound shape based on mutability requirement
		if (isMutable)
		{
			// Create mutable compound shape for dynamic modification
			JPH::MutableCompoundShapeSettings mutableSettings;
			for (sizet i = 0; i < shapes.size(); ++i)
			{
				JPH::Vec3 joltOffset = JoltUtils::ToJoltVector(offsets[i]);
				mutableSettings.AddShape(joltOffset, JPH::Quat::sIdentity(), shapes[i]);
			}

			JPH::Shape::ShapeResult result = mutableSettings.Create();
			if (result.HasError())
			{
				OLO_CORE_ERROR("Failed to create mutable compound shape: {0}", result.GetError().c_str());
				return nullptr;
			}

			return result.Get();
		}
		else
		{
			// Create static compound shape for better performance when no modification is needed
			JPH::StaticCompoundShapeSettings compoundSettings;
			for (sizet i = 0; i < shapes.size(); ++i)
			{
				JPH::Vec3 joltOffset = JoltUtils::ToJoltVector(offsets[i]);
				compoundSettings.AddShape(joltOffset, JPH::Quat::sIdentity(), shapes[i]);
			}

			JPH::Shape::ShapeResult result = compoundSettings.Create();
			if (result.HasError())
			{
				OLO_CORE_ERROR("Failed to create static compound shape: {0}", result.GetError().c_str());
				return nullptr;
			}

			return result.Get();
		}
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

		// Get entity transform for scaling primitive colliders
		const auto& transform = entity.GetTransform();
		const glm::vec3& entityScale = transform.Scale;

		// Check for box collider
		if (entity.HasComponent<BoxCollider3DComponent>())
		{
			const auto& component = entity.GetComponent<BoxCollider3DComponent>();
			auto shape = CreateBoxShape(component, entityScale);
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
			auto shape = CreateSphereShape(component, entityScale);
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
			auto shape = CreateCapsuleShape(component, entityScale);
			if (shape)
			{
				if (colliderCount == 0)
					singleShape = shape;
				colliderCount++;
			}
		}

		// Check for mesh collider
		if (entity.HasComponent<MeshCollider3DComponent>())
		{
			const auto& component = entity.GetComponent<MeshCollider3DComponent>();
			// Combine entity scale with component scale for mesh colliders
			const glm::vec3 combinedScale = entityScale * component.m_Scale;
			auto shape = CreateMeshShape(component, combinedScale);
			if (shape)
			{
				if (colliderCount == 0)
					singleShape = shape;
				colliderCount++;
			}
		}

		// Check for convex mesh collider
		if (entity.HasComponent<ConvexMeshCollider3DComponent>())
		{
			const auto& component = entity.GetComponent<ConvexMeshCollider3DComponent>();
			// Combine entity scale with component scale for convex mesh colliders
			const glm::vec3 combinedScale = entityScale * component.m_Scale;
			auto shape = CreateConvexMeshShape(component, combinedScale);
			if (shape)
			{
				if (colliderCount == 0)
					singleShape = shape;
				colliderCount++;
			}
		}

		// Check for triangle mesh collider
		if (entity.HasComponent<TriangleMeshCollider3DComponent>())
		{
			const auto& component = entity.GetComponent<TriangleMeshCollider3DComponent>();
			// Combine entity scale with component scale for triangle mesh colliders
			const glm::vec3 combinedScale = entityScale * component.m_Scale;
			auto shape = CreateTriangleMeshShape(component, combinedScale);
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
		// First try to find with shared lock (read-only)
		{
			std::shared_lock<std::shared_mutex> readLock(s_ShapeCacheMutex);
			auto it = s_ShapeCache.find(cacheKey);
			if (it != s_ShapeCache.end())
			{
				return it->second;
			}
		}

		// Not found, create shape (outside lock to avoid holding during creation)
		auto shape = createFunc();
		if (shape)
		{
			// Insert with unique lock (write access)
			std::unique_lock<std::shared_mutex> writeLock(s_ShapeCacheMutex);
			// Check again in case another thread inserted while we were creating
			auto it = s_ShapeCache.find(cacheKey);
			if (it != s_ShapeCache.end())
			{
				return it->second; // Another thread beat us to it
			}
			s_ShapeCache[cacheKey] = shape;
		}
		return shape;
	}

	void JoltShapes::ClearShapeCache()
	{
		std::unique_lock<std::shared_mutex> lock(s_ShapeCacheMutex);
		s_ShapeCache.clear();
	}

	glm::vec3 JoltShapes::CalculateShapeLocalCenterOfMass(Entity entity)
	{
		if (!entity)
		{
			OLO_CORE_WARN("CalculateShapeLocalCenterOfMass: Invalid entity, returning zero center of mass");
			return glm::vec3(0.0f);
		}

		// Calculate weighted center of mass for all colliders on this entity
		glm::vec3 totalWeightedCOM(0.0f);
		f32 totalVolume = 0.0f;

		// Check for box collider
		if (entity.HasComponent<BoxCollider3DComponent>())
		{
			const auto& collider = entity.GetComponent<BoxCollider3DComponent>();
			// Box center of mass is at its geometric center (offset position)
			glm::vec3 boxCOM = collider.m_Offset;
			// Box volume = 8 * halfExtents.x * halfExtents.y * halfExtents.z
			f32 boxVolume = 8.0f * collider.m_HalfExtents.x * collider.m_HalfExtents.y * collider.m_HalfExtents.z;
			
			totalWeightedCOM += boxCOM * boxVolume;
			totalVolume += boxVolume;
		}

		// Check for sphere collider
		if (entity.HasComponent<SphereCollider3DComponent>())
		{
			const auto& collider = entity.GetComponent<SphereCollider3DComponent>();
			// Sphere center of mass is at its center (offset position)
			glm::vec3 sphereCOM = collider.m_Offset;
			// Sphere volume = (4/3) * π * r³
			f32 sphereVolume = (4.0f / 3.0f) * glm::pi<f32>() * glm::pow(collider.m_Radius, 3.0f);
			
			totalWeightedCOM += sphereCOM * sphereVolume;
			totalVolume += sphereVolume;
		}

		// Check for capsule collider
		if (entity.HasComponent<CapsuleCollider3DComponent>())
		{
			const auto& collider = entity.GetComponent<CapsuleCollider3DComponent>();
			// Capsule center of mass is at its geometric center (offset position)
			glm::vec3 capsuleCOM = collider.m_Offset;
			// Capsule volume = π * r² * (2 * halfHeight) + (4/3) * π * r³ (cylinder + hemisphere caps)
			f32 cylinderVolume = glm::pi<f32>() * collider.m_Radius * collider.m_Radius * (2.0f * collider.m_HalfHeight);
			f32 sphereCapVolume = (4.0f / 3.0f) * glm::pi<f32>() * glm::pow(collider.m_Radius, 3.0f);
			f32 capsuleVolume = cylinderVolume + sphereCapVolume;
			
			totalWeightedCOM += capsuleCOM * capsuleVolume;
			totalVolume += capsuleVolume;
		}

		// For mesh colliders, we approximate volume using AABB dimensions (width*height*depth)
		// TODO: For most accurate physics simulation, implement proper mesh volume calculation:
		// 1. For triangle meshes: Use divergence theorem with triangulated surface or Monte Carlo integration
		// 2. For convex meshes: Compute actual convex hull volume using tetrahedralization
		// 3. For complex meshes: Use convex decomposition and sum component volumes
		// This would require accessing vertex/triangle data from the mesh geometry
		
		if (entity.HasComponent<MeshCollider3DComponent>())
		{
			const auto& collider = entity.GetComponent<MeshCollider3DComponent>();
			// Use offset as center of mass approximation
			glm::vec3 meshCOM = collider.m_Offset;
			// Compute volume from mesh AABB, with fallback to conservative default
			// TODO: For accurate center-of-mass calculation, also compute actual mesh centroid from vertex data
			f32 meshVolume = ComputeMeshVolume(collider.m_ColliderAsset, collider.m_Scale);
			
			totalWeightedCOM += meshCOM * meshVolume;
			totalVolume += meshVolume;
		}

		if (entity.HasComponent<ConvexMeshCollider3DComponent>())
		{
			const auto& collider = entity.GetComponent<ConvexMeshCollider3DComponent>();
			// Use offset as center of mass approximation
			glm::vec3 convexMeshCOM = collider.m_Offset;
			// Compute volume from mesh AABB, with fallback to conservative default
			f32 convexMeshVolume = ComputeMeshVolume(collider.m_ColliderAsset, collider.m_Scale);
			
			totalWeightedCOM += convexMeshCOM * convexMeshVolume;
			totalVolume += convexMeshVolume;
		}

		if (entity.HasComponent<TriangleMeshCollider3DComponent>())
		{
			const auto& collider = entity.GetComponent<TriangleMeshCollider3DComponent>();
			// Use offset as center of mass approximation
			glm::vec3 triangleMeshCOM = collider.m_Offset;
			// Compute volume from mesh AABB, with fallback to conservative default
			f32 triangleMeshVolume = ComputeMeshVolume(collider.m_ColliderAsset, collider.m_Scale);
			
			totalWeightedCOM += triangleMeshCOM * triangleMeshVolume;
			totalVolume += triangleMeshVolume;
		}

		// Calculate final center of mass
		if (totalVolume > 0.0f)
		{
			return totalWeightedCOM / totalVolume;
		}
		else
		{
			// No colliders found, return zero center of mass
			OLO_CORE_WARN("CalculateShapeLocalCenterOfMass: Entity has no collider components, returning zero center of mass");
			return glm::vec3(0.0f);
		}
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

		// Use fast switch on shape type and subtype for better performance
		switch (shape->GetType())
		{
			case JPH::EShapeType::Convex:
			{
				switch (shape->GetSubType())
				{
					case JPH::EShapeSubType::Box:
						return ShapeType::Box;
					case JPH::EShapeSubType::Sphere:
						return ShapeType::Sphere;
					case JPH::EShapeSubType::Capsule:
						return ShapeType::Capsule;
					case JPH::EShapeSubType::ConvexHull:
						return ShapeType::ConvexMesh;
					default:
						OLO_CORE_WARN("GetShapeType: Unknown convex shape subtype {0}, defaulting to Box", (i32)shape->GetSubType());
						return ShapeType::Box; // Default for other convex shapes
				}
			}
			case JPH::EShapeType::Compound:
			{
				switch (shape->GetSubType())
				{
					case JPH::EShapeSubType::StaticCompound:
						return ShapeType::CompoundShape;
					case JPH::EShapeSubType::MutableCompound:
						return ShapeType::MutableCompoundShape;
					default:
						OLO_CORE_WARN("GetShapeType: Unknown compound shape subtype {0}, defaulting to CompoundShape", (i32)shape->GetSubType());
						return ShapeType::CompoundShape; // Default for compound shapes
				}
			}
			case JPH::EShapeType::Mesh:
				return ShapeType::TriangleMesh;
			case JPH::EShapeType::Decorated:
			{
				// Handle decorated shapes by unwrapping the inner shape
				switch (shape->GetSubType())
				{
					case JPH::EShapeSubType::Scaled:
					{
						// Safe cast to ScaledShape with runtime type checking
						const JPH::ScaledShape* scaledShape = dynamic_cast<const JPH::ScaledShape*>(shape);
						if (scaledShape && scaledShape->GetInnerShape())
						{
							// Recursively determine the inner shape type
							return GetShapeType(scaledShape->GetInnerShape());
						}
						else
						{
							OLO_CORE_WARN("GetShapeType: Failed to cast to ScaledShape or ScaledShape has no inner shape, defaulting to Box");
							return ShapeType::Box;
						}
					}
					default:
					{
						// Handle other decorated shape subtypes with safe runtime type checking
						const JPH::DecoratedShape* decoratedShape = dynamic_cast<const JPH::DecoratedShape*>(shape);
						if (decoratedShape && decoratedShape->GetInnerShape())
						{
							OLO_CORE_WARN("GetShapeType: Unknown decorated shape subtype {0}, unwrapping inner shape", (i32)shape->GetSubType());
							return GetShapeType(decoratedShape->GetInnerShape());
						}
						else
						{
							OLO_CORE_WARN("GetShapeType: Failed to cast to DecoratedShape or decorated shape subtype {0} has no inner shape, defaulting to Box", (i32)shape->GetSubType());
							return ShapeType::Box;
						}
					}
				}
			}
			case JPH::EShapeType::HeightField:
				OLO_CORE_WARN("GetShapeType: HeightField shape type {0} not supported, defaulting to Box", (i32)shape->GetType());
				return ShapeType::Box;
			case JPH::EShapeType::SoftBody:
				OLO_CORE_WARN("GetShapeType: SoftBody shape type {0} not supported, defaulting to Box", (i32)shape->GetType());
				return ShapeType::Box;
			case JPH::EShapeType::User1:
			case JPH::EShapeType::User2:
			case JPH::EShapeType::User3:
			case JPH::EShapeType::User4:
				OLO_CORE_WARN("GetShapeType: User-defined shape type {0} not supported, defaulting to Box", (i32)shape->GetType());
				return ShapeType::Box;
			case JPH::EShapeType::Plane:
				OLO_CORE_WARN("GetShapeType: Plane shape type {0} not supported, defaulting to Box", (i32)shape->GetType());
				return ShapeType::Box;
			case JPH::EShapeType::Empty:
				OLO_CORE_WARN("GetShapeType: Empty shape type {0} not supported, defaulting to Box", (i32)shape->GetType());
				return ShapeType::Box;
			default:
				OLO_CORE_WARN("GetShapeType: Unknown shape type {0}, defaulting to Box", (i32)shape->GetType());
				return ShapeType::Box; // Default fallback
		}
	}

	const char* JoltShapes::GetShapeTypeName(const JPH::Shape* shape)
	{
		return ShapeTypeToString(GetShapeType(shape));
	}

	// Private helper implementations

	f32 JoltShapes::ComputeMeshVolume(AssetHandle colliderAsset, const glm::vec3& scale)
	{
		// TODO: For most accurate volume calculation, compute actual mesh volume from vertex/triangle data
		// using methods like divergence theorem, tetrahedralization, or convex decomposition.
		// This would require accessing the original mesh geometry and implementing volume integration.
		
		// Conservative default volume for fallback cases
		constexpr f32 defaultVolume = 1.0f;
		
		// Attempt to get mesh asset from collider asset
		auto meshColliderAsset = AssetManager::GetAsset<MeshColliderAsset>(colliderAsset);
		if (!meshColliderAsset)
		{
			OLO_CORE_WARN("ComputeMeshVolume: Could not get MeshColliderAsset for handle {}, using default volume {}", 
						  colliderAsset, defaultVolume);
			return defaultVolume;
		}
		
		// Get the mesh source from the collider asset
		auto meshSource = AssetManager::GetAsset<MeshSource>(meshColliderAsset->m_ColliderMesh);
		if (!meshSource)
		{
			OLO_CORE_WARN("ComputeMeshVolume: Could not get MeshSource for collider mesh handle {}, using default volume {}", 
						  meshColliderAsset->m_ColliderMesh, defaultVolume);
			return defaultVolume;
		}
		
		// Get the AABB and compute approximate volume from bounding box
		const auto& boundingBox = meshSource->GetBoundingBox();
		glm::vec3 size = boundingBox.GetSize();
		
		// Apply scaling
		size *= scale;
		
		// Compute volume as width * height * depth
		f32 volume = size.x * size.y * size.z;
		
		// Validate the computed volume
		if (volume <= 0.0f || !std::isfinite(volume))
		{
			OLO_CORE_WARN("ComputeMeshVolume: Invalid computed volume {} for mesh, using default volume {}", 
						  volume, defaultVolume);
			return defaultVolume;
		}
		
		return volume;
	}

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

	JPH::Ref<JPH::Shape> JoltShapes::CreateMeshShapeInternal(AssetHandle meshAsset, bool useComplexAsSimple, const glm::vec3& scale)
	{
		// Get the mesh collider cache instance
		auto& cache = MeshColliderCache::GetInstance();
		
		// Get the asset reference
		auto meshColliderAsset = AssetManager::GetAsset<MeshColliderAsset>(meshAsset);
		if (!meshColliderAsset)
		{
			OLO_CORE_ERROR("Failed to get MeshColliderAsset for handle {0}", meshAsset);
			return nullptr;
		}
		
		// Try to get cached mesh data
		const auto& cachedData = cache.GetMeshData(meshColliderAsset);
		if (!cachedData.m_IsValid)
		{
			OLO_CORE_ERROR("Failed to get valid cached mesh data for asset {0}", meshAsset);
			return nullptr;
		}

		// Choose between simple (convex) and complex (triangle) based on usage
		const MeshColliderData* meshData = nullptr;
		if (useComplexAsSimple || cachedData.m_ComplexColliderData.m_Submeshes.empty())
		{
			// Use convex shape for dynamic bodies or if no complex data
			if (cachedData.m_SimpleColliderData.m_Submeshes.empty())
			{
				OLO_CORE_ERROR("No simple (convex) mesh data available for asset {0}", meshAsset);
				return nullptr;
			}
			meshData = &cachedData.m_SimpleColliderData;
		}
		else
		{
			// Use triangle mesh for static bodies
			meshData = &cachedData.m_ComplexColliderData;
		}

		// For now, just use the first submesh - could be extended to support multiple submeshes
		if (meshData->m_Submeshes.empty())
		{
			OLO_CORE_ERROR("No submesh data available for asset {0}", meshAsset);
			return nullptr;
		}

		// Try to deserialize the Jolt shape from the selected meshData
		if (!meshData->m_Submeshes.empty())
		{
			const auto& colliderSubmesh = meshData->m_Submeshes[0];
			
			if (!colliderSubmesh.m_ColliderData.empty())
			{
				// Create buffer from the collider data
				Buffer buffer = Buffer::Copy(std::span<const u8>(colliderSubmesh.m_ColliderData.data(), colliderSubmesh.m_ColliderData.size()));

				// Try to deserialize the shape
				JPH::Ref<JPH::Shape> shape = JoltBinaryStreamUtils::DeserializeShapeFromBuffer(buffer);
				
				// Clean up buffer
				buffer.Release();

				if (shape)
				{
					OLO_CORE_TRACE("Successfully deserialized mesh shape for asset {0}", meshAsset);
					
					// Check if scaling is needed
					if (scale != glm::vec3(1.0f))
					{
						// Convert glm scale to Jolt Vec3 and create scaled shape
						JPH::Vec3 joltScale = JoltUtils::ToJoltVector(scale);
						return new JPH::ScaledShape(shape, joltScale);
					}
					
					return shape;
				}
			}
		}
		
		OLO_CORE_WARN("Mesh shape deserialization failed for asset {0}, falling back to placeholder", meshAsset);
		
		// For now, return a placeholder box shape
		// Convert scale to half-extents since CreateBoxShapeInternal expects half-extents, not full scale
		glm::vec3 halfExtents = scale * 0.5f;
		return CreateBoxShapeInternal(halfExtents);
	}

	JPH::Ref<JPH::Shape> JoltShapes::CreateConvexMeshShapeInternal(AssetHandle meshAsset, f32 convexRadius, const glm::vec3& scale)
	{
		(void)convexRadius; // Suppress unused parameter warning

		// Get the mesh collider cache instance
		auto& cache = MeshColliderCache::GetInstance();
		
		// Get the asset reference
		auto meshColliderAsset = AssetManager::GetAsset<MeshColliderAsset>(meshAsset);
		if (!meshColliderAsset)
		{
			OLO_CORE_ERROR("Failed to get MeshColliderAsset for handle {0}", meshAsset);
			return nullptr;
		}
		
		// Try to get cached mesh data
		const auto& cachedData = cache.GetMeshData(meshColliderAsset);
		if (!cachedData.m_IsValid || cachedData.m_SimpleColliderData.m_Submeshes.empty())
		{
			OLO_CORE_ERROR("Failed to get valid convex mesh data for asset {0}", meshAsset);
			return nullptr;
		}

		// For now, just use the first submesh
		const auto& submesh = cachedData.m_SimpleColliderData.m_Submeshes[0];
		
		// Try to deserialize the convex Jolt shape from cached ColliderData
		if (!submesh.m_ColliderData.empty())
		{
			// Create buffer from the collider data
			Buffer buffer = Buffer::Copy(std::span<const u8>(submesh.m_ColliderData.data(), submesh.m_ColliderData.size()));

			// Try to deserialize the shape
			JPH::Ref<JPH::Shape> shape = JoltBinaryStreamUtils::DeserializeShapeFromBuffer(buffer);
			
			// Clean up buffer
			buffer.Release();

			if (shape)
			{
				OLO_CORE_TRACE("Successfully deserialized convex mesh shape for asset {0}", meshAsset);
				
				// Check if scaling is needed
				if (scale != glm::vec3(1.0f))
				{
					// Convert glm scale to Jolt Vec3 and create scaled shape
					JPH::Vec3 joltScale = JoltUtils::ToJoltVector(scale);
					return new JPH::ScaledShape(shape, joltScale);
				}
				
				return shape;
			}
		}
		
		OLO_CORE_WARN("Convex mesh shape deserialization failed for asset {0}, falling back to placeholder", meshAsset);
		
		// For now, return a placeholder box shape
		// Convert scale to half-extents since CreateBoxShapeInternal expects half-extents, not full scale
		glm::vec3 halfExtents = scale * 0.5f;
		return CreateBoxShapeInternal(halfExtents);
	}

	JPH::Ref<JPH::Shape> JoltShapes::CreateTriangleMeshShapeInternal(AssetHandle meshAsset, const glm::vec3& scale)
	{
		// Get the mesh collider cache instance
		auto& cache = MeshColliderCache::GetInstance();
		
		// Get the asset reference
		auto meshColliderAsset = AssetManager::GetAsset<MeshColliderAsset>(meshAsset);
		if (!meshColliderAsset)
		{
			OLO_CORE_ERROR("Failed to get MeshColliderAsset for handle {0}", meshAsset);
			return nullptr;
		}
		
		// Try to get cached mesh data
		const auto& cachedData = cache.GetMeshData(meshColliderAsset);
		if (!cachedData.m_IsValid || cachedData.m_ComplexColliderData.m_Submeshes.empty())
		{
			OLO_CORE_ERROR("Failed to get valid triangle mesh data for asset {0}", meshAsset);
			return nullptr;
		}

		// For now, just use the first submesh
		const auto& submesh = cachedData.m_ComplexColliderData.m_Submeshes[0];
		
		// Try to deserialize the triangle mesh Jolt shape from cached ColliderData
		if (!submesh.m_ColliderData.empty())
		{
			// Create buffer from the collider data
			Buffer buffer = Buffer::Copy(std::span<const u8>(submesh.m_ColliderData.data(), submesh.m_ColliderData.size()));

			// Try to deserialize the shape
			JPH::Ref<JPH::Shape> shape = JoltBinaryStreamUtils::DeserializeShapeFromBuffer(buffer);
			
			// Clean up buffer
			buffer.Release();

			if (shape)
			{
				OLO_CORE_TRACE("Successfully deserialized triangle mesh shape for asset {0}", meshAsset);
				
				// Check if scaling is needed
				if (scale != glm::vec3(1.0f))
				{
					// Convert glm scale to Jolt Vec3 and create scaled shape
					JPH::Vec3 joltScale = JoltUtils::ToJoltVector(scale);
					return new JPH::ScaledShape(shape, joltScale);
				}
				
				return shape;
			}
		}
		
		OLO_CORE_WARN("Triangle mesh shape deserialization failed for asset {0}, falling back to placeholder", meshAsset);
		
		// For now, return a placeholder box shape
		// Convert scale to half-extents since CreateBoxShapeInternal expects half-extents, not full scale
		glm::vec3 halfExtents = scale * 0.5f;
		return CreateBoxShapeInternal(halfExtents);
	}

	bool JoltShapes::ValidateBoxDimensions(const glm::vec3& halfExtents)
	{
		return halfExtents.x >= s_MinShapeSize && halfExtents.x <= s_MaxShapeSize &&
			   halfExtents.y >= s_MinShapeSize && halfExtents.y <= s_MaxShapeSize &&
			   halfExtents.z >= s_MinShapeSize && halfExtents.z <= s_MaxShapeSize;
	}

	bool JoltShapes::ValidateSphereDimensions(f32 radius)
	{
		return radius >= s_MinShapeSize && radius <= s_MaxShapeSize;
	}

	bool JoltShapes::ValidateCapsuleDimensions(f32 radius, f32 halfHeight)
	{
		return radius >= s_MinShapeSize && radius <= s_MaxShapeSize &&
			   halfHeight >= s_MinShapeSize && halfHeight <= s_MaxShapeSize &&
			   halfHeight >= radius; // Capsule half-height must be at least as large as radius
	}

	bool JoltShapes::ValidateMeshAsset(AssetHandle meshAsset)
	{
		if (meshAsset == 0)
		{
			OLO_CORE_ERROR("Invalid mesh asset handle: 0");
			return false;
		}

		// Check if the asset exists in the asset manager
		if (!AssetManager::IsAssetHandleValid(meshAsset))
		{
			OLO_CORE_ERROR("Mesh asset handle {0} is not valid", meshAsset);
			return false;
		}

		// Additional validation could be added here to check asset type
		return true;
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

	std::pair<f32, f32> JoltShapes::ApplyScaleToCapsule(f32 radius, f32 halfHeight, const glm::vec3& scale)
	{
		// For capsule, radius is affected by X and Z, height by Y
		f32 radiusScale = glm::max(glm::abs(scale.x), glm::abs(scale.z));
		f32 heightScale = glm::abs(scale.y);
		
		f32 scaledRadius = radius * radiusScale;
		f32 scaledHalfHeight = halfHeight * heightScale;
		
		// Ensure half-height is at least as large as radius
		if (scaledHalfHeight < scaledRadius)
			scaledHalfHeight = scaledRadius;
			
		return { scaledRadius, scaledHalfHeight };
	}

	JPH::Ref<JPH::Shape> JoltShapes::GetOrCreatePersistentCachedShape(const std::string& cacheKey, std::function<JPH::Ref<JPH::Shape>()> createFunc)
	{
		// First check in-memory cache with shared lock
		{
			std::shared_lock<std::shared_mutex> readLock(s_ShapeCacheMutex);
			auto it = s_ShapeCache.find(cacheKey);
			if (it != s_ShapeCache.end())
			{
				return it->second;
			}
		}

		// Try to load from persistent cache (outside lock)
		JPH::Ref<JPH::Shape> persistentShape;
		if (s_PersistentCacheEnabled.load(std::memory_order_relaxed))
		{
			persistentShape = LoadShapeFromCache(cacheKey);
			if (persistentShape)
			{
				// Insert loaded shape with unique lock
				std::unique_lock<std::shared_mutex> writeLock(s_ShapeCacheMutex);
				// Check again in case another thread inserted while we were loading
				auto it = s_ShapeCache.find(cacheKey);
				if (it != s_ShapeCache.end())
				{
					return it->second; // Another thread beat us to it
				}
				s_ShapeCache[cacheKey] = persistentShape;
				return persistentShape;
			}
		}

		// Create new shape (outside lock to avoid holding during creation)
		auto shape = createFunc();
		if (shape)
		{
			// Insert with unique lock
			{
				std::unique_lock<std::shared_mutex> writeLock(s_ShapeCacheMutex);
				// Check again in case another thread inserted while we were creating
				auto it = s_ShapeCache.find(cacheKey);
				if (it != s_ShapeCache.end())
				{
					return it->second; // Another thread beat us to it
				}
				s_ShapeCache[cacheKey] = shape;
			}
			
			// Save to persistent cache (outside lock)
			if (s_PersistentCacheEnabled.load(std::memory_order_relaxed))
			{
				SaveShapeToCache(cacheKey, shape);
			}
		}
		
		return shape;
	}

	bool JoltShapes::SaveShapeToCache(const std::string& cacheKey, const JPH::Shape* shape)
	{
		if (!s_PersistentCacheEnabled.load(std::memory_order_relaxed) || !shape)
			return false;

		try
		{
			// Create cache file path
			std::filesystem::path cacheFilePath = s_PersistentCacheDirectory / (cacheKey + ".jsc"); // Jolt Shape Cache

			// Serialize shape to buffer
			Buffer buffer = JoltBinaryStreamUtils::SerializeShapeToBuffer(shape);
			if (!buffer.Data || buffer.Size == 0)
			{
				OLO_CORE_ERROR("JoltShapes::SaveShapeToCache: Failed to serialize shape for key: {}", cacheKey);
				return false;
			}

			// Write buffer to file
			std::ofstream file(cacheFilePath, std::ios::binary);
			if (!file.is_open())
			{
				OLO_CORE_ERROR("JoltShapes::SaveShapeToCache: Failed to open cache file: {}", cacheFilePath.string());
				buffer.Release();
				return false;
			}

			file.write(reinterpret_cast<const char*>(buffer.Data), buffer.Size);
			file.close();
			buffer.Release();

			return true;
		}
		catch (const std::exception& e)
		{
			OLO_CORE_ERROR("JoltShapes::SaveShapeToCache: Exception: {}", e.what());
			return false;
		}
	}

	JPH::Ref<JPH::Shape> JoltShapes::LoadShapeFromCache(const std::string& cacheKey)
	{
		if (!s_PersistentCacheEnabled.load(std::memory_order_relaxed))
			return nullptr;

		try
		{
			// Create cache file path
			std::filesystem::path cacheFilePath = s_PersistentCacheDirectory / (cacheKey + ".jsc");

			if (!std::filesystem::exists(cacheFilePath))
			{
				return nullptr; // No cached version exists
			}

			// Read file into buffer
			std::ifstream file(cacheFilePath, std::ios::binary | std::ios::ate);
			if (!file.is_open())
			{
				return nullptr;
			}

			std::streamsize size = file.tellg();
			file.seekg(0, std::ios::beg);

			// Validate file size before casting to u64
			if (size < 0)
			{
				OLO_CORE_ERROR("JoltShapes::LoadShapeFromCache: Invalid file size {} for cache file: {}", size, cacheFilePath.string());
				return nullptr;
			}

			// Check if size exceeds u64 maximum (unlikely but safe)
			if (static_cast<u64>(size) > std::numeric_limits<u64>::max())
			{
				OLO_CORE_ERROR("JoltShapes::LoadShapeFromCache: File size {} exceeds maximum supported size for cache file: {}", size, cacheFilePath.string());
				return nullptr;
			}

			Buffer buffer(static_cast<u64>(size));
			if (!file.read(reinterpret_cast<char*>(buffer.Data), size))
			{
				buffer.Release();
				return nullptr;
			}
			file.close();

			// Deserialize shape from buffer
			JPH::Ref<JPH::Shape> shape = JoltBinaryStreamUtils::DeserializeShapeFromBuffer(buffer);
			buffer.Release();

			if (!shape)
			{
				OLO_CORE_WARN("JoltShapes::LoadShapeFromCache: Failed to deserialize shape for key: {}", cacheKey);
				// Delete corrupted cache file
				std::filesystem::remove(cacheFilePath);
			}

			return shape;
		}
		catch (const std::exception& e)
		{
			OLO_CORE_ERROR("JoltShapes::LoadShapeFromCache: Exception: {}", e.what());
			return nullptr;
		}
	}

	void JoltShapes::ClearPersistentCache()
	{
		if (!s_PersistentCacheEnabled.load(std::memory_order_relaxed))
			return;

		try
		{
			if (std::filesystem::exists(s_PersistentCacheDirectory))
			{
				for (const auto& entry : std::filesystem::directory_iterator(s_PersistentCacheDirectory))
				{
					if (entry.path().extension() == ".jsc")
					{
						std::filesystem::remove(entry.path());
					}
				}
			}
			OLO_CORE_INFO("Cleared JoltShapes persistent cache");
		}
		catch (const std::exception& e)
		{
			OLO_CORE_ERROR("JoltShapes::ClearPersistentCache: Exception: {}", e.what());
		}
	}

}
