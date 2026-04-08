#include "OloEnginePCH.h"
#include "OloEngine/Renderer/MeshOptimization.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Animation/MorphTargets/MorphTarget.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetSet.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace OloEngine::MeshOptimization
{
    // ── Core optimization ──────────────────────────────────────────

    void OptimizeMesh(MeshSource& meshSource)
    {
        OLO_PROFILE_FUNCTION();

        auto& vertices = meshSource.GetVertices();
        auto& indices = meshSource.GetIndices();

        if (vertices.IsEmpty() || indices.IsEmpty())
        {
            return;
        }

        auto vertexCount = static_cast<sizet>(vertices.Num());
        auto indexCount = static_cast<sizet>(indices.Num());

        // 1 & 2. Per-submesh cache and overdraw optimization.
        // These operations reorder triangles within the index buffer, so they
        // must run per-submesh to preserve each submesh's [m_BaseIndex, m_BaseIndex + m_IndexCount) range.
        auto const& submeshes = meshSource.GetSubmeshes();
        if (submeshes.Num() > 0)
        {
            constexpr f32 kOverdrawThreshold = 1.05f;
            for (i32 s = 0; s < submeshes.Num(); ++s)
            {
                auto const& sub = submeshes[s];
                if (sub.m_IndexCount == 0 || sub.m_BaseIndex > static_cast<u32>(indices.Num()) || sub.m_BaseIndex + sub.m_IndexCount > static_cast<u32>(indices.Num()))
                {
                    OLO_CORE_WARN("MeshOptimization::OptimizeMesh: Submesh {} has out-of-range index bounds, skipping", s);
                    continue;
                }
                u32* subIndices = indices.GetData() + sub.m_BaseIndex;
                auto subIndexCount = static_cast<sizet>(sub.m_IndexCount);

                meshopt_optimizeVertexCache(subIndices, subIndices, subIndexCount, vertexCount);
                meshopt_optimizeOverdraw(
                    subIndices, subIndices, subIndexCount,
                    &vertices.GetData()[0].Position.x, vertexCount, sizeof(Vertex),
                    kOverdrawThreshold);
            }
        }
        else
        {
            // No submeshes — optimize the entire buffer as a single range
            meshopt_optimizeVertexCache(indices.GetData(), indices.GetData(), indexCount, vertexCount);

            constexpr f32 kOverdrawThreshold = 1.05f;
            meshopt_optimizeOverdraw(
                indices.GetData(), indices.GetData(), indexCount,
                &vertices.GetData()[0].Position.x, vertexCount, sizeof(Vertex),
                kOverdrawThreshold);
        }

        // 3. Vertex fetch optimization — reorder vertices for sequential memory access
        std::vector<u32> remap(vertexCount);
        auto remappedVertexCount = meshopt_optimizeVertexFetchRemap(remap.data(), indices.GetData(), indexCount, vertexCount);

        meshopt_remapIndexBuffer(indices.GetData(), indices.GetData(), indexCount, remap.data());

        {
            TArray<Vertex> remappedVertices(static_cast<i32>(remappedVertexCount));
            meshopt_remapVertexBuffer(remappedVertices.GetData(), vertices.GetData(), vertexCount, sizeof(Vertex), remap.data());
            vertices = MoveTemp(remappedVertices);
        }

        if (meshSource.HasBoneInfluences())
        {
            auto& boneInfluences = meshSource.GetBoneInfluences();
            TArray<BoneInfluence> remappedBones(static_cast<i32>(remappedVertexCount));
            meshopt_remapVertexBuffer(remappedBones.GetData(), boneInfluences.GetData(), vertexCount, sizeof(BoneInfluence), remap.data());
            boneInfluences = MoveTemp(remappedBones);
        }

        // 3b. Remap morph target vertex delta arrays to follow the new vertex ordering
        if (meshSource.HasMorphTargets())
        {
            auto& morphTargets = meshSource.GetMorphTargets();
            for (auto& target : morphTargets->Targets)
            {
                if (!target.IsSparse && target.Vertices.size() == vertexCount)
                {
                    std::vector<MorphTargetVertex> remappedDeltas(remappedVertexCount);
                    meshopt_remapVertexBuffer(remappedDeltas.data(), target.Vertices.data(),
                                              vertexCount, sizeof(MorphTargetVertex), remap.data());
                    target.Vertices = MoveTemp(remappedDeltas);
                }
                // Sparse targets reference vertices by index — remap those indices
                if (target.IsSparse)
                {
                    for (auto& sparse : target.SparseVertices)
                    {
                        if (sparse.VertexIndex < static_cast<u32>(vertexCount))
                        {
                            sparse.VertexIndex = remap[sparse.VertexIndex];
                        }
                    }
                }
            }
        }

        // Update vertexCount to reflect the compacted buffer size
        vertexCount = remappedVertexCount;

        // Update submesh vertex ranges to match the remapped vertex buffer.
        // After vertex fetch remap, vertex positions in the buffer have changed,
        // so each submesh's m_BaseVertex/m_VertexCount must be recomputed.
        auto& mutableSubmeshes = meshSource.GetSubmeshes();
        for (i32 s = 0; s < mutableSubmeshes.Num(); ++s)
        {
            auto& sub = mutableSubmeshes[s];
            u32 minVertex = std::numeric_limits<u32>::max();
            u32 maxVertex = 0;
            u32 const end = sub.m_BaseIndex + sub.m_IndexCount;
            for (u32 idx = sub.m_BaseIndex; idx < end && idx < static_cast<u32>(indices.Num()); ++idx)
            {
                u32 const v = indices[static_cast<i32>(idx)];
                minVertex = std::min(minVertex, v);
                maxVertex = std::max(maxVertex, v);
            }
            if (minVertex <= maxVertex)
            {
                sub.m_BaseVertex = minVertex;
                sub.m_VertexCount = maxVertex - minVertex + 1;
            }
        }

        // 4. Generate shadow index buffer (merges position-equivalent vertices)
        GenerateShadowIndices(meshSource);

        OLO_CORE_TRACE("MeshOptimization::OptimizeMesh: Optimized {} vertices, {} indices", vertexCount, indexCount);
    }

    // ── LOD generation ─────────────────────────────────────────────

    Ref<MeshSource> GenerateLODMesh(const MeshSource& meshSource, f32 targetRatio, f32 targetError)
    {
        OLO_PROFILE_FUNCTION();

        const auto& srcVertices = meshSource.GetVertices();
        const auto& srcIndices = meshSource.GetIndices();

        if (srcVertices.IsEmpty() || srcIndices.IsEmpty())
        {
            return nullptr;
        }

        // Multi-submesh LOD collapses all materials into index 0 — reject early
        if (meshSource.GetSubmeshes().Num() > 1)
        {
            OLO_CORE_WARN("MeshOptimization::GenerateLODMesh: Multi-submesh LOD not supported ({} submeshes)",
                          meshSource.GetSubmeshes().Num());
            return nullptr;
        }

        // Animated meshes with skeleton or morph targets are not
        // supported until auxiliary streams (weights/morphs) are preserved.
        if (meshSource.HasSkeleton() || meshSource.HasMorphTargets())
        {
            OLO_CORE_WARN("MeshOptimization::GenerateLODMesh: Animated sources with bone/morph data are not supported for LOD generation");
            return nullptr;
        }

        // Sanitize incoming floats (may originate from serialized/UI data)
        if (!std::isfinite(targetRatio))
        {
            targetRatio = 0.5f;
        }
        targetRatio = std::clamp(targetRatio, 0.0f, 1.0f);
        if (!std::isfinite(targetError) || targetError < 0.0f)
        {
            targetError = 0.01f;
        }

        auto vertexCount = static_cast<sizet>(srcVertices.Num());
        auto indexCount = static_cast<sizet>(srcIndices.Num());

        auto targetIndexCount = static_cast<sizet>(static_cast<f32>(indexCount) * targetRatio);
        targetIndexCount = (targetIndexCount / 3) * 3;
        if (targetIndexCount < 3)
        {
            targetIndexCount = 3;
        }

        std::vector<u32> simplifiedIndices(indexCount);
        auto resultIndexCount = meshopt_simplify(
            simplifiedIndices.data(), srcIndices.GetData(), indexCount,
            &srcVertices.GetData()[0].Position.x, vertexCount, sizeof(Vertex),
            targetIndexCount, targetError);

        if (resultIndexCount == 0)
        {
            OLO_CORE_WARN("MeshOptimization::GenerateLODMesh: Simplification produced zero indices");
            return nullptr;
        }

        simplifiedIndices.resize(resultIndexCount);

        TArray<Vertex> lodVertices;
        lodVertices.Reserve(srcVertices.Num());
        lodVertices.Append(srcVertices.GetData(), srcVertices.Num());

        TArray<u32> lodIndices;
        lodIndices.Reserve(static_cast<i32>(resultIndexCount));
        lodIndices.Append(simplifiedIndices.data(), static_cast<i32>(resultIndexCount));

        auto lodMesh = Ref<MeshSource>::Create(MoveTemp(lodVertices), MoveTemp(lodIndices));

        // Copy material table from source
        for (const auto& [index, handle] : meshSource.GetMaterials())
        {
            lodMesh->SetMaterial(index, handle);
        }

        const auto& srcSubmeshes = meshSource.GetSubmeshes();
        if (srcSubmeshes.Num() == 1)
        {
            Submesh submesh = srcSubmeshes[0];
            submesh.m_BaseVertex = 0;
            submesh.m_BaseIndex = 0;
            submesh.m_VertexCount = static_cast<u32>(lodMesh->GetVertices().Num());
            submesh.m_IndexCount = static_cast<u32>(resultIndexCount);
            lodMesh->AddSubmesh(submesh);
        }
        else
        {
            // No-submesh source: create a single submesh spanning all data
            Submesh submesh;
            submesh.m_BaseVertex = 0;
            submesh.m_BaseIndex = 0;
            submesh.m_VertexCount = static_cast<u32>(lodMesh->GetVertices().Num());
            submesh.m_IndexCount = static_cast<u32>(resultIndexCount);
            submesh.m_MaterialIndex = 0;
            lodMesh->AddSubmesh(submesh);
        }

        OLO_CORE_TRACE("MeshOptimization::GenerateLODMesh: {} -> {} triangles (ratio={:.2f})",
                       indexCount / 3, resultIndexCount / 3, targetRatio);

        return lodMesh;
    }

    Ref<MeshSource> GenerateLODMeshWithAttributes(const MeshSource& meshSource, f32 targetRatio, f32 targetError)
    {
        OLO_PROFILE_FUNCTION();

        const auto& srcVertices = meshSource.GetVertices();
        const auto& srcIndices = meshSource.GetIndices();

        if (srcVertices.IsEmpty() || srcIndices.IsEmpty())
        {
            return nullptr;
        }

        // Multi-submesh LOD collapses all materials into index 0 — reject early
        if (meshSource.GetSubmeshes().Num() > 1)
        {
            OLO_CORE_WARN("MeshOptimization::GenerateLODMeshWithAttributes: Multi-submesh LOD not supported ({} submeshes)",
                          meshSource.GetSubmeshes().Num());
            return nullptr;
        }

        // Animated meshes with skeleton or morph targets are not
        // supported until auxiliary streams (weights/morphs) are preserved.
        if (meshSource.HasSkeleton() || meshSource.HasMorphTargets())
        {
            OLO_CORE_WARN("MeshOptimization::GenerateLODMeshWithAttributes: Animated sources with bone/morph data are not supported for LOD generation");
            return nullptr;
        }

        // Sanitize incoming floats (may originate from serialized/UI data)
        if (!std::isfinite(targetRatio))
        {
            targetRatio = 0.5f;
        }
        targetRatio = std::clamp(targetRatio, 0.0f, 1.0f);
        if (!std::isfinite(targetError) || targetError < 0.0f)
        {
            targetError = 0.01f;
        }

        auto vertexCount = static_cast<sizet>(srcVertices.Num());
        auto indexCount = static_cast<sizet>(srcIndices.Num());

        auto targetIndexCount = static_cast<sizet>(static_cast<f32>(indexCount) * targetRatio);
        targetIndexCount = (targetIndexCount / 3) * 3;
        if (targetIndexCount < 3)
        {
            targetIndexCount = 3;
        }

        // Build attribute array: Normal (3 floats) + TexCoord (2 floats) = 5 per vertex
        constexpr sizet kAttributeCount = 5;
        std::vector<f32> attributes(vertexCount * kAttributeCount);
        for (sizet i = 0; i < vertexCount; ++i)
        {
            const auto& v = srcVertices[static_cast<i32>(i)];
            attributes[i * kAttributeCount + 0] = v.Normal.x;
            attributes[i * kAttributeCount + 1] = v.Normal.y;
            attributes[i * kAttributeCount + 2] = v.Normal.z;
            attributes[i * kAttributeCount + 3] = v.TexCoord.x;
            attributes[i * kAttributeCount + 4] = v.TexCoord.y;
        }

        // Normals matter less than UVs for visual quality
        f32 attributeWeights[kAttributeCount] = { 0.5f, 0.5f, 0.5f, 1.0f, 1.0f };

        std::vector<u32> simplifiedIndices(indexCount);
        auto resultIndexCount = meshopt_simplifyWithAttributes(
            simplifiedIndices.data(), srcIndices.GetData(), indexCount,
            &srcVertices.GetData()[0].Position.x, vertexCount, sizeof(Vertex),
            attributes.data(), sizeof(f32) * kAttributeCount,
            attributeWeights, kAttributeCount,
            nullptr, // no vertex locking
            targetIndexCount, targetError, 0, nullptr);

        if (resultIndexCount == 0)
        {
            OLO_CORE_WARN("MeshOptimization::GenerateLODMeshWithAttributes: Simplification produced zero indices");
            return nullptr;
        }

        simplifiedIndices.resize(resultIndexCount);

        TArray<Vertex> lodVertices;
        lodVertices.Reserve(srcVertices.Num());
        lodVertices.Append(srcVertices.GetData(), srcVertices.Num());

        TArray<u32> lodIndices;
        lodIndices.Reserve(static_cast<i32>(resultIndexCount));
        lodIndices.Append(simplifiedIndices.data(), static_cast<i32>(resultIndexCount));

        auto lodMesh = Ref<MeshSource>::Create(MoveTemp(lodVertices), MoveTemp(lodIndices));

        // Copy material table from source
        for (const auto& [index, handle] : meshSource.GetMaterials())
        {
            lodMesh->SetMaterial(index, handle);
        }

        const auto& srcSubmeshes = meshSource.GetSubmeshes();
        if (srcSubmeshes.Num() == 1)
        {
            Submesh submesh = srcSubmeshes[0];
            submesh.m_BaseVertex = 0;
            submesh.m_BaseIndex = 0;
            submesh.m_VertexCount = static_cast<u32>(lodMesh->GetVertices().Num());
            submesh.m_IndexCount = static_cast<u32>(resultIndexCount);
            lodMesh->AddSubmesh(submesh);
        }
        else
        {
            // No-submesh source: create a single submesh spanning all data
            Submesh submesh;
            submesh.m_BaseVertex = 0;
            submesh.m_BaseIndex = 0;
            submesh.m_VertexCount = static_cast<u32>(lodMesh->GetVertices().Num());
            submesh.m_IndexCount = static_cast<u32>(resultIndexCount);
            submesh.m_MaterialIndex = 0;
            lodMesh->AddSubmesh(submesh);
        }

        OLO_CORE_TRACE("MeshOptimization::GenerateLODMeshWithAttributes: {} -> {} triangles (ratio={:.2f})",
                       indexCount / 3, resultIndexCount / 3, targetRatio);

        return lodMesh;
    }

    LODGroup GenerateLODGroup(const MeshSource& meshSource, AssetHandle baseMeshHandle, u32 lodCount, f32 maxDistance)
    {
        OLO_PROFILE_FUNCTION();

        LODGroup group;

        if (lodCount < 2)
        {
            lodCount = 2;
        }

        // Sanitize maxDistance (may originate from serialized/UI data)
        if (!std::isfinite(maxDistance) || maxDistance <= 0.0f)
        {
            maxDistance = 200.0f;
        }
        maxDistance = std::clamp(maxDistance, 1.0f, 100000.0f);

        auto const& srcIndices = meshSource.GetIndices();
        u32 const baseTriCount = srcIndices.IsEmpty() ? 0 : static_cast<u32>(srcIndices.Num()) / 3;

        f32 const distanceStep = maxDistance / static_cast<f32>(lodCount);
        group.Levels.emplace_back(baseMeshHandle, distanceStep, baseTriCount);

        for (u32 i = 1; i < lodCount; ++i)
        {
            f32 const ratio = 1.0f / static_cast<f32>(1u << i);
            f32 const error = 0.01f * static_cast<f32>(i);

            auto lodMeshSource = GenerateLODMeshWithAttributes(meshSource, ratio, error);
            if (!lodMeshSource)
            {
                OLO_CORE_WARN("MeshOptimization::GenerateLODGroup: Failed to generate LOD level {}", i);
                continue;
            }

            lodMeshSource->Build();

            auto lodMesh = Ref<Mesh>::Create(lodMeshSource, 0);
            AssetHandle const handle = AssetManager::AddMemoryOnlyAsset(lodMesh);

            u32 const triCount = lodMeshSource->GetIndices().IsEmpty()
                                     ? 0
                                     : static_cast<u32>(lodMeshSource->GetIndices().Num()) / 3;
            f32 const levelDistance = distanceStep * static_cast<f32>(i + 1);
            group.Levels.emplace_back(handle, levelDistance, triCount);

            OLO_CORE_TRACE("MeshOptimization::GenerateLODGroup: LOD {} - {} triangles, distance {:.1f}",
                           i, triCount, levelDistance);
        }

        return group;
    }

    // ── Shadow rendering ───────────────────────────────────────────

    void GenerateShadowIndices(MeshSource& meshSource)
    {
        OLO_PROFILE_FUNCTION();

        const auto& vertices = meshSource.GetVertices();
        const auto& indices = meshSource.GetIndices();

        if (vertices.IsEmpty() || indices.IsEmpty())
        {
            return;
        }

        auto vertexCount = static_cast<sizet>(vertices.Num());
        auto indexCount = static_cast<sizet>(indices.Num());

        TArray<u32> shadowIndices(static_cast<i32>(indexCount));
        meshopt_generateShadowIndexBuffer(
            shadowIndices.GetData(), indices.GetData(), indexCount,
            vertices.GetData(), vertexCount, sizeof(glm::vec3), sizeof(Vertex));

        // Spatial-sort shadow triangles for better early-z rejection in depth-only passes
        meshopt_spatialSortTriangles(
            shadowIndices.GetData(), shadowIndices.GetData(), indexCount,
            &vertices.GetData()[0].Position.x, vertexCount, sizeof(Vertex));

        meshSource.GetShadowIndices() = MoveTemp(shadowIndices);

        OLO_CORE_TRACE("MeshOptimization::GenerateShadowIndices: Generated shadow IB for {} indices (spatially sorted)", indexCount);
    }

    // ── Mesh analysis ──────────────────────────────────────────────

    MeshAnalysis AnalyzeMesh(const MeshSource& meshSource)
    {
        OLO_PROFILE_FUNCTION();

        MeshAnalysis result;
        const auto& vertices = meshSource.GetVertices();
        const auto& indices = meshSource.GetIndices();

        if (vertices.IsEmpty() || indices.IsEmpty())
        {
            return result;
        }

        auto vertexCount = static_cast<sizet>(vertices.Num());
        auto indexCount = static_cast<sizet>(indices.Num());

        result.VertexCount = static_cast<u32>(vertexCount);
        result.TriangleCount = static_cast<u32>(indexCount / 3);

        // Vertex cache analysis (FIFO cache size 16, warp size 0, primgroup 0)
        meshopt_VertexCacheStatistics const cacheStats =
            meshopt_analyzeVertexCache(indices.GetData(), indexCount, vertexCount, 16, 0, 0);
        result.ACMR = cacheStats.acmr;
        result.ATVR = cacheStats.atvr;

        // Overdraw analysis
        meshopt_OverdrawStatistics const overdrawStats =
            meshopt_analyzeOverdraw(indices.GetData(), indexCount,
                                    &vertices.GetData()[0].Position.x, vertexCount, sizeof(Vertex));
        result.Overdraw = overdrawStats.overdraw;

        // Vertex fetch analysis
        meshopt_VertexFetchStatistics const fetchStats =
            meshopt_analyzeVertexFetch(indices.GetData(), indexCount, vertexCount, sizeof(Vertex));
        result.OverfetchRatio = fetchStats.overfetch;

        return result;
    }

    // ── Meshlet generation ─────────────────────────────────────────

    MeshletData GenerateMeshlets(const MeshSource& meshSource, u32 maxVertices, u32 maxTriangles)
    {
        OLO_PROFILE_FUNCTION();

        MeshletData result;
        const auto& vertices = meshSource.GetVertices();
        const auto& indices = meshSource.GetIndices();

        if (vertices.IsEmpty() || indices.IsEmpty())
        {
            return result;
        }

        auto vertexCount = static_cast<sizet>(vertices.Num());
        auto indexCount = static_cast<sizet>(indices.Num());

        sizet maxMeshletCount = meshopt_buildMeshletsBound(indexCount, maxVertices, maxTriangles);

        std::vector<meshopt_Meshlet> meshlets(maxMeshletCount);
        std::vector<u32> meshletVertices(maxMeshletCount * maxVertices);
        std::vector<u8> meshletTriangles(maxMeshletCount * maxTriangles * 3);

        sizet meshletCount = meshopt_buildMeshlets(
            meshlets.data(), meshletVertices.data(), meshletTriangles.data(),
            indices.GetData(), indexCount,
            &vertices.GetData()[0].Position.x, vertexCount, sizeof(Vertex),
            maxVertices, maxTriangles, 0.0f);

        // Trim arrays based on actual output
        meshlets.resize(meshletCount);
        if (meshletCount > 0)
        {
            const auto& last = meshlets[meshletCount - 1];
            meshletVertices.resize(last.vertex_offset + last.vertex_count);
            meshletTriangles.resize(last.triangle_offset + ((last.triangle_count * 3 + 3) & ~3));
        }

        // Copy to engine data structures
        result.Meshlets.reserve(meshletCount);
        for (const auto& m : meshlets)
        {
            result.Meshlets.push_back({ m.vertex_offset, m.triangle_offset, m.vertex_count, m.triangle_count });
        }
        result.MeshletVertices = std::move(meshletVertices);
        result.MeshletTriangles = std::move(meshletTriangles);

        // Compute per-meshlet bounding cones for GPU culling
        result.Bounds.reserve(meshletCount);
        for (sizet i = 0; i < meshletCount; ++i)
        {
            meshopt_Bounds const bounds = meshopt_computeMeshletBounds(
                &result.MeshletVertices[meshlets[i].vertex_offset],
                &result.MeshletTriangles[meshlets[i].triangle_offset],
                meshlets[i].triangle_count,
                &vertices.GetData()[0].Position.x, vertexCount, sizeof(Vertex));

            MeshletData::MeshletBounds mb;
            std::memcpy(mb.Center, bounds.center, sizeof(f32) * 3);
            mb.Radius = bounds.radius;
            std::memcpy(mb.ConeApex, bounds.cone_apex, sizeof(f32) * 3);
            std::memcpy(mb.ConeAxis, bounds.cone_axis, sizeof(f32) * 3);
            mb.ConeCutoff = bounds.cone_cutoff;
            result.Bounds.push_back(mb);
        }

        OLO_CORE_TRACE("MeshOptimization::GenerateMeshlets: {} meshlets from {} triangles",
                       meshletCount, indexCount / 3);

        return result;
    }

    // ── Spatial optimization ───────────────────────────────────────

    void SpatialSortTriangles(MeshSource& meshSource)
    {
        OLO_PROFILE_FUNCTION();

        auto& vertices = meshSource.GetVertices();
        auto& indices = meshSource.GetIndices();

        if (vertices.IsEmpty() || indices.IsEmpty())
        {
            return;
        }

        auto vertexCount = static_cast<sizet>(vertices.Num());

        // Sort per-submesh to preserve BaseIndex/IndexCount boundaries.
        if (!meshSource.GetSubmeshes().IsEmpty())
        {
            auto submeshCount = meshSource.GetSubmeshes().Num();
            for (i32 s = 0; s < submeshCount; ++s)
            {
                const auto& sub = meshSource.GetSubmeshes()[s];
                if (sub.m_IndexCount < 3 || sub.m_BaseIndex > static_cast<u32>(indices.Num()) || sub.m_BaseIndex + sub.m_IndexCount > static_cast<u32>(indices.Num()))
                {
                    continue;
                }

                meshopt_spatialSortTriangles(
                    indices.GetData() + sub.m_BaseIndex,
                    indices.GetData() + sub.m_BaseIndex,
                    static_cast<sizet>(sub.m_IndexCount),
                    &vertices.GetData()[0].Position.x, vertexCount, sizeof(Vertex));
            }

            sizet totalIndices = 0;
            for (i32 s = 0; s < submeshCount; ++s)
            {
                totalIndices += meshSource.GetSubmeshes()[s].m_IndexCount;
            }
            OLO_CORE_TRACE("MeshOptimization::SpatialSortTriangles: Sorted {} triangles across {} submeshes",
                           totalIndices / 3, submeshCount);
        }
        else
        {
            auto indexCount = static_cast<sizet>(indices.Num());
            meshopt_spatialSortTriangles(
                indices.GetData(), indices.GetData(), indexCount,
                &vertices.GetData()[0].Position.x, vertexCount, sizeof(Vertex));

            OLO_CORE_TRACE("MeshOptimization::SpatialSortTriangles: Sorted {} triangles", indexCount / 3);
        }
    }

    // ── Buffer encoding for asset packs ────────────────────────────

    EncodedMeshBuffer EncodeVertexBuffer(const void* vertices, sizet vertexCount, sizet vertexSize)
    {
        OLO_PROFILE_FUNCTION();

        EncodedMeshBuffer result;
        result.OriginalSize = vertexCount * vertexSize;

        sizet maxSize = meshopt_encodeVertexBufferBound(vertexCount, vertexSize);
        result.Data.resize(maxSize);

        sizet encodedSize = meshopt_encodeVertexBuffer(
            result.Data.data(), maxSize,
            vertices, vertexCount, vertexSize);

        result.Data.resize(encodedSize);

        OLO_CORE_TRACE("MeshOptimization::EncodeVertexBuffer: {} -> {} bytes ({:.1f}% reduction)",
                       result.OriginalSize, encodedSize,
                       (1.0f - static_cast<f32>(encodedSize) / static_cast<f32>(result.OriginalSize)) * 100.0f);

        return result;
    }

    bool DecodeVertexBuffer(void* destination, sizet vertexCount, sizet vertexSize, const EncodedMeshBuffer& encoded)
    {
        OLO_PROFILE_FUNCTION();

        int const rc = meshopt_decodeVertexBuffer(
            destination, vertexCount, vertexSize,
            encoded.Data.data(), encoded.Data.size());
        return rc == 0;
    }

    EncodedMeshBuffer EncodeIndexBuffer(const u32* indices, sizet indexCount, sizet vertexCount)
    {
        OLO_PROFILE_FUNCTION();

        EncodedMeshBuffer result;
        result.OriginalSize = indexCount * sizeof(u32);

        sizet maxSize = meshopt_encodeIndexBufferBound(indexCount, vertexCount);
        result.Data.resize(maxSize);

        sizet encodedSize = meshopt_encodeIndexBuffer(
            result.Data.data(), maxSize,
            indices, indexCount);

        result.Data.resize(encodedSize);

        OLO_CORE_TRACE("MeshOptimization::EncodeIndexBuffer: {} -> {} bytes ({:.1f}% reduction)",
                       result.OriginalSize, encodedSize,
                       (1.0f - static_cast<f32>(encodedSize) / static_cast<f32>(result.OriginalSize)) * 100.0f);

        return result;
    }

    bool DecodeIndexBuffer(u32* destination, sizet indexCount, const EncodedMeshBuffer& encoded)
    {
        OLO_PROFILE_FUNCTION();

        int const rc = meshopt_decodeIndexBuffer(
            destination, indexCount, sizeof(u32),
            encoded.Data.data(), encoded.Data.size());
        return rc == 0;
    }
} // namespace OloEngine::MeshOptimization
