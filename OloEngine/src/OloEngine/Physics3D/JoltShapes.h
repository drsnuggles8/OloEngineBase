#pragma once

// @file JoltShapes.h
// @brief Utility functions for creating and converting Jolt collision shapes for the 3D physics system.
//
// This header defines the JoltShapes class which provides static utility functions to convert engine
// component data (BoxCollider3DComponent, SphereCollider3DComponent, etc.) into Jolt Physics shape objects.
// It serves as the primary interface for mapping high-level engine collider components to low-level
// Jolt shape representations used by the physics simulation.
//
// Key Usage Notes:
// - Thread Safety: Shape creation functions are thread-safe. Shape caching uses shared_mutex for concurrent reads.
// - Memory Management: Returns JPH::Ref<JPH::Shape> smart pointers - Jolt handles lifetime automatically.
// - Coordinate Space: Expects engine world coordinates; shapes are created in Jolt's coordinate system.
// - Scaling: Scale parameters are applied during shape creation and baked into the final shape geometry.
// - Validation: All shape parameters are validated against s_MinShapeSize/s_MaxShapeSize bounds.

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
#include <filesystem>
#include "OloEngine/Threading/Mutex.h"
#include "OloEngine/Threading/SharedMutex.h"
#include "OloEngine/Threading/UniqueLock.h"
#include "OloEngine/Threading/SharedLock.h"
#include <utility>
#include <atomic>

namespace OloEngine
{

    class JoltShapes
    {
      public:
        static void Initialize();
        static void Initialize(const std::filesystem::path& cacheDirectory);
        static void Shutdown();

        // Cache directory configuration
        static void SetPersistentCacheDirectory(const std::filesystem::path& directory);
        static std::filesystem::path GetPersistentCacheDirectory();

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

        // Persistent shape caching using binary streams
        static JPH::Ref<JPH::Shape> GetOrCreatePersistentCachedShape(const std::string& cacheKey, std::function<JPH::Ref<JPH::Shape>()> createFunc);
        static bool SaveShapeToCache(const std::string& cacheKey, const JPH::Shape* shape);
        static JPH::Ref<JPH::Shape> LoadShapeFromCache(const std::string& cacheKey);
        static void ClearPersistentCache();
        static bool IsPersistentCacheEnabled()
        {
            return s_PersistentCacheEnabled.load(std::memory_order_acquire);
        }
        static void SetPersistentCacheEnabled(bool enabled)
        {
            s_PersistentCacheEnabled.store(enabled, std::memory_order_release);
        }

        // Helper functions
        static glm::vec3 CalculateShapeLocalCenterOfMass(Entity entity);
        static f32 CalculateShapeVolume(const JPH::Shape* shape);
        static bool IsShapeValid(const JPH::Shape* shape);

        // Shape type utilities
        static ShapeType GetShapeType(const JPH::Shape* shape);
        static const char* GetShapeTypeName(const JPH::Shape* shape);

        // Delete constructors and assignment operators to prevent instantiation
        JoltShapes() = delete;
        ~JoltShapes() = delete;
        JoltShapes(const JoltShapes&) = delete;
        JoltShapes(JoltShapes&&) = delete;
        JoltShapes& operator=(const JoltShapes&) = delete;
        JoltShapes& operator=(JoltShapes&&) = delete;

      private:
        // Helper struct for collecting collider shapes
        struct CollectedShape
        {
            JPH::Ref<JPH::Shape> shape;
            glm::vec3 offset;
        };

        // Collects all collider shapes for an entity with proper scaling
        static std::vector<CollectedShape> CollectColliderShapesForEntity(Entity entity, const glm::vec3& entityScale);

        // Configuration helpers
        static std::filesystem::path GetDefaultCacheDirectory();

        // Internal shape creation helpers
        static JPH::Ref<JPH::Shape> CreateBoxShapeInternal(const glm::vec3& halfExtents);
        static JPH::Ref<JPH::Shape> CreateSphereShapeInternal(f32 radius);
        static JPH::Ref<JPH::Shape> CreateCapsuleShapeInternal(f32 radius, f32 halfHeight);
        static JPH::Ref<JPH::Shape> CreateMeshShapeInternal(AssetHandle meshAsset, bool useComplexAsSimple, const glm::vec3& scale);
        static JPH::Ref<JPH::Shape> CreateConvexMeshShapeInternal(AssetHandle meshAsset, f32 convexRadius, const glm::vec3& scale);
        static JPH::Ref<JPH::Shape> CreateTriangleMeshShapeInternal(AssetHandle meshAsset, const glm::vec3& scale);

        // Common helper for mesh shape creation from cached data
        static JPH::Ref<JPH::Shape> CreateMeshShapeFromCachedData(AssetHandle meshAsset, const SubmeshColliderData& submeshData, const glm::vec3& scale, const std::string& shapeTypeName);

        // Shape validation helpers
        static bool ValidateBoxDimensions(const glm::vec3& halfExtents);
        static bool ValidateSphereDimensions(f32 radius);
        static bool ValidateCapsuleDimensions(f32 radius, f32 halfHeight);
        static bool ValidateMeshAsset(AssetHandle meshAsset);

        // Scaling helpers
        static glm::vec3 ApplyScaleToBoxExtents(const glm::vec3& halfExtents, const glm::vec3& scale);
        static f32 ApplyScaleToSphereRadius(f32 radius, const glm::vec3& scale);
        static std::pair<f32, f32> ApplyScaleToCapsule(f32 radius, f32 halfHeight, const glm::vec3& scale);

        // Volume calculation helpers
        static f32 ComputeMeshVolume(AssetHandle colliderAsset, const glm::vec3& scale = glm::vec3(1.0f));

      private:
        static std::atomic<bool> s_Initialized;
        static FMutex s_InitializationMutex;
        static std::unordered_map<std::string, JPH::Ref<JPH::Shape>> s_ShapeCache;
        static FSharedMutex s_ShapeCacheMutex;
        static std::atomic<bool> s_PersistentCacheEnabled;
        static std::filesystem::path s_PersistentCacheDirectory;
        static FSharedMutex s_PersistentCacheDirectoryMutex;

        // Constants for shape validation
        static constexpr f32 s_MinShapeSize = 0.001f;
        static constexpr f32 s_MaxShapeSize = 10000.0f;
    };

} // namespace OloEngine
