#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Crowd/CrowdGeometry.h"

#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace OloEngine
{
    namespace
    {
        // At the default 32-pixel handover, 2.5% of the part's extent is
        // below one pixel. The simplifier can keep more triangles to meet it.
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

        // An unindexed triangle soup such as Fox can still be reconnected if
        // every vertex stream matches. Each output names an original corner
        // of this part. Keep the original indices for morphs, whose sparse or
        // dense per-target deltas aren't part of this equality comparison.
        void WeldIdenticalCorners(const MeshSource& source, const u32* indices, u32 indexCount, TArray<u32>& welded)
        {
            if (source.HasMorphTargets())
                return;
            const auto& vertices = source.GetVertices();
            const auto& bones = source.GetBoneInfluences();
            const auto& uv2 = source.GetLightmapUVs();
            if ((!bones.IsEmpty() && bones.Num() != vertices.Num()) || (!uv2.IsEmpty() && uv2.Num() != vertices.Num()))
                return;
            std::array<meshopt_Stream, 3> streams{ meshopt_Stream{ vertices.GetData(), sizeof(Vertex), sizeof(Vertex) } };
            sizet streamCount = 1;
            if (!bones.IsEmpty())
                streams[streamCount++] = { bones.GetData(), sizeof(BoneInfluence), sizeof(BoneInfluence) };
            if (!uv2.IsEmpty())
                streams[streamCount++] = { uv2.GetData(), sizeof(glm::vec2), sizeof(glm::vec2) };
            welded.SetNum(static_cast<i32>(indexCount));
            meshopt_generateShadowIndexBufferMulti(welded.GetData(), indices, indexCount, vertices.Num(), streams.data(), streamCount);
        }

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

            const u32* indices = source.GetIndices().GetData() + part.m_BaseIndex;
            for (u32 i = 0; i < part.m_IndexCount; ++i)
            {
                const u32 global = indices[i];
                if (global < part.m_BaseVertex || global - part.m_BaseVertex >= part.m_VertexCount)
                    return false;
            }

            // Vertex's interleaved Normal and TexCoord provide five scalar
            // attributes. Sparse mode fits the error scale to this part; the
            // default topology rules retain each seam's separate wedges.
            const auto* vertices = source.GetVertices().GetData();
            TArray<u32> welded;
            WeldIdenticalCorners(source, indices, part.m_IndexCount, welded);
            constexpr std::array<f32, 5> weights{ 0.5f, 0.5f, 0.5f, 1.0f, 1.0f };
            const u32 target = 3u * std::max(1u, part.m_IndexCount / 12u);
            TArray<u32> reduced(static_cast<i32>(part.m_IndexCount));
            const sizet count = meshopt_simplifyWithAttributes(
                reduced.GetData(), welded.IsEmpty() ? indices : welded.GetData(), part.m_IndexCount, &vertices[0].Position.x, vertexCount, sizeof(Vertex),
                &vertices[0].Normal.x, sizeof(Vertex), weights.data(), weights.size(), nullptr, target,
                kMidSimplificationError, meshopt_SimplifySparse | meshopt_SimplifyErrorClamped, nullptr);
            if (count > 0 && count < part.m_IndexCount)
                result.Append(reduced.GetData(), static_cast<i32>(count));
            else
                result.Append(indices, static_cast<i32>(part.m_IndexCount));
            return true;
        }

        bool HasPaletteFrames(const CrowdClip& clip)
        {
            return clip.FrameCount > 0 && clip.BoneCount > 0 && clip.BoneCount <= UBOStructures::AnimationConstants::MAX_BONES &&
                   static_cast<u64>(clip.FrameCount) * clip.RecordsPerFrame() <= static_cast<u64>(clip.Atlas.Num());
        }

        struct DeformedVertex
        {
            glm::vec4 Position;
            glm::vec3 Normal;
            bool Skinned;
        };

        // Mirrors OloDeformSkinnedVertex. Keep the homogeneous weight sum
        // and the full transformed normal through frame interpolation.
        bool DeformCrowdVertex(const Vertex& vertex, const BoneInfluence& influence, const CrowdClip& clip, u32 frame,
                               DeformedVertex& sample)
        {
            const auto& weights = influence.m_Weights;
            const f32 totalWeight = weights[0] + weights[1] + weights[2] + weights[3];
            if (!std::isfinite(totalWeight))
                return false;
            sample = { glm::vec4(vertex.Position, 1.0f), vertex.Normal, totalWeight > 0.001f };
            if (sample.Skinned)
            {
                glm::mat4 skin(0.0f);
                for (u32 i = 0; i < 4; ++i)
                {
                    const u32 bone = influence.m_BoneIDs[i];
                    if (bone < clip.BoneCount)
                        skin += clip.Bone(frame, bone) * weights[i];
                }
                sample.Position = skin * sample.Position;
                sample.Normal = glm::mat3(skin) * sample.Normal;
            }
            return Math::IsFinite(sample.Position) && Math::IsFinite(sample.Normal);
        }

        bool SkinProxyVertex(Vertex& vertex, const BoneInfluence& influence, const CrowdClip& clip, u32 frame)
        {
            DeformedVertex sample;
            if (!DeformCrowdVertex(vertex, influence, clip, frame, sample))
                return false;
            if (!sample.Skinned)
                return true;
            if (std::abs(sample.Position.w) < 1e-8f)
                return false;
            // The rigid mesh stores Euclidean xyz for its later model transform.
            vertex.Position = glm::vec3(sample.Position) / sample.Position.w;
            const f32 lengthSquared = glm::dot(sample.Normal, sample.Normal);
            if (lengthSquared > 1e-12f)
                vertex.Normal = sample.Normal * glm::inversesqrt(lengthSquared);
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
        if (!geometry || frame >= clip.FrameCount || !HasPaletteFrames(clip))
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

    CrowdVertexFrames BakeCrowdVertexFrames(const MeshSource& geometry, u32 submeshIndex, const CrowdClip& clip, u32 maxRecords)
    {
        const auto& parts = geometry.GetSubmeshes();
        const auto& vertices = geometry.GetVertices();
        const auto& influences = geometry.GetBoneInfluences();
        if (!HasPaletteFrames(clip) || submeshIndex >= static_cast<u32>(parts.Num()) || influences.Num() != vertices.Num())
            return {};

        const Submesh& part = parts[submeshIndex];
        const u32 vertexCount = static_cast<u32>(vertices.Num());
        const auto& indices = geometry.GetIndices();
        const u32 indexCount = static_cast<u32>(indices.Num());
        if (part.m_VertexCount == 0 || part.m_BaseVertex > vertexCount || part.m_VertexCount > vertexCount - part.m_BaseVertex ||
            part.m_IndexCount == 0 || part.m_BaseIndex > indexCount || part.m_IndexCount > indexCount - part.m_BaseIndex || part.m_IndexCount % 3u != 0)
            return {};
        // The shader derives the record directly from the absolute draw
        // vertex. An index outside this part would read another sample.
        for (u32 i = 0; i < part.m_IndexCount; ++i)
        {
            const u32 index = indices[part.m_BaseIndex + i];
            if (index < part.m_BaseVertex || index - part.m_BaseVertex >= part.m_VertexCount)
                return {};
        }

        const u32 stride = 1u + (part.m_VertexCount + 3u) / 4u;
        const u64 count = static_cast<u64>(clip.FrameCount) * stride;
        if (count > maxRecords || count > static_cast<u64>(std::numeric_limits<i32>::max()))
            return {};
        CrowdVertexFrames baked;
        baked.RecordsPerFrame = stride;
        baked.Records.SetNum(static_cast<i32>(count));
        for (u32 frame = 0; frame < clip.FrameCount; ++frame)
        {
            const u32 first = frame * stride;
            baked.Records[first].CrowdPose = { part.m_BaseVertex, stride, 0u, 0u };
            for (u32 local = 0; local < part.m_VertexCount; ++local)
            {
                const u32 vertex = part.m_BaseVertex + local;
                auto& record = baked.Records[first + 1u + local / 4u];
                const u32 lane = local % 4u;
                DeformedVertex sample;
                if (!DeformCrowdVertex(vertices[vertex], influences[vertex], clip, frame, sample))
                    return {};
                record.Transform[lane] = sample.Position;
                record.Normal[lane] = glm::vec4(sample.Normal, 0.0f);
            }
        }
        return baked;
    }
} // namespace OloEngine
