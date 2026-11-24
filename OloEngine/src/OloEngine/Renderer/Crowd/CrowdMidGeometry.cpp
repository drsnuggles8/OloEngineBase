#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Crowd/CrowdGeometry.h"

#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/MeshOptimization.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <cmath>

namespace OloEngine
{
    namespace
    {
        // At the default 32-pixel handover, 2.5% of the part's extent is
        // below one pixel. The simplifier can keep more triangles to meet it.
        constexpr f32 kMidTriangleRatio = 0.25f;
        constexpr f32 kMidSimplificationError = 0.025f;

        void CopyMaterialTable(const MeshSource& source, MeshSource& result)
        {
            for (const auto& [slot, handle] : source.GetMaterials())
                result.SetMaterial(slot, handle);
            TArray<Ref<Material>> materials;
            for (const auto& material : source.GetImportedMaterials())
                materials.Add(material);
            result.SetImportedMaterials(MoveTemp(materials));
        }

        // GenerateLODMeshWithAttributes operates on one material at a time
        // and retains the input vertex array. Feed it local indices, then
        // restore absolute indices so the whole mesh uses one vertex stream.
        bool AppendMidIndices(const MeshSource& source, const Submesh& part, TArray<u32>& result)
        {
            const auto vertexCount = static_cast<u32>(source.GetVertices().Num());
            const auto indexCount = static_cast<u32>(source.GetIndices().Num());
            if (part.m_BaseVertex > vertexCount || part.m_VertexCount > vertexCount - part.m_BaseVertex ||
                part.m_BaseIndex > indexCount || part.m_IndexCount > indexCount - part.m_BaseIndex ||
                part.m_IndexCount % 3u != 0u)
                return false;
            if (part.m_IndexCount == 0)
                return true;

            TArray<i32> toLocal;
            toLocal.Init(-1, static_cast<i32>(part.m_VertexCount));
            TArray<u32> toSource;
            TArray<Vertex> vertices;
            TArray<u32> indices;
            indices.Reserve(part.m_IndexCount);
            for (u32 i = 0; i < part.m_IndexCount; ++i)
            {
                const u32 global = source.GetIndices()[part.m_BaseIndex + i];
                if (global < part.m_BaseVertex || global - part.m_BaseVertex >= part.m_VertexCount)
                    return false;
                // After vertex-fetch optimisation two parts' vertex ranges
                // can overlap. Only vertices used by this material may be
                // welded together by the single-material simplifier.
                i32& local = toLocal[global - part.m_BaseVertex];
                if (local < 0)
                {
                    local = vertices.Add(source.GetVertices()[global]);
                    toSource.Add(global);
                }
                indices.Add(static_cast<u32>(local));
            }
            const auto local = Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));
            const auto reduced = MeshOptimization::GenerateLODMeshWithAttributes(*local, kMidTriangleRatio, kMidSimplificationError);
            if (!reduced)
                return false;
            for (const u32 index : reduced->GetIndices())
                result.Add(toSource[index]);
            return true;
        }

        bool SkinProxyVertex(Vertex& vertex, const BoneInfluence& influence, const CrowdClip& clip, u32 frame)
        {
            // Same zero-weight rest-pose fallback and weighted matrix sum as
            // SkeletalDeformation.glsl. A rigid proxy stores Euclidean xyz:
            // divide by homogeneous w so an unnormalised influence sum casts
            // the same silhouette as the raster vertex's perspective divide.
            const auto& weights = influence.m_Weights;
            if (weights[0] + weights[1] + weights[2] + weights[3] <= 0.001f)
                return Math::IsFinite(vertex.Position) && Math::IsFinite(vertex.Normal);
            glm::mat4 skin(0.0f);
            for (u32 i = 0; i < 4; ++i)
            {
                const u32 bone = influence.m_BoneIDs[i];
                if (bone < clip.BoneCount)
                    skin += clip.Bone(frame, bone) * weights[i];
            }
            const glm::vec4 position = skin * glm::vec4(vertex.Position, 1.0f);
            const glm::vec3 normal = glm::mat3(skin) * vertex.Normal;
            if (!Math::IsFinite(position) || !Math::IsFinite(normal) || std::abs(position.w) < 1e-8f)
                return false;
            vertex.Position = glm::vec3(position) / position.w;
            const f32 lengthSquared = glm::dot(normal, normal);
            if (lengthSquared > 1e-12f)
                vertex.Normal = normal * glm::inversesqrt(lengthSquared);
            return true;
        }
    } // namespace

    Ref<MeshSource> MakeCrowdMidGeometry(const Ref<MeshSource>& source)
    {
        if (!source || source->GetSubmeshes().IsEmpty())
            return source;
        TArray<u32> indices;
        auto parts = source->GetSubmeshes();
        for (Submesh& part : parts)
        {
            const auto begin = static_cast<u32>(indices.Num());
            if (!AppendMidIndices(*source, part, indices))
                return source;
            part.m_BaseIndex = begin;
            part.m_IndexCount = static_cast<u32>(indices.Num()) - begin;
        }
        if (indices.Num() >= source->GetIndices().Num())
            return source;

        auto result = Ref<MeshSource>::Create(TArray<Vertex>(source->GetVertices()), MoveTemp(indices));
        result->SetSkeleton(source->GetSkeletonRef());
        result->GetBoneInfo() = source->GetBoneInfo();
        result->GetBoneInfluences() = source->GetBoneInfluences();
        result->SetMorphTargets(source->GetMorphTargets());
        result->GetLightmapUVs() = source->GetLightmapUVs();
        result->SetSourceIsRigged(source->IsSourceRigged());
        result->SetSubmeshes(parts);
        CopyMaterialTable(*source, *result);
        // The indices above address the source's unchanged vertex, bone and
        // morph streams. Keep that correspondence through GPU upload.
        result->SetPreOptimized(true);
        result->Build();
        return result;
    }

    Ref<MeshSource> BakeCrowdProxy(const Ref<MeshSource>& geometry, const CrowdClip& clip, u32 frame)
    {
        if (!geometry || frame >= clip.FrameCount || clip.BoneCount == 0 || clip.BoneCount > UBOStructures::AnimationConstants::MAX_BONES ||
            static_cast<u64>(clip.FrameCount) * clip.RecordsPerFrame() > static_cast<u64>(clip.Atlas.Num()))
            return nullptr;
        TArray<Vertex> vertices(geometry->GetVertices());
        const auto& influences = geometry->GetBoneInfluences();
        if (vertices.IsEmpty() || influences.Num() != vertices.Num() || geometry->GetIndices().IsEmpty())
            return nullptr;
        const i32 vertexCount = vertices.Num();
        for (i32 i = 0; i < vertexCount; ++i)
            if (!SkinProxyVertex(vertices[i], influences[i], clip, frame))
                return nullptr;

        auto proxy = Ref<MeshSource>::Create(MoveTemp(vertices), TArray<u32>(geometry->GetIndices()));
        auto parts = geometry->GetSubmeshes();
        for (Submesh& part : parts)
            part.m_IsRigged = false;
        proxy->GetBoneInfluences().Reset();
        proxy->GetLightmapUVs() = geometry->GetLightmapUVs();
        proxy->SetSubmeshes(parts);
        CopyMaterialTable(*geometry, *proxy);
        proxy->SetPreOptimized(true);
        proxy->Build();
        return proxy;
    }
} // namespace OloEngine
