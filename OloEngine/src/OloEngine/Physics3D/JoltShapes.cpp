#include "OloEnginePCH.h"
#include "JoltShapes.h"
#include "JoltBinaryStream.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Asset/AssetManager.h"
#include "MeshColliderCache.h"

#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/MutableCompoundShape.h>

namespace OloEngine {

	bool JoltShapes::s_Initialized = false;
	std::unordered_map<std::string, JPH::Ref<JPH::Shape>> JoltShapes::s_ShapeCache;
	bool JoltShapes::s_PersistentCacheEnabled = true;
	std::filesystem::path JoltShapes::s_PersistentCacheDirectory = "assets/cache/shapes";

	void JoltShapes::Initialize()
	{
		if (s_Initialized)
			return;

		OLO_CORE_INFO("Initializing JoltShapes system");
		s_ShapeCache.clear();
		
		// Initialize mesh collider cache
		MeshColliderCache::GetInstance().Initialize();
		
		// Create persistent cache directory if enabled
		if (s_PersistentCacheEnabled)
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
				s_PersistentCacheEnabled = false;
			}
		}
		
		s_Initialized = true;
	}

	void JoltShapes::Shutdown()
	{
		if (!s_Initialized)
			return;

		OLO_CORE_INFO("Shutting down JoltShapes system");
		
		// Shutdown mesh collider cache
		MeshColliderCache::GetInstance().Shutdown();
		
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

	JPH::Ref<JPH::Shape> JoltShapes::CreateMeshShape(const MeshCollider3DComponent& component, const glm::vec3& scale)
	{
		if (!ValidateMeshAsset(component.ColliderAsset))
		{
			OLO_CORE_ERROR("Invalid mesh collider asset handle: {0}", component.ColliderAsset);
			return nullptr;
		}

		return CreateMeshShapeInternal(component.ColliderAsset, component.UseComplexAsSimple, scale);
	}

	JPH::Ref<JPH::Shape> JoltShapes::CreateConvexMeshShape(const ConvexMeshCollider3DComponent& component, const glm::vec3& scale)
	{
		if (!ValidateMeshAsset(component.ColliderAsset))
		{
			OLO_CORE_ERROR("Invalid convex mesh collider asset handle: {0}", component.ColliderAsset);
			return nullptr;
		}

		return CreateConvexMeshShapeInternal(component.ColliderAsset, component.ConvexRadius, scale);
	}

	JPH::Ref<JPH::Shape> JoltShapes::CreateTriangleMeshShape(const TriangleMeshCollider3DComponent& component, const glm::vec3& scale)
	{
		if (!ValidateMeshAsset(component.ColliderAsset))
		{
			OLO_CORE_ERROR("Invalid triangle mesh collider asset handle: {0}", component.ColliderAsset);
			return nullptr;
		}

		return CreateTriangleMeshShapeInternal(component.ColliderAsset, scale);
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

		if (entity.HasComponent<MeshCollider3DComponent>())
		{
			const auto& component = entity.GetComponent<MeshCollider3DComponent>();
			auto shape = CreateMeshShape(component);
			if (shape)
			{
				shapes.push_back(shape);
				offsets.push_back(component.Offset);
			}
		}

		if (entity.HasComponent<ConvexMeshCollider3DComponent>())
		{
			const auto& component = entity.GetComponent<ConvexMeshCollider3DComponent>();
			auto shape = CreateConvexMeshShape(component);
			if (shape)
			{
				shapes.push_back(shape);
				offsets.push_back(component.Offset);
			}
		}

		if (entity.HasComponent<TriangleMeshCollider3DComponent>())
		{
			const auto& component = entity.GetComponent<TriangleMeshCollider3DComponent>();
			auto shape = CreateTriangleMeshShape(component);
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

		// Check for mesh collider
		if (entity.HasComponent<MeshCollider3DComponent>())
		{
			const auto& component = entity.GetComponent<MeshCollider3DComponent>();
			auto shape = CreateMeshShape(component);
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
			auto shape = CreateConvexMeshShape(component);
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
			auto shape = CreateTriangleMeshShape(component);
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
		if (!cachedData.IsValid)
		{
			OLO_CORE_ERROR("Failed to get valid cached mesh data for asset {0}", meshAsset);
			return nullptr;
		}

		// Choose between simple (convex) and complex (triangle) based on usage
		const MeshColliderData* meshData = nullptr;
		if (useComplexAsSimple || cachedData.ComplexColliderData.Submeshes.empty())
		{
			// Use convex shape for dynamic bodies or if no complex data
			if (cachedData.SimpleColliderData.Submeshes.empty())
			{
				OLO_CORE_ERROR("No simple (convex) mesh data available for asset {0}", meshAsset);
				return nullptr;
			}
			meshData = &cachedData.SimpleColliderData;
		}
		else
		{
			// Use triangle mesh for static bodies
			meshData = &cachedData.ComplexColliderData;
		}

		// For now, just use the first submesh - could be extended to support multiple submeshes
		if (meshData->Submeshes.empty())
		{
			OLO_CORE_ERROR("No submesh data available for asset {0}", meshAsset);
			return nullptr;
		}

		const auto& submesh = meshData->Submeshes[0];
		
		// TODO: Deserialize the Jolt shape from ColliderData
		// This requires implementing shape deserialization from the cached binary data
		OLO_CORE_WARN("Mesh shape deserialization not yet implemented for asset {0}", meshAsset);
		
		// For now, return a placeholder box shape
		return CreateBoxShapeInternal(glm::vec3(1.0f));
	}

	JPH::Ref<JPH::Shape> JoltShapes::CreateConvexMeshShapeInternal(AssetHandle meshAsset, f32 convexRadius, const glm::vec3& scale)
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
		if (!cachedData.IsValid || cachedData.SimpleColliderData.Submeshes.empty())
		{
			OLO_CORE_ERROR("Failed to get valid convex mesh data for asset {0}", meshAsset);
			return nullptr;
		}

		// For now, just use the first submesh
		const auto& submesh = cachedData.SimpleColliderData.Submeshes[0];
		
		// TODO: Deserialize the convex Jolt shape from ColliderData
		// This requires implementing shape deserialization from the cached binary data
		OLO_CORE_WARN("Convex mesh shape deserialization not yet implemented for asset {0}", meshAsset);
		
		// For now, return a placeholder box shape
		return CreateBoxShapeInternal(glm::vec3(1.0f));
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
		if (!cachedData.IsValid || cachedData.ComplexColliderData.Submeshes.empty())
		{
			OLO_CORE_ERROR("Failed to get valid triangle mesh data for asset {0}", meshAsset);
			return nullptr;
		}

		// For now, just use the first submesh
		const auto& submesh = cachedData.ComplexColliderData.Submeshes[0];
		
		// TODO: Deserialize the triangle mesh Jolt shape from ColliderData
		// This requires implementing shape deserialization from the cached binary data
		OLO_CORE_WARN("Triangle mesh shape deserialization not yet implemented for asset {0}", meshAsset);
		
		// For now, return a placeholder box shape
		return CreateBoxShapeInternal(glm::vec3(1.0f));
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

	JPH::Ref<JPH::Shape> JoltShapes::GetOrCreatePersistentCachedShape(const std::string& cacheKey, std::function<JPH::Ref<JPH::Shape>()> createFunc)
	{
		// First check in-memory cache
		auto it = s_ShapeCache.find(cacheKey);
		if (it != s_ShapeCache.end())
		{
			return it->second;
		}

		// Try to load from persistent cache
		if (s_PersistentCacheEnabled)
		{
			auto shape = LoadShapeFromCache(cacheKey);
			if (shape)
			{
				s_ShapeCache[cacheKey] = shape;
				return shape;
			}
		}

		// Create new shape
		auto shape = createFunc();
		if (shape)
		{
			s_ShapeCache[cacheKey] = shape;
			
			// Save to persistent cache
			if (s_PersistentCacheEnabled)
			{
				SaveShapeToCache(cacheKey, shape);
			}
		}
		
		return shape;
	}

	bool JoltShapes::SaveShapeToCache(const std::string& cacheKey, const JPH::Shape* shape)
	{
		if (!s_PersistentCacheEnabled || !shape)
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
		if (!s_PersistentCacheEnabled)
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
		if (!s_PersistentCacheEnabled)
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