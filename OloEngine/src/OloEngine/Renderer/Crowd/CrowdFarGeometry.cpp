#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Crowd/CrowdGeometry.h"

#include "OloEngine/Math/Math.h"

#include <algorithm>
#include <array>
#include <limits>
#include <span>

namespace OloEngine
{
    namespace
    {
        // A far actor covers only a handful of pixels. Represent each material
        // with up to eight volumetric splats. Their support vertices belong to
        // the original surface, so tails and limbs keep their own skinning and
        // the same texture coordinates in every view, including a shadow view.
        constexpr u32 kMaxSplats = 8;
        constexpr std::array<u32, 36> kSplatIndices{
            0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5, 0, 4, 5, 0, 5, 1,
            2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3
        };

        struct TriangleSample
        {
            std::array<u32, 3> Corners;
            glm::dvec3 Center;
            u32 Ordinal;
        };

        void EmitSplat(std::span<const TriangleSample> patch, const MeshSource& source, TArray<u32>& indices)
        {
            glm::dvec3 lo(source.GetVertices()[patch.front().Corners[0]].Position), hi = lo;
            for (const auto& triangle : patch)
                for (const u32 corner : triangle.Corners)
                {
                    const glm::dvec3 p(source.GetVertices()[corner].Position);
                    lo = glm::min(lo, p);
                    hi = glm::max(hi, p);
                }
            // Normalise the three support directions per patch. A narrow leg
            // then has a footprint even when its length dominates its width.
            glm::dvec3 inverseExtent(0.0);
            for (u32 axis = 0; axis < 3; ++axis)
                if (hi[axis] > lo[axis])
                    inverseExtent[axis] = 1.0 / (hi[axis] - lo[axis]);

            std::array<u32, 8> support{};
            std::array<f64, 8> scores;
            scores.fill(std::numeric_limits<f64>::lowest());
            for (const auto& triangle : patch)
                for (const u32 corner : triangle.Corners)
                {
                    const glm::dvec3 p = (glm::dvec3(source.GetVertices()[corner].Position) - lo) * inverseExtent;
                    for (u32 i = 0; i < support.size(); ++i)
                    {
                        const f64 score = ((i & 4u) ? p.x : -p.x) + ((i & 2u) ? p.y : -p.y) + ((i & 1u) ? p.z : -p.z);
                        if (score > scores[i] || (!(score < scores[i]) && corner < support[i]))
                        {
                            scores[i] = score;
                            support[i] = corner;
                        }
                    }
                }
            for (u32 i = 0; i < kSplatIndices.size(); i += 3)
            {
                const u32 a = support[kSplatIndices[i]], b = support[kSplatIndices[i + 1]], c = support[kSplatIndices[i + 2]];
                if (a != b && b != c && a != c)
                {
                    indices.Add(a);
                    indices.Add(b);
                    indices.Add(c);
                }
            }
        }

        void PartitionSplats(std::span<TriangleSample> triangles, u32 count, const MeshSource& source, TArray<u32>& indices)
        {
            if (count == 1)
            {
                EmitSplat(triangles, source, indices);
                return;
            }
            glm::dvec3 lo = triangles.front().Center, hi = lo;
            for (const auto& triangle : triangles)
            {
                lo = glm::min(lo, triangle.Center);
                hi = glm::max(hi, triangle.Center);
            }
            const glm::dvec3 extent = hi - lo;
            const u32 axis = extent.x >= extent.y && extent.x >= extent.z ? 0u : extent.y >= extent.z ? 1u
                                                                                                      : 2u;
            const u32 leftCount = count / 2u;
            const sizet split = (triangles.size() * leftCount) / count;
            std::nth_element(triangles.begin(), triangles.begin() + split, triangles.end(),
                             [axis](const TriangleSample& a, const TriangleSample& b)
                             {
                                 if (a.Center[axis] < b.Center[axis])
                                     return true;
                                 if (b.Center[axis] < a.Center[axis])
                                     return false;
                                 return a.Ordinal < b.Ordinal;
                             });
            PartitionSplats(triangles.first(split), leftCount, source, indices);
            PartitionSplats(triangles.subspan(split), count - leftCount, source, indices);
        }

        bool SelectPartIndices(const MeshSource& source, const Submesh& part, TArray<u32>& selected)
        {
            const auto vertexCount = static_cast<u32>(source.GetVertices().Num());
            const auto indexCount = static_cast<u32>(source.GetIndices().Num());
            if (part.m_BaseVertex > vertexCount || part.m_VertexCount > vertexCount - part.m_BaseVertex ||
                part.m_BaseIndex > indexCount || part.m_IndexCount > indexCount - part.m_BaseIndex ||
                part.m_IndexCount % 3 != 0)
                return false;
            const u32 faces = part.m_IndexCount / 3;
            TArray<TriangleSample> triangles;
            triangles.Reserve(faces);
            for (u32 face = 0; face < faces; ++face)
            {
                TriangleSample triangle{ {}, glm::dvec3(0.0), face };
                for (u32 i = 0; i < 3; ++i)
                {
                    const u32 corner = source.GetIndices()[part.m_BaseIndex + face * 3 + i];
                    if (corner < part.m_BaseVertex || corner - part.m_BaseVertex >= part.m_VertexCount ||
                        !Math::IsFinite(source.GetVertices()[corner].Position))
                        return false;
                    triangle.Corners[i] = corner;
                    triangle.Center += glm::dvec3(source.GetVertices()[corner].Position) / 3.0;
                }
                triangles.Add(triangle);
            }
            const i32 before = selected.Num();
            if (faces > kSplatIndices.size() / 3)
            {
                const u32 splatCount = std::clamp(faces / 48u, 1u, kMaxSplats);
                PartitionSplats({ triangles.GetData(), faces }, splatCount, source, selected);
            }
            // A small accessory may already be cheaper than one splat; retain
            // its triangles, including their winding and original UV seams.
            if (selected.Num() == before && part.m_IndexCount > 0)
                selected.Append(source.GetIndices().GetData() + part.m_BaseIndex, part.m_IndexCount);
            return true;
        }
    } // namespace

    Ref<MeshSource> MakeCrowdFarGeometry(const Ref<MeshSource>& source)
    {
        if (!source || source->GetSubmeshes().IsEmpty() || source->GetBoneInfluences().Num() != source->GetVertices().Num())
            return source;

        TArray<Vertex> vertices;
        TArray<BoneInfluence> weights;
        TArray<glm::vec2> lightmapUVs;
        TArray<u32> indices;
        auto parts = source->GetSubmeshes();
        for (Submesh& part : parts)
        {
            TArray<u32> selected;
            if (!SelectPartIndices(*source, part, selected))
                return source;

            const u32 sourceBase = part.m_BaseVertex;
            TArray<i32> remap;
            remap.Init(-1, static_cast<i32>(part.m_VertexCount));
            part.m_BaseVertex = static_cast<u32>(vertices.Num());
            part.m_BaseIndex = static_cast<u32>(indices.Num());
            for (const u32 original : selected)
            {
                i32& index = remap[original - sourceBase];
                if (index < 0)
                {
                    index = vertices.Add(source->GetVertices()[original]);
                    weights.Add(source->GetBoneInfluences()[original]);
                    if (source->GetLightmapUVs().Num() == source->GetVertices().Num())
                        lightmapUVs.Add(source->GetLightmapUVs()[original]);
                }
                indices.Add(static_cast<u32>(index));
            }
            part.m_VertexCount = static_cast<u32>(vertices.Num()) - part.m_BaseVertex;
            part.m_IndexCount = static_cast<u32>(selected.Num());
        }
        if (indices.Num() >= source->GetIndices().Num() || indices.IsEmpty())
            return source;
        auto result = Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));
        result->GetBoneInfluences() = MoveTemp(weights);
        result->GetLightmapUVs() = MoveTemp(lightmapUVs);
        result->SetSkeleton(source->GetSkeletonRef());
        result->GetBoneInfo() = source->GetBoneInfo();
        result->SetSourceIsRigged(source->IsSourceRigged());
        result->SetSubmeshes(parts);
        for (const auto& [slot, handle] : source->GetMaterials())
            result->SetMaterial(slot, handle);
        TArray<Ref<Material>> materials;
        for (const auto& material : source->GetImportedMaterials())
            materials.Add(material);
        result->SetImportedMaterials(MoveTemp(materials));
        result->SetPreOptimized(true);
        result->Build();
        return result;
    }
} // namespace OloEngine
