#include "OloEngine/Physics3D/MeshCookingFactory.h"
#include "OloEngine/Physics3D/JoltUtils.h"
#include "OloEngine/Physics3D/JoltBinaryStream.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshSource.h"

#include <climits>
#include <cstring>
#include <set>
#include <sstream>
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Core/Array.h>
#include <Jolt/Core/StreamIn.h>
#include <Jolt/Core/StreamOut.h>
#include <Jolt/Math/Float3.h>
#include <Jolt/Geometry/IndexedTriangle.h>
#include <Jolt/Geometry/ConvexHullBuilder.h>

#include <fstream>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>

namespace OloEngine
{

    MeshCookingFactory::MeshCookingFactory(const std::filesystem::path& cacheDirectory)
        : m_CacheDirectory(cacheDirectory)
    {
    }

    MeshCookingFactory::~MeshCookingFactory()
    {
        if (m_Initialized)
        {
            Shutdown();
        }
    }

    void MeshCookingFactory::Initialize()
    {
        if (m_Initialized)
        {
            OLO_CORE_WARN("MeshCookingFactory already initialized");
            return;
        }

        // Create cache directory if it doesn't exist
        try
        {
            if (!std::filesystem::exists(m_CacheDirectory))
            {
                std::filesystem::create_directories(m_CacheDirectory);
            }
            m_CacheAvailable = true;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("MeshCookingFactory: Exception during cache directory creation: {0}. Cache functionality will be disabled.", e.what());
            m_CacheAvailable = false;
        }

        m_Initialized = true;
        OLO_CORE_INFO("MeshCookingFactory initialized");
    }

    void MeshCookingFactory::Shutdown()
    {
        if (!m_Initialized)
        {
            return;
        }

        m_Initialized = false;
        OLO_CORE_INFO("MeshCookingFactory shutdown");
    }

    std::pair<ECookingResult, ECookingResult> MeshCookingFactory::CookMesh(Ref<MeshColliderAsset> colliderAsset, bool invalidateOld)
    {
        if (!m_Initialized)
        {
            OLO_CORE_ERROR("MeshCookingFactory not initialized");
            return { ECookingResult::Failed, ECookingResult::Failed };
        }

        if (!colliderAsset)
        {
            OLO_CORE_ERROR("Invalid mesh collider asset");
            return { ECookingResult::SourceDataInvalid, ECookingResult::SourceDataInvalid };
        }

        // Cook both simple (convex) and complex (triangle) versions
        ECookingResult simpleResult = CookMeshType(colliderAsset, EMeshColliderType::Convex, invalidateOld);
        ECookingResult complexResult = CookMeshType(colliderAsset, EMeshColliderType::Triangle, invalidateOld);

        return { simpleResult, complexResult };
    }

    ECookingResult MeshCookingFactory::CookMeshType(Ref<MeshColliderAsset> colliderAsset, EMeshColliderType type, bool invalidateOld)
    {
        if (!colliderAsset)
        {
            return ECookingResult::SourceDataInvalid;
        }

        // Get the source mesh
        auto meshAsset = AssetManager::GetAsset<Mesh>(colliderAsset->m_ColliderMesh);
        if (!meshAsset)
        {
            LogCookingError("CookMeshType", "Failed to load source mesh asset");
            return ECookingResult::SourceDataInvalid;
        }

        // Check cache (only if cache is available)
        std::filesystem::path cacheFilePath;
        if (m_CacheAvailable)
        {
            cacheFilePath = GetCacheFilePath(colliderAsset, type);
            if (!invalidateOld && std::filesystem::exists(cacheFilePath))
            {
                // TODO: Add timestamp checking against source mesh
                return ECookingResult::AlreadyExists;
            }
        }

        // Create collider data
        MeshColliderData colliderData;
        colliderData.m_Type = type;
        colliderData.m_Scale = colliderAsset->m_ColliderScale;

        // Process each submesh
        auto meshSource = meshAsset->GetMeshSource();
        if (!meshSource)
        {
            LogCookingError("CookMeshType", "MeshAsset has no valid MeshSource");
            return ECookingResult::SourceDataInvalid;
        }

        const auto& submeshes = meshSource->GetSubmeshes();
        for (i32 i = 0; i < submeshes.Num(); ++i)
        {
            const auto& submesh = submeshes[i];

            SubmeshColliderData submeshData;
            ECookingResult result = ProcessSubmesh(submesh, meshSource, glm::mat4(1.0f), type, submeshData);

            if (result != ECookingResult::Success)
            {
                LogCookingError("CookMeshType", fmt::format("Failed to process submesh {}", i));
                return result;
            }

            colliderData.m_Submeshes.push_back(submeshData);
        }

        colliderData.m_IsValid = !colliderData.m_Submeshes.empty();

        // Serialize to cache (only if cache is available)
        if (m_CacheAvailable && !cacheFilePath.empty())
        {
            if (!SerializeMeshCollider(cacheFilePath, colliderData))
            {
                LogCookingError("CookMeshType", "Failed to serialize mesh collider to cache");
                // Don't return an error here - the cooking succeeded, just cache write failed
                OLO_CORE_WARN("MeshCookingFactory: Cache write failed but cooking succeeded, continuing without cache");
            }
        }

        // Update statistics
        if (type == EMeshColliderType::Triangle)
        {
            m_TriangleMeshCount++;
        }
        else if (type == EMeshColliderType::Convex)
        {
            m_ConvexMeshCount++;
        }

        OLO_CORE_INFO("Successfully cooked {} mesh collider", type == EMeshColliderType::Triangle ? "triangle" : "convex");
        return ECookingResult::Success;
    }

    ECookingResult MeshCookingFactory::ProcessSubmesh(const Submesh& submesh, Ref<MeshSource> meshSource, const glm::mat4& transform,
                                                      EMeshColliderType type, SubmeshColliderData& outData)
    {
        // Get mesh data from the MeshSource using submesh indices
        const auto& allVertices = meshSource->GetVertices();
        const auto& allIndices = meshSource->GetIndices();

        const auto vertexCount = static_cast<u32>(allVertices.Num());
        const auto indexCount = static_cast<u32>(allIndices.Num());

        // Validate submesh vertex range bounds
        if (submesh.m_BaseVertex >= vertexCount)
        {
            LogCookingError("ProcessSubmesh", "Submesh BaseVertex (" + std::to_string(submesh.m_BaseVertex) +
                                                  ") is beyond vertices size (" + std::to_string(vertexCount) + ")");
            return ECookingResult::SourceDataInvalid;
        }

        if (submesh.m_BaseVertex + submesh.m_VertexCount > vertexCount)
        {
            LogCookingError("ProcessSubmesh", "Submesh vertex range [" + std::to_string(submesh.m_BaseVertex) +
                                                  ", " + std::to_string(submesh.m_BaseVertex + submesh.m_VertexCount) +
                                                  ") exceeds vertices size (" + std::to_string(vertexCount) + ")");
            return ECookingResult::SourceDataInvalid;
        }

        // Validate submesh index range bounds
        if (submesh.m_BaseIndex >= indexCount)
        {
            LogCookingError("ProcessSubmesh", "Submesh BaseIndex (" + std::to_string(submesh.m_BaseIndex) +
                                                  ") is beyond indices size (" + std::to_string(indexCount) + ")");
            return ECookingResult::SourceDataInvalid;
        }

        if (submesh.m_BaseIndex + submesh.m_IndexCount > indexCount)
        {
            LogCookingError("ProcessSubmesh", "Submesh index range [" + std::to_string(submesh.m_BaseIndex) +
                                                  ", " + std::to_string(submesh.m_BaseIndex + submesh.m_IndexCount) +
                                                  ") exceeds indices size (" + std::to_string(indexCount) + ")");
            return ECookingResult::SourceDataInvalid;
        }

        // Extract vertices and indices for this specific submesh
        std::vector<glm::vec3> submeshVertices;
        std::vector<u32> submeshIndices;

        // Extract vertices from BaseVertex to BaseVertex + VertexCount
        for (u32 i = 0; i < submesh.m_VertexCount; ++i)
        {
            const Vertex& vertex = allVertices[submesh.m_BaseVertex + i];
            submeshVertices.push_back(vertex.Position);
        }

        // Extract indices and adjust them relative to submesh base
        for (u32 i = 0; i < submesh.m_IndexCount; ++i)
        {
            u32 originalIndex = allIndices[submesh.m_BaseIndex + i];

            // Validate index is within submesh vertex range to prevent underflow
            if (originalIndex < submesh.m_BaseVertex)
            {
                LogCookingError("ProcessSubmesh", "Index " + std::to_string(originalIndex) +
                                                      " at position " + std::to_string(i) + " is below BaseVertex (" +
                                                      std::to_string(submesh.m_BaseVertex) + "), would cause underflow");
                return ECookingResult::SourceDataInvalid;
            }

            // Indices are relative to BaseVertex, so subtract BaseVertex to make them 0-based for this submesh
            submeshIndices.push_back(originalIndex - submesh.m_BaseVertex);
        }

        // Validate extracted data
        if (submeshVertices.empty() || submeshIndices.empty())
        {
            LogCookingError("ProcessSubmesh", "No valid vertices or indices found in submesh");
            return ECookingResult::SourceDataInvalid;
        }

        outData.m_Transform = transform;
        outData.m_Type = type;
        outData.m_VertexCount = submeshVertices.size();
        outData.m_IndexCount = submeshIndices.size();

        // Cook based on type
        if (type == EMeshColliderType::Triangle)
        {
            return CookTriangleMesh(submeshVertices, submeshIndices, transform, outData);
        }
        else if (type == EMeshColliderType::Convex)
        {
            return CookConvexMesh(submeshVertices, submeshIndices, transform, outData);
        }

        return ECookingResult::Failed;
    }

    ECookingResult MeshCookingFactory::CookTriangleMesh(const std::vector<glm::vec3>& vertices, const std::vector<u32>& indices,
                                                        const glm::mat4& transform, SubmeshColliderData& outData)
    {
        // Validate input mesh data early
        if (!ValidateMeshData(vertices, indices))
        {
            LogCookingError("CookTriangleMesh", "Invalid input mesh data");
            return ECookingResult::SourceDataInvalid;
        }

        try
        {
            // Use the vertex positions directly
            std::vector<glm::vec3> positions = vertices;
            std::vector<u32> triangleIndices = indices;

            // Apply vertex welding if enabled
            if (m_VertexWeldingEnabled)
            {
                WeldVertices(positions, triangleIndices, m_VertexWeldTolerance);
            }

            // Remove invalid triangles
            RemoveInvalidTriangles(positions, triangleIndices, m_AreaTestEpsilon);

            // Check if any valid triangles remain after cleanup
            if (triangleIndices.empty())
            {
                LogCookingError("CookTriangleMesh", "No valid triangles remain after processing");
                return ECookingResult::SourceDataInvalid;
            }

            // Optimize triangle mesh
            OptimizeTriangleMesh(positions, triangleIndices);

            // Convert to Jolt format
            ::JPH::Array<::JPH::Float3> joltVertices;
            joltVertices.reserve(positions.size());
            for (const auto& pos : positions)
            {
                joltVertices.push_back(::JPH::Float3(pos.x, pos.y, pos.z));
            }

            ::JPH::Array<::JPH::IndexedTriangle> joltTriangles;
            joltTriangles.reserve(triangleIndices.size() / 3);
            for (sizet i = 0; i < triangleIndices.size(); i += 3)
            {
                joltTriangles.push_back(::JPH::IndexedTriangle(triangleIndices[i], triangleIndices[i + 1], triangleIndices[i + 2]));
            }

            // Create Jolt mesh shape
            ::JPH::MeshShapeSettings meshSettings(joltVertices, joltTriangles);
            ::JPH::ShapeSettings::ShapeResult result = meshSettings.Create();

            if (result.HasError())
            {
                LogCookingError("CookTriangleMesh", result.GetError().c_str());
                return ECookingResult::Failed;
            }

            // Serialize the shape using JoltBinaryStream
            JoltBinaryStreamWriter writer;
            if (!JoltBinaryStreamUtils::SerializeShape(result.Get(), writer))
            {
                LogCookingError("CookTriangleMesh", "Failed to serialize triangle mesh shape");
                return ECookingResult::Failed;
            }

            // Store the serialized data
            const auto& serializedData = writer.GetData();
            outData.m_ColliderData.assign(serializedData.begin(), serializedData.end());
            outData.m_Type = EMeshColliderType::Triangle;
            outData.m_Transform = transform;
            outData.m_VertexCount = positions.size();
            outData.m_IndexCount = triangleIndices.size();

            return ECookingResult::Success;
        }
        catch (const std::exception& e)
        {
            LogCookingError("CookTriangleMesh", e.what());
            return ECookingResult::Failed;
        }
    }

    ECookingResult MeshCookingFactory::CookConvexMesh(const std::vector<glm::vec3>& vertices, const std::vector<u32>& indices,
                                                      const glm::mat4& transform, SubmeshColliderData& outData)
    {
        // Validate input mesh data early
        if (!ValidateMeshData(vertices, indices))
        {
            LogCookingError("CookConvexMesh", "Invalid input mesh data");
            return ECookingResult::SourceDataInvalid;
        }

        try
        {
            // Simplify vertices for convex hull (already have positions)
            std::vector<glm::vec3> hullVertices;
            ECookingResult simplifyResult = SimplifyMeshForConvex(vertices, indices, hullVertices, m_ConvexSimplificationRatio);

            if (simplifyResult != ECookingResult::Success)
            {
                return simplifyResult;
            }

            // Check if any valid vertices remain after simplification
            if (hullVertices.empty())
            {
                LogCookingError("CookConvexMesh", "No valid vertices remain after simplification");
                return ECookingResult::SourceDataInvalid;
            }

            // Validate that simplified mesh has enough vertices for convex hull
            if (hullVertices.size() < s_MinVerticesForConvexHull)
            {
                LogCookingError("CookConvexMesh", "Insufficient vertices after simplification for convex hull");
                return ECookingResult::SourceDataInvalid;
            }

            // Generate convex hull
            std::vector<glm::vec3> finalHullVertices;
            ECookingResult hullResult = GenerateConvexHull(hullVertices, finalHullVertices);

            if (hullResult != ECookingResult::Success)
            {
                return hullResult;
            }

            // Validate that convex hull generation produced enough vertices
            if (finalHullVertices.size() < s_MinVerticesForConvexHull)
            {
                LogCookingError("CookConvexMesh", "Convex hull generation produced insufficient vertices");
                return ECookingResult::SourceDataInvalid;
            }

            // Validate convex hull
            if (!ValidateConvexHull(finalHullVertices))
            {
                return ECookingResult::Failed;
            }

            // Convert to Jolt format
            ::JPH::Array<::JPH::Vec3> joltVertices;
            joltVertices.reserve(finalHullVertices.size());
            for (const auto& vertex : finalHullVertices)
            {
                joltVertices.push_back(JoltUtils::ToJoltVector(vertex));
            }

            // Create Jolt convex hull shape
            ::JPH::ConvexHullShapeSettings convexSettings(joltVertices);
            convexSettings.mMaxConvexRadius = m_MaxConvexRadius; // Configurable convex radius

            ::JPH::ShapeSettings::ShapeResult result = convexSettings.Create();

            if (result.HasError())
            {
                LogCookingError("CookConvexMesh", result.GetError().c_str());
                return ECookingResult::Failed;
            }

            // Serialize the shape using JoltBinaryStream
            JoltBinaryStreamWriter writer;
            if (!JoltBinaryStreamUtils::SerializeShape(result.Get(), writer))
            {
                LogCookingError("CookConvexMesh", "Failed to serialize convex mesh shape");
                return ECookingResult::Failed;
            }

            // Store the serialized data
            const auto& serializedData = writer.GetData();
            outData.m_ColliderData.assign(serializedData.begin(), serializedData.end());
            outData.m_Type = EMeshColliderType::Convex;
            outData.m_Transform = transform;
            outData.m_VertexCount = finalHullVertices.size();
            outData.m_IndexCount = 0; // Convex hulls don't use explicit indices

            return ECookingResult::Success;
        }
        catch (const std::exception& e)
        {
            LogCookingError("CookConvexMesh", e.what());
            return ECookingResult::Failed;
        }
    }

    ECookingResult MeshCookingFactory::GenerateConvexHull(const std::vector<glm::vec3>& vertices, std::vector<glm::vec3>& outHullVertices)
    {
        if (vertices.size() < s_MinVerticesForConvexHull)
        {
            return ECookingResult::SourceDataInvalid;
        }

        try
        {
            // Convert to Jolt format
            JPH::Array<JPH::Vec3> joltVertices;
            joltVertices.reserve(vertices.size());
            for (const auto& vertex : vertices)
            {
                joltVertices.push_back(JoltUtils::ToJoltVector(vertex));
            }

            // Use Jolt's convex hull builder
            JPH::ConvexHullBuilder builder(joltVertices);

            const char* error = nullptr;
            JPH::ConvexHullBuilder::EResult result = builder.Initialize(i32_max, 1e-5f, error);

            if (result != JPH::ConvexHullBuilder::EResult::Success)
            {
                std::string errorMsg = error ? error : "Unknown error";
                LogCookingError("GenerateConvexHull", "Convex hull generation failed: " + errorMsg);
                return ECookingResult::Failed;
            }

            // Extract hull vertices by collecting unique vertex indices from faces
            std::set<int> usedVertices;
            const auto& faces = builder.GetFaces();

            for (const auto* face : faces)
            {
                if (!face->mRemoved)
                {
                    // Iterate through all edges of this face to collect vertex indices
                    auto* edge = face->mFirstEdge;
                    do
                    {
                        usedVertices.insert(edge->mStartIdx);
                        edge = edge->mNextEdge;
                    } while (edge != face->mFirstEdge);
                }
            }

            // Convert indices to actual vertex positions
            outHullVertices.clear();
            outHullVertices.reserve(usedVertices.size());

            for (int index : usedVertices)
            {
                outHullVertices.push_back(JoltUtils::FromJoltVector(joltVertices[index]));
            }

            // Limit vertex count with proper convex hull reconstruction
            if (outHullVertices.size() > m_MaxConvexHullVertices)
            {
                OLO_CORE_WARN("GenerateConvexHull: Hull has {0} vertices, exceeds limit of {1}. Attempting to reduce...",
                              outHullVertices.size(), m_MaxConvexHullVertices);

                // Try to select the most important vertices (extremes in each direction)
                std::vector<glm::vec3> reducedVertices;
                if (!ReduceConvexHullVertices(outHullVertices, m_MaxConvexHullVertices, reducedVertices))
                {
                    LogCookingError("GenerateConvexHull", "Failed to reduce vertex count while maintaining valid convex hull");
                    return ECookingResult::Failed;
                }

                // Validate the reduced hull
                if (!ValidateConvexHull(reducedVertices))
                {
                    LogCookingError("GenerateConvexHull", "Reduced convex hull failed validation");
                    return ECookingResult::Failed;
                }

                outHullVertices = std::move(reducedVertices);
                OLO_CORE_INFO("GenerateConvexHull: Successfully reduced hull to {0} vertices", outHullVertices.size());
            }

            return ECookingResult::Success;
        }
        catch (const std::exception& e)
        {
            LogCookingError("GenerateConvexHull", e.what());
            return ECookingResult::Failed;
        }
    }

    ECookingResult MeshCookingFactory::SimplifyMeshForConvex(const std::vector<glm::vec3>& vertices, const std::vector<u32>& indices,
                                                             std::vector<glm::vec3>& outVertices, f32 simplificationRatio)
    {
        // Use vertex positions directly
        std::vector<glm::vec3> positions = vertices;

        // Remove duplicates
        std::vector<u32> dummyIndices = indices;
        RemoveDuplicateVertices(positions, dummyIndices);

        // Simple decimation: keep every nth vertex based on simplification ratio
        sizet targetVertexCount = static_cast<sizet>(positions.size() * simplificationRatio);
        targetVertexCount = std::max(targetVertexCount, static_cast<sizet>(s_MinVerticesForConvexHull));
        targetVertexCount = std::min(targetVertexCount, static_cast<sizet>(m_MaxConvexHullVertices));

        if (positions.size() <= targetVertexCount)
        {
            outVertices = positions;
        }
        else
        {
            // Simple uniform sampling
            outVertices.clear();
            outVertices.reserve(targetVertexCount);

            f32 step = static_cast<f32>(positions.size()) / static_cast<f32>(targetVertexCount);
            for (sizet i = 0; i < targetVertexCount; ++i)
            {
                sizet index = static_cast<sizet>(i * step);
                if (index < positions.size())
                {
                    outVertices.push_back(positions[index]);
                }
            }
        }

        return ECookingResult::Success;
    }

    bool MeshCookingFactory::ValidateMeshData(const std::vector<glm::vec3>& vertices, const std::vector<u32>& indices)
    {
        if (vertices.empty() || indices.empty())
        {
            return false;
        }

        if (indices.size() % 3 != 0)
        {
            return false; // Must be triangulated
        }

        if (vertices.size() > s_MaxVerticesPerMesh || indices.size() > s_MaxTrianglesPerMesh * 3)
        {
            return false; // Too large
        }

        // Check index bounds
        u32 maxIndex = static_cast<u32>(vertices.size()) - 1;
        for (u32 index : indices)
        {
            if (index > maxIndex)
            {
                return false;
            }
        }

        return true;
    }

    bool MeshCookingFactory::ValidateConvexHull(const std::vector<glm::vec3>& vertices)
    {
        return vertices.size() >= s_MinVerticesForConvexHull && vertices.size() <= m_MaxConvexHullVertices;
    }

    bool MeshCookingFactory::ReduceConvexHullVertices(const std::vector<glm::vec3>& inputVertices, u32 maxVertices, std::vector<glm::vec3>& outReducedVertices)
    {
        outReducedVertices.clear();

        if (inputVertices.size() <= maxVertices)
        {
            outReducedVertices = inputVertices;
            return true;
        }

        if (maxVertices < s_MinVerticesForConvexHull)
        {
            return false; // Cannot create a valid convex hull with too few vertices
        }

        try
        {
            // Strategy: Select the most "extreme" vertices in each direction to preserve the hull shape
            // This is a heuristic approach that tries to maintain the overall shape while reducing vertex count

            // Calculate bounding box to understand the extents
            glm::vec3 minBounds = inputVertices[0];
            glm::vec3 maxBounds = inputVertices[0];

            for (const auto& vertex : inputVertices)
            {
                minBounds = glm::min(minBounds, vertex);
                maxBounds = glm::max(maxBounds, vertex);
            }

            glm::vec3 center = (minBounds + maxBounds) * 0.5f;

            // Find vertices that are most extreme in various directions
            std::set<sizet> selectedIndices;

            // Always include the 6 axis-aligned extremes (min/max X, Y, Z)
            auto findExtreme = [&](const glm::vec3& direction) -> sizet
            {
                f32 maxDot = -std::numeric_limits<f32>::max();
                sizet bestIndex = 0;

                for (sizet i = 0; i < inputVertices.size(); ++i)
                {
                    f32 dot = glm::dot(inputVertices[i] - center, direction);
                    if (dot > maxDot)
                    {
                        maxDot = dot;
                        bestIndex = i;
                    }
                }
                return bestIndex;
            };

            // Add extremes in the 6 primary directions
            selectedIndices.insert(findExtreme(glm::vec3(1, 0, 0)));  // +X
            selectedIndices.insert(findExtreme(glm::vec3(-1, 0, 0))); // -X
            selectedIndices.insert(findExtreme(glm::vec3(0, 1, 0)));  // +Y
            selectedIndices.insert(findExtreme(glm::vec3(0, -1, 0))); // -Y
            selectedIndices.insert(findExtreme(glm::vec3(0, 0, 1)));  // +Z
            selectedIndices.insert(findExtreme(glm::vec3(0, 0, -1))); // -Z

            // Add diagonal extremes if we have room
            if (selectedIndices.size() < maxVertices)
            {
                const std::vector<glm::vec3> diagonals = {
                    glm::normalize(glm::vec3(1, 1, 1)),
                    glm::normalize(glm::vec3(1, 1, -1)),
                    glm::normalize(glm::vec3(1, -1, 1)),
                    glm::normalize(glm::vec3(1, -1, -1)),
                    glm::normalize(glm::vec3(-1, 1, 1)),
                    glm::normalize(glm::vec3(-1, 1, -1)),
                    glm::normalize(glm::vec3(-1, -1, 1)),
                    glm::normalize(glm::vec3(-1, -1, -1))
                };

                for (const auto& diagonal : diagonals)
                {
                    if (selectedIndices.size() >= maxVertices)
                        break;
                    selectedIndices.insert(findExtreme(diagonal));
                }
            }

            // If we still need more vertices, add the ones furthest from the center
            if (selectedIndices.size() < maxVertices)
            {
                std::vector<std::pair<f32, sizet>> distanceIndices;

                for (sizet i = 0; i < inputVertices.size(); ++i)
                {
                    if (selectedIndices.find(i) == selectedIndices.end())
                    {
                        f32 distance = glm::length(inputVertices[i] - center);
                        distanceIndices.emplace_back(distance, i);
                    }
                }

                // Sort by distance (furthest first)
                std::sort(distanceIndices.begin(), distanceIndices.end(), std::greater<>());

                // Add the furthest vertices until we reach the limit
                for (const auto& [distance, index] : distanceIndices)
                {
                    if (selectedIndices.size() >= maxVertices)
                        break;
                    selectedIndices.insert(index);
                }
            }

            // Convert selected indices to vertices
            outReducedVertices.reserve(selectedIndices.size());
            for (sizet index : selectedIndices)
            {
                outReducedVertices.push_back(inputVertices[index]);
            }

            return true;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("ReduceConvexHullVertices: Exception during vertex reduction: {0}", e.what());
            return false;
        }
    }

    std::vector<glm::vec3> MeshCookingFactory::ExtractVertexPositions(const std::vector<glm::vec3>& vertices)
    {
        // Input is already positions, just return a copy
        return vertices;
    }

    void MeshCookingFactory::WeldVertices(std::vector<glm::vec3>& vertices, std::vector<u32>& indices, f32 tolerance)
    {
        // Custom hash for glm::ivec3 grid keys
        struct IVec3Hash
        {
            sizet operator()(const glm::ivec3& key) const
            {
                // FNV-1a hash for better distribution
                constexpr sizet fnvOffset = 14695981039346656037ULL;
                constexpr sizet fnvPrime = 1099511628211ULL;

                sizet hash = fnvOffset;
                hash = (hash ^ static_cast<sizet>(key.x)) * fnvPrime;
                hash = (hash ^ static_cast<sizet>(key.y)) * fnvPrime;
                hash = (hash ^ static_cast<sizet>(key.z)) * fnvPrime;
                return hash;
            }
        };

        if (vertices.empty())
            return;

        const f32 invTolerance = 1.0f / tolerance;

        // Spatial hash map: grid cell -> list of vertex indices in that cell
        std::unordered_map<glm::ivec3, std::vector<u32>, IVec3Hash> spatialHash;
        spatialHash.reserve(vertices.size() / 4); // Estimate bucket count to avoid rehashing

        std::vector<u32> remapTable(vertices.size());
        std::vector<glm::vec3> weldedVertices;
        weldedVertices.reserve(vertices.size()); // Worst case: no welding occurs

        // 27 neighbor cell offsets (including center cell)
        const glm::ivec3 neighborOffsets[27] = {
            { -1, -1, -1 }, { -1, -1, 0 }, { -1, -1, 1 }, { -1, 0, -1 }, { -1, 0, 0 }, { -1, 0, 1 }, { -1, 1, -1 }, { -1, 1, 0 }, { -1, 1, 1 }, { 0, -1, -1 }, { 0, -1, 0 }, { 0, -1, 1 }, { 0, 0, -1 }, { 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, -1 }, { 0, 1, 0 }, { 0, 1, 1 }, { 1, -1, -1 }, { 1, -1, 0 }, { 1, -1, 1 }, { 1, 0, -1 }, { 1, 0, 0 }, { 1, 0, 1 }, { 1, 1, -1 }, { 1, 1, 0 }, { 1, 1, 1 }
        };

        for (sizet i = 0; i < vertices.size(); ++i)
        {
            const glm::vec3& vertex = vertices[i];
            const glm::ivec3 gridKey = glm::ivec3(glm::floor(vertex * invTolerance));

            u32 matchedIndex = static_cast<u32>(-1);

            // Check all 27 neighboring cells (including center) for existing vertices
            for (const auto& offset : neighborOffsets)
            {
                const glm::ivec3 neighborKey = gridKey + offset;
                auto cellIt = spatialHash.find(neighborKey);

                if (cellIt != spatialHash.end())
                {
                    // Check vertices in this neighboring cell
                    for (u32 existingIndex : cellIt->second)
                    {
                        if (glm::distance(vertex, weldedVertices[existingIndex]) < tolerance)
                        {
                            matchedIndex = existingIndex;
                            break;
                        }
                    }

                    if (matchedIndex != static_cast<u32>(-1))
                        break;
                }
            }

            if (matchedIndex != static_cast<u32>(-1))
            {
                // Found a match - reuse existing vertex
                remapTable[i] = matchedIndex;
            }
            else
            {
                // No match found - add new vertex
                const u32 newIndex = static_cast<u32>(weldedVertices.size());
                remapTable[i] = newIndex;
                weldedVertices.push_back(vertex);

                // Add this vertex to the spatial hash grid
                spatialHash[gridKey].push_back(newIndex);
            }
        }

        // Update indices using remap table
        for (u32& index : indices)
        {
            index = remapTable[index];
        }

        // Replace original vertices with welded vertices
        vertices = std::move(weldedVertices);
    }

    void MeshCookingFactory::RemoveDuplicateVertices(std::vector<glm::vec3>& vertices, std::vector<u32>& indices)
    {
        WeldVertices(vertices, indices, 1e-6f); // Very small tolerance for exact duplicates
    }

    void MeshCookingFactory::OptimizeTriangleMesh(std::vector<glm::vec3>& vertices, std::vector<u32>& indices)
    {
        // Remove degenerate triangles and very small triangles
        RemoveInvalidTriangles(vertices, indices, m_AreaTestEpsilon);
    }

    void MeshCookingFactory::RemoveInvalidTriangles(std::vector<glm::vec3>& vertices, std::vector<u32>& indices, f32 areaEpsilon)
    {
        std::vector<u32> validIndices;
        validIndices.reserve(indices.size());

        for (sizet i = 0; i < indices.size(); i += 3)
        {
            if (i + 2 >= indices.size())
                break;

            u32 i0 = indices[i];
            u32 i1 = indices[i + 1];
            u32 i2 = indices[i + 2];

            // Check for degenerate triangles
            if (i0 == i1 || i1 == i2 || i0 == i2)
                continue;

            // Check triangle area
            const glm::vec3& v0 = vertices[i0];
            const glm::vec3& v1 = vertices[i1];
            const glm::vec3& v2 = vertices[i2];

            glm::vec3 edge1 = v1 - v0;
            glm::vec3 edge2 = v2 - v0;
            f32 area = 0.5f * glm::length(glm::cross(edge1, edge2));

            if (area > areaEpsilon)
            {
                validIndices.push_back(i0);
                validIndices.push_back(i1);
                validIndices.push_back(i2);
            }
        }

        indices = validIndices;
    }

    bool MeshCookingFactory::SerializeMeshCollider(const std::filesystem::path& filepath, const MeshColliderData& meshData)
    {
        try
        {
            std::ofstream file(filepath, std::ios::binary);
            if (!file.is_open())
            {
                return false;
            }

            // Write header
            OloMeshColliderHeader header{};
            // Initialize header fields for file format validation
            memcpy(header.m_Header, "OloMeshC", 8);
            header.m_Version = 1;
            header.m_Type = meshData.m_Type;
            header.m_SubmeshCount = static_cast<u32>(meshData.m_Submeshes.size());
            header.m_Scale = meshData.m_Scale;

            file.write(reinterpret_cast<const char*>(&header), sizeof(header));

            // Write submesh data
            for (const auto& submesh : meshData.m_Submeshes)
            {
                // Write submesh info
                u32 dataSize = static_cast<u32>(submesh.m_ColliderData.size());
                file.write(reinterpret_cast<const char*>(&dataSize), sizeof(dataSize));
                file.write(reinterpret_cast<const char*>(&submesh.m_Transform), sizeof(float) * 16);
                file.write(reinterpret_cast<const char*>(&submesh.m_Type), sizeof(submesh.m_Type));
                file.write(reinterpret_cast<const char*>(&submesh.m_VertexCount), sizeof(submesh.m_VertexCount));
                file.write(reinterpret_cast<const char*>(&submesh.m_IndexCount), sizeof(submesh.m_IndexCount));

                // Write collider data
                if (dataSize > 0)
                {
                    file.write(reinterpret_cast<const char*>(submesh.m_ColliderData.data()), dataSize);
                }
            }

            file.close();
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    MeshColliderData MeshCookingFactory::DeserializeMeshCollider(const std::filesystem::path& filepath)
    {
        MeshColliderData meshData;

        try
        {
            std::ifstream file(filepath, std::ios::binary);
            if (!file.is_open())
            {
                return meshData;
            }

            // Read header
            OloMeshColliderHeader header;
            file.read(reinterpret_cast<char*>(&header), sizeof(header));

            // Validate header read
            if (!file.good() || file.gcount() != sizeof(header))
            {
                return meshData;
            } // Validate header
            if (memcmp(header.m_Header, "OloMeshC", 8) != 0 || header.m_Version != 1)
            {
                return meshData;
            }

            meshData.m_Type = header.m_Type;
            meshData.m_Scale = header.m_Scale;
            meshData.m_Submeshes.reserve(header.m_SubmeshCount);

            // Read submesh data
            for (u32 i = 0; i < header.m_SubmeshCount; ++i)
            {
                SubmeshColliderData submesh;

                // Read submesh info
                u32 dataSize;
                file.read(reinterpret_cast<char*>(&dataSize), sizeof(dataSize));
                if (!file.good() || file.gcount() != sizeof(dataSize))
                {
                    return meshData;
                }

                // Validate dataSize to prevent excessive allocations or out-of-bounds reads
                if (dataSize > 100 * 1024 * 1024) // 100MB max reasonable size
                {
                    return meshData;
                }

                // Read transform as 16 floats explicitly
                file.read(reinterpret_cast<char*>(&submesh.m_Transform), sizeof(float) * 16);
                if (!file.good() || file.gcount() != sizeof(float) * 16)
                {
                    return meshData;
                }

                file.read(reinterpret_cast<char*>(&submesh.m_Type), sizeof(submesh.m_Type));
                if (!file.good() || file.gcount() != sizeof(submesh.m_Type))
                {
                    return meshData;
                }

                file.read(reinterpret_cast<char*>(&submesh.m_VertexCount), sizeof(submesh.m_VertexCount));
                if (!file.good() || file.gcount() != sizeof(submesh.m_VertexCount))
                {
                    return meshData;
                }

                file.read(reinterpret_cast<char*>(&submesh.m_IndexCount), sizeof(submesh.m_IndexCount));
                if (!file.good() || file.gcount() != sizeof(submesh.m_IndexCount))
                {
                    return meshData;
                }

                // Read collider data with validation
                if (dataSize > 0)
                {
                    submesh.m_ColliderData.resize(dataSize);
                    file.read(reinterpret_cast<char*>(submesh.m_ColliderData.data()), dataSize);
                    if (!file.good() || static_cast<u32>(file.gcount()) != dataSize)
                    {
                        return meshData;
                    }
                }

                meshData.m_Submeshes.push_back(submesh);
            }

            meshData.m_IsValid = true;
            file.close();
        }
        catch (const std::exception&)
        {
            meshData.m_IsValid = false;
        }

        return meshData;
    }

    std::filesystem::path MeshCookingFactory::GetCacheFilePath(Ref<MeshColliderAsset> colliderAsset, EMeshColliderType type)
    {
        std::string cacheKey = GenerateCacheKey(colliderAsset, type);
        std::string filename = cacheKey + ".omc";
        return m_CacheDirectory / filename;
    }

    std::string MeshCookingFactory::GenerateCacheKey(Ref<MeshColliderAsset> colliderAsset, EMeshColliderType type)
    {
        // Generate a unique cache key based on asset handle and type
        std::string typeString = (type == EMeshColliderType::Triangle) ? "tri" : "cvx";
        return fmt::format("{}_{}", static_cast<u64>(colliderAsset->GetHandle()), typeString);
    }

    std::filesystem::path MeshCookingFactory::GetCacheDirectory()
    {
        return m_CacheDirectory;
    }

    bool MeshCookingFactory::IsCacheValid(const std::filesystem::path& cacheFilePath, const std::filesystem::path& sourcePath)
    {
        try
        {
            if (!std::filesystem::exists(cacheFilePath) || !std::filesystem::exists(sourcePath))
            {
                return false;
            }

            // Compare file modification times
            auto cacheTime = std::filesystem::last_write_time(cacheFilePath);
            auto sourceTime = std::filesystem::last_write_time(sourcePath);

            return cacheTime >= sourceTime;
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            OLO_CORE_WARN("MeshCookingFactory: Filesystem error checking cache validity: {0}. Treating cache as invalid.", e.what());
            return false;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_WARN("MeshCookingFactory: Exception checking cache validity: {0}. Treating cache as invalid.", e.what());
            return false;
        }
    }

    void MeshCookingFactory::ClearCache()
    {
        if (!m_CacheAvailable)
        {
            OLO_CORE_WARN("MeshCookingFactory: Cache is not available, cannot clear cache");
            return;
        }

        try
        {
            if (std::filesystem::exists(m_CacheDirectory))
            {
                for (const auto& entry : std::filesystem::directory_iterator(m_CacheDirectory))
                {
                    if (entry.path().extension() == ".omc")
                    {
                        std::filesystem::remove(entry.path());
                    }
                }
            }

            m_CachedMeshCount = 0;
            OLO_CORE_INFO("Cleared mesh collider cache");
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            OLO_CORE_ERROR("MeshCookingFactory: Filesystem error clearing cache: {0}", e.what());
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("MeshCookingFactory: Exception clearing cache: {0}", e.what());
        }
    }

    void MeshCookingFactory::LogCookingError(const std::string& operation, const std::string& error)
    {
        OLO_CORE_ERROR("MeshCookingFactory::{}: {}", operation, error);
    }

    JPH::Ref<JPH::Shape> MeshCookingFactory::CreateShapeFromColliderData(const SubmeshColliderData& colliderData)
    {
        if (colliderData.m_ColliderData.empty())
        {
            OLO_CORE_ERROR("MeshCookingFactory::CreateShapeFromColliderData: Empty collider data");
            return nullptr;
        }

        try
        {
            // Set up buffer struct pointing directly to existing collider data
            // No copy needed since DeserializeShapeFromBuffer only reads the data
            // const_cast is safe here because JoltBinaryStreamReader only reads from the buffer
            Buffer buffer;
            buffer.Size = colliderData.m_ColliderData.size();
            buffer.Data = const_cast<u8*>(colliderData.m_ColliderData.data());

            // Deserialize the shape using JoltBinaryStream
            JPH::Ref<JPH::Shape> shape = JoltBinaryStreamUtils::DeserializeShapeFromBuffer(buffer);

            if (!shape)
            {
                OLO_CORE_ERROR("MeshCookingFactory::CreateShapeFromColliderData: Failed to deserialize shape");
                return nullptr;
            }

            return shape;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("MeshCookingFactory::CreateShapeFromColliderData: Exception: {}", e.what());
            return nullptr;
        }
    }

    bool MeshCookingFactory::CanCreateShapeFromColliderData(const SubmeshColliderData& colliderData) const
    {
        if (colliderData.m_ColliderData.empty())
        {
            return false;
        }

        // Try to validate the data by checking if it can be deserialized
        try
        {
            // Set up buffer struct pointing directly to existing collider data
            // No copy needed since ValidateShapeData only reads the data
            Buffer buffer;
            buffer.Size = colliderData.m_ColliderData.size();
            buffer.Data = const_cast<u8*>(colliderData.m_ColliderData.data());

            bool isValid = JoltBinaryStreamUtils::ValidateShapeData(buffer);

            return isValid;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

} // namespace OloEngine
