#pragma once

#include "Physics3DTypes.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/MeshColliderAsset.h"
#include "OloEngine/Renderer/Mesh.h"

#include <filesystem>
#include <vector>
#include <memory>

// Jolt forward declarations
namespace JPH {
	class Shape;
	template<typename T> class Ref;
}

namespace OloEngine {

	// Forward declarations
	struct Submesh;
	struct Vertex;

	enum class ECookingResult : u8
	{
		Success = 0,
		Failed,
		Cancelled,
		SourceDataInvalid,
		OutputInvalid,
		AlreadyExists
	};

	enum class EMeshColliderType : u8
	{
		Triangle = 0,   // Static triangle mesh for precise collision
		Convex = 1,     // Convex hull for dynamic bodies
		None = 2        // Invalid/no collision
	};

	struct SubmeshColliderData
	{
		std::vector<u8> m_ColliderData;  // Serialized Jolt shape data
		glm::mat4 m_Transform = glm::mat4(1.0f);
		EMeshColliderType m_Type = EMeshColliderType::Triangle;
		sizet m_VertexCount = 0;
		sizet m_IndexCount = 0;
	};

	struct MeshColliderData
	{
		std::vector<SubmeshColliderData> m_Submeshes;
		EMeshColliderType m_Type = EMeshColliderType::Triangle;
		glm::vec3 m_Scale = glm::vec3(1.0f);
		bool m_IsValid = false;
	};

	struct CachedColliderData
	{
		MeshColliderData m_SimpleColliderData;   // For dynamic bodies (convex)
		MeshColliderData m_ComplexColliderData;  // For static bodies (triangle mesh)
		std::chrono::system_clock::time_point m_LastAccessed; // Time when cached data was last loaded/accessed (for LRU eviction)
		bool m_IsValid = false;
	};

	// .omc file format (OloEngine Mesh Collider)
	struct OloMeshColliderHeader
	{
		char m_Header[8] = {'O','l','o','M','e','s','h','C'};
		u32 m_Version = 1;
		EMeshColliderType m_Type = EMeshColliderType::Triangle;
		u32 m_SubmeshCount = 0;
		glm::vec3 m_Scale = glm::vec3(1.0f);
		f32 m_Reserved[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // For future use
	};

	class MeshCookingFactory : public RefCounted
	{
	public:
		MeshCookingFactory();
		~MeshCookingFactory();

		// Initialization
		void Initialize();
		void Shutdown();
		bool IsInitialized() const { return m_Initialized; }

		// Main cooking interface
		std::pair<ECookingResult, ECookingResult> CookMesh(Ref<MeshColliderAsset> colliderAsset, bool invalidateOld = false);
		ECookingResult CookMeshType(Ref<MeshColliderAsset> colliderAsset, EMeshColliderType type, bool invalidateOld = false);

		// Mesh data extraction and cooking
		ECookingResult CookTriangleMesh(const std::vector<glm::vec3>& vertices, const std::vector<u32>& indices, 
										const glm::mat4& transform, SubmeshColliderData& outData);
		ECookingResult CookConvexMesh(const std::vector<glm::vec3>& vertices, const std::vector<u32>& indices,
									  const glm::mat4& transform, SubmeshColliderData& outData);

		// Convex hull generation
		ECookingResult GenerateConvexHull(const std::vector<glm::vec3>& vertices, std::vector<glm::vec3>& outHullVertices);

		// Mesh simplification for convex hulls
		ECookingResult SimplifyMeshForConvex(const std::vector<glm::vec3>& vertices, const std::vector<u32>& indices,
											 std::vector<glm::vec3>& outVertices, f32 simplificationRatio = 0.1f);

		// Validation
		bool ValidateMeshData(const std::vector<glm::vec3>& vertices, const std::vector<u32>& indices);
		bool ValidateConvexHull(const std::vector<glm::vec3>& vertices);

		// Serialization
		bool SerializeMeshCollider(const std::filesystem::path& filepath, const MeshColliderData& meshData);
		MeshColliderData DeserializeMeshCollider(const std::filesystem::path& filepath);

		// Shape reconstruction from cached data
		JPH::Ref<JPH::Shape> CreateShapeFromColliderData(const SubmeshColliderData& colliderData);
		bool CanCreateShapeFromColliderData(const SubmeshColliderData& colliderData) const;

		// Cache management
		std::filesystem::path GetCacheFilePath(Ref<MeshColliderAsset> colliderAsset, EMeshColliderType type);
		bool IsCacheValid(const std::filesystem::path& cacheFilePath, const std::filesystem::path& sourcePath);
		void ClearCache();

		// Settings
		void SetVertexWeldingEnabled(bool enabled) { m_VertexWeldingEnabled = enabled; }
		bool IsVertexWeldingEnabled() const { return m_VertexWeldingEnabled; }

		void SetVertexWeldTolerance(f32 tolerance) { m_VertexWeldTolerance = tolerance; }
		f32 GetVertexWeldTolerance() const { return m_VertexWeldTolerance; }

		void SetMaxConvexHullVertices(u32 maxVertices) { m_MaxConvexHullVertices = maxVertices; }
		u32 GetMaxConvexHullVertices() const { return m_MaxConvexHullVertices; }

		// Statistics
		sizet GetTriangleMeshCount() const { return m_TriangleMeshCount; }
		sizet GetConvexMeshCount() const { return m_ConvexMeshCount; }
		sizet GetCachedMeshCount() const { return m_CachedMeshCount; }

	private:
		// Internal mesh processing
		ECookingResult ProcessSubmesh(const Submesh& submesh, Ref<MeshSource> meshSource, const glm::mat4& transform, 
									  EMeshColliderType type, SubmeshColliderData& outData);
		
		// Vertex processing
		std::vector<glm::vec3> ExtractVertexPositions(const std::vector<glm::vec3>& vertices);
		void WeldVertices(std::vector<glm::vec3>& vertices, std::vector<u32>& indices, f32 tolerance);
		void RemoveDuplicateVertices(std::vector<glm::vec3>& vertices, std::vector<u32>& indices);

		// Triangle mesh optimization
		void OptimizeTriangleMesh(std::vector<glm::vec3>& vertices, std::vector<u32>& indices);
		void RemoveInvalidTriangles(std::vector<glm::vec3>& vertices, std::vector<u32>& indices, f32 areaEpsilon);

		// Cache path generation
		std::string GenerateCacheKey(Ref<MeshColliderAsset> colliderAsset, EMeshColliderType type);
		std::filesystem::path GetCacheDirectory();

		// Error handling
		void LogCookingError(const std::string& operation, const std::string& error);

	private:
		bool m_Initialized = false;

		// Cooking settings
		bool m_VertexWeldingEnabled = true;
		f32 m_VertexWeldTolerance = 0.001f;
		u32 m_MaxConvexHullVertices = 256;
		f32 m_AreaTestEpsilon = 0.0001f;
		f32 m_ConvexSimplificationRatio = 0.1f;

		// Statistics
		sizet m_TriangleMeshCount = 0;
		sizet m_ConvexMeshCount = 0;
		sizet m_CachedMeshCount = 0;

		// Cache directory
		std::filesystem::path m_CacheDirectory;

		// Constants
		static constexpr u32 MaxTrianglesPerMesh = 65536;
		static constexpr u32 MaxVerticesPerMesh = 32768;
		static constexpr u32 MinVerticesForConvexHull = 4;
		static constexpr f32 MinTriangleArea = 1e-6f;
	};

}