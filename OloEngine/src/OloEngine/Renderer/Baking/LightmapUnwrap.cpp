#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Baking/LightmapUnwrap.h"

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Instrumentor.h"
#include "OloEngine/Renderer/MeshOptimization.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Templates/UnrealTemplate.h"

#include <xatlas.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace OloEngine
{
    namespace
    {
        // One contiguous slice of the source arrays handed to xatlas as one mesh.
        // Mirrors Submesh's Base/Count fields; for a MeshSource without submeshes a
        // single synthetic range covers everything.
        struct SubmeshRange
        {
            u32 m_BaseVertex = 0;
            u32 m_BaseIndex = 0;
            u32 m_IndexCount = 0;
            u32 m_VertexCount = 0;
        };

        // RAII so every early-return failure path still frees the atlas.
        class AtlasGuard
        {
          public:
            explicit AtlasGuard(xatlas::Atlas* atlas) : m_Atlas(atlas) {}
            ~AtlasGuard()
            {
                if (m_Atlas != nullptr)
                {
                    xatlas::Destroy(m_Atlas);
                }
            }
            AtlasGuard(const AtlasGuard&) = delete;
            AtlasGuard& operator=(const AtlasGuard&) = delete;
            AtlasGuard(AtlasGuard&&) = delete;
            AtlasGuard& operator=(AtlasGuard&&) = delete;

          private:
            xatlas::Atlas* m_Atlas = nullptr;
        };

        // Validates that the submeshes' index ranges exactly partition [0, totalIndexCount):
        // no gap (an uncovered index range would be silently dropped by the rebuild) and no
        // overlap (a shared range would be duplicated). Empty submeshes are allowed and skipped.
        [[nodiscard]] bool SubmeshRangesPartitionIndexBuffer(const std::vector<SubmeshRange>& ranges, u32 totalIndexCount)
        {
            std::vector<SubmeshRange> sorted;
            sorted.reserve(ranges.size());
            for (const SubmeshRange& range : ranges)
            {
                if (range.m_IndexCount > 0)
                {
                    sorted.push_back(range);
                }
            }
            std::sort(sorted.begin(), sorted.end(),
                      [](const SubmeshRange& a, const SubmeshRange& b)
                      { return a.m_BaseIndex < b.m_BaseIndex; });

            u32 expectedNext = 0;
            for (const SubmeshRange& range : sorted)
            {
                if (range.m_BaseIndex != expectedNext)
                {
                    return false;
                }
                expectedNext += range.m_IndexCount;
            }
            return expectedNext == totalIndexCount;
        }
    } // namespace

    bool LightmapUnwrap::Generate(MeshSource& meshSource, const LightmapUnwrapOptions& options)
    {
        OLO_PROFILE_FUNCTION();

        // Idempotence: an existing complete UV2 stream is left alone.
        if (meshSource.HasLightmapUVs())
        {
            return true;
        }

        // Refusals — the rebuild would leave these parallel/per-index data stale.
        if (meshSource.HasSkeleton() || meshSource.HasBoneInfluences())
        {
            OLO_CORE_WARN("LightmapUnwrap::Generate: refused — mesh has a skeleton or bone influences "
                          "(skinned geometry samples probes, not lightmaps)");
            return false;
        }
        if (meshSource.HasMorphTargets())
        {
            OLO_CORE_WARN("LightmapUnwrap::Generate: refused — mesh has morph targets "
                          "(their per-vertex deltas would go stale)");
            return false;
        }
        if (meshSource.HasVirtualMeshBlob())
        {
            OLO_CORE_WARN("LightmapUnwrap::Generate: refused — mesh has a cooked virtual-mesh blob "
                          "(its cluster DAG references the original vertex order)");
            return false;
        }

        const MeshSource& source = meshSource; // const view: read through const accessors only
        const TArray<Vertex>& vertices = source.GetVertices();
        const TArray<u32>& indices = source.GetIndices();

        if (vertices.IsEmpty() || indices.IsEmpty())
        {
            OLO_CORE_WARN("LightmapUnwrap::Generate: refused — mesh has no vertices or no indices");
            return false;
        }
        if (indices.Num() % 3 != 0)
        {
            OLO_CORE_WARN("LightmapUnwrap::Generate: refused — index count {} is not a multiple of 3", indices.Num());
            return false;
        }

        const u32 totalVertexCount = static_cast<u32>(vertices.Num());
        const u32 totalIndexCount = static_cast<u32>(indices.Num());
        const TArray<Submesh>& submeshes = source.GetSubmeshes();

        // ── Collect + validate ranges ────────────────────────────────────────
        std::vector<SubmeshRange> ranges;
        if (submeshes.IsEmpty())
        {
            ranges.push_back(SubmeshRange{ 0, 0, totalIndexCount, totalVertexCount });
        }
        else
        {
            ranges.reserve(static_cast<sizet>(submeshes.Num()));
            for (i32 s = 0; s < submeshes.Num(); ++s)
            {
                const Submesh& sub = submeshes[s];
                if (sub.m_BaseIndex > totalIndexCount || sub.m_IndexCount > totalIndexCount - sub.m_BaseIndex)
                {
                    OLO_CORE_WARN("LightmapUnwrap::Generate: submesh {} index range [{}, +{}) exceeds index buffer ({})",
                                  s, sub.m_BaseIndex, sub.m_IndexCount, totalIndexCount);
                    return false;
                }
                if (sub.m_IndexCount % 3 != 0)
                {
                    OLO_CORE_WARN("LightmapUnwrap::Generate: submesh {} index count {} is not a multiple of 3",
                                  s, sub.m_IndexCount);
                    return false;
                }
                if (sub.m_IndexCount > 0 &&
                    (sub.m_BaseVertex > totalVertexCount || sub.m_VertexCount > totalVertexCount - sub.m_BaseVertex ||
                     sub.m_VertexCount == 0))
                {
                    OLO_CORE_WARN("LightmapUnwrap::Generate: submesh {} vertex range [{}, +{}) exceeds vertex buffer ({})",
                                  s, sub.m_BaseVertex, sub.m_VertexCount, totalVertexCount);
                    return false;
                }
                ranges.push_back(SubmeshRange{ sub.m_BaseVertex, sub.m_BaseIndex, sub.m_IndexCount, sub.m_VertexCount });
            }
            if (!SubmeshRangesPartitionIndexBuffer(ranges, totalIndexCount))
            {
                OLO_CORE_WARN("LightmapUnwrap::Generate: submesh index ranges do not exactly partition the index "
                              "buffer — the rebuild would drop or duplicate triangles; refusing");
                return false;
            }
        }

        // Every global index must sit inside its submesh's declared vertex window — the
        // same contract AnimatedModel enforces — because the xatlas input is the
        // submesh-relative slice.
        for (sizet r = 0; r < ranges.size(); ++r)
        {
            const SubmeshRange& range = ranges[r];
            for (u32 i = 0; i < range.m_IndexCount; ++i)
            {
                const u32 v = indices[static_cast<i32>(range.m_BaseIndex + i)];
                if (v < range.m_BaseVertex || v - range.m_BaseVertex >= range.m_VertexCount)
                {
                    OLO_CORE_WARN("LightmapUnwrap::Generate: submesh {} index {} references vertex {} outside "
                                  "its vertex range [{}, +{})",
                                  r, range.m_BaseIndex + i, v, range.m_BaseVertex, range.m_VertexCount);
                    return false;
                }
            }
        }

        // ── Sanitize options ─────────────────────────────────────────────────
        constexpr u32 kMaxResolution = 16384;
        constexpr u32 kMaxPadding = 64;
        const u32 resolution = std::min(options.Resolution, kMaxResolution);
        const u32 padding = std::min(options.Padding, kMaxPadding);
        f32 texelsPerUnit = options.TexelsPerUnit;
        if (!std::isfinite(texelsPerUnit) || texelsPerUnit < 0.0f)
        {
            texelsPerUnit = 0.0f;
        }

        // ── Run xatlas: one xatlas mesh per non-empty submesh, one shared atlas ──
        xatlas::Atlas* atlas = xatlas::Create();
        if (atlas == nullptr)
        {
            OLO_CORE_ERROR("LightmapUnwrap::Generate: xatlas::Create failed");
            return false;
        }
        const AtlasGuard atlasGuard(atlas);

        u32 addedMeshCount = 0;
        for (const SubmeshRange& range : ranges)
        {
            if (range.m_IndexCount == 0)
            {
                continue;
            }
            ++addedMeshCount;
        }
        if (addedMeshCount == 0)
        {
            OLO_CORE_WARN("LightmapUnwrap::Generate: refused — every submesh is empty");
            return false;
        }

        // Submesh-relative index buffers. Kept alive until xatlas::Generate returns —
        // xatlas documents that MeshDecl data is copied by AddMesh, but with async
        // AddMesh processing the storage is cheap insurance.
        std::vector<std::vector<u32>> relativeIndexStorage;
        relativeIndexStorage.reserve(ranges.size());

        for (sizet r = 0; r < ranges.size(); ++r)
        {
            const SubmeshRange& range = ranges[r];
            if (range.m_IndexCount == 0)
            {
                continue;
            }

            std::vector<u32>& relativeIndices = relativeIndexStorage.emplace_back();
            relativeIndices.resize(range.m_IndexCount);
            for (u32 i = 0; i < range.m_IndexCount; ++i)
            {
                relativeIndices[i] = indices[static_cast<i32>(range.m_BaseIndex + i)] - range.m_BaseVertex;
            }

            const Vertex* firstVertex = &vertices[static_cast<i32>(range.m_BaseVertex)];

            xatlas::MeshDecl meshDecl;
            meshDecl.vertexPositionData = &firstVertex->Position.x;
            meshDecl.vertexPositionStride = sizeof(Vertex);
            meshDecl.vertexNormalData = &firstVertex->Normal.x;
            meshDecl.vertexNormalStride = sizeof(Vertex);
            meshDecl.vertexCount = range.m_VertexCount;
            meshDecl.indexData = relativeIndices.data();
            meshDecl.indexCount = range.m_IndexCount;
            meshDecl.indexFormat = xatlas::IndexFormat::UInt32;

            const xatlas::AddMeshError addError = xatlas::AddMesh(atlas, meshDecl, addedMeshCount);
            if (addError != xatlas::AddMeshError::Success)
            {
                OLO_CORE_WARN("LightmapUnwrap::Generate: xatlas::AddMesh failed for submesh {} (error {})",
                              r, static_cast<u32>(addError));
                return false;
            }
        }

        // Default ChartOptions/PackOptions are the deterministic configuration; only the
        // pack sizing knobs are set from LightmapUnwrapOptions.
        const xatlas::ChartOptions chartOptions{};
        xatlas::PackOptions packOptions{};
        packOptions.resolution = resolution;
        packOptions.padding = padding;
        packOptions.texelsPerUnit = texelsPerUnit;
        xatlas::Generate(atlas, chartOptions, packOptions);

        if (atlas->atlasCount != 1)
        {
            OLO_CORE_WARN("LightmapUnwrap::Generate: xatlas produced {} atlas pages (want exactly 1) — charts "
                          "did not fit a {}x{} page; raise LightmapUnwrapOptions::Resolution at the call site",
                          atlas->atlasCount, resolution, resolution);
            return false;
        }
        if (atlas->width == 0 || atlas->height == 0)
        {
            OLO_CORE_WARN("LightmapUnwrap::Generate: xatlas produced a zero-sized atlas ({}x{})",
                          atlas->width, atlas->height);
            return false;
        }
        if (atlas->meshCount != addedMeshCount)
        {
            OLO_CORE_WARN("LightmapUnwrap::Generate: xatlas returned {} meshes for {} inputs",
                          atlas->meshCount, addedMeshCount);
            return false;
        }

        // ── Rebuild into locals (commit only after every check passes) ───────
        const f32 invAtlasWidth = 1.0f / static_cast<f32>(atlas->width);
        const f32 invAtlasHeight = 1.0f / static_cast<f32>(atlas->height);

        u32 outputVertexTotal = 0;
        u32 outputIndexTotal = 0;
        for (u32 m = 0; m < atlas->meshCount; ++m)
        {
            outputVertexTotal += atlas->meshes[m].vertexCount;
            outputIndexTotal += atlas->meshes[m].indexCount;
        }

        TArray<Vertex> newVertices;
        TArray<u32> newIndices;
        TArray<glm::vec2> newLightmapUVs;
        newVertices.Reserve(static_cast<i32>(outputVertexTotal));
        newIndices.Reserve(static_cast<i32>(outputIndexTotal));
        newLightmapUVs.Reserve(static_cast<i32>(outputVertexTotal));
        TArray<Submesh> newSubmeshes = submeshes; // copy; Base/Count fields patched below

        u32 xatlasMeshIndex = 0;
        for (sizet r = 0; r < ranges.size(); ++r)
        {
            const SubmeshRange& range = ranges[r];
            const u32 newBaseVertex = static_cast<u32>(newVertices.Num());
            const u32 newBaseIndex = static_cast<u32>(newIndices.Num());
            u32 newVertexCount = 0;
            u32 newIndexCount = 0;

            if (range.m_IndexCount > 0)
            {
                const xatlas::Mesh& outputMesh = atlas->meshes[xatlasMeshIndex];
                ++xatlasMeshIndex;

                if (outputMesh.chartCount == 0)
                {
                    OLO_CORE_WARN("LightmapUnwrap::Generate: xatlas produced 0 charts for submesh {} — "
                                  "whole-mesh failure (all-or-nothing)",
                                  r);
                    return false;
                }
                if (outputMesh.indexCount != range.m_IndexCount)
                {
                    OLO_CORE_WARN("LightmapUnwrap::Generate: xatlas changed submesh {}'s index count "
                                  "({} -> {}) — refusing",
                                  r, range.m_IndexCount, outputMesh.indexCount);
                    return false;
                }

                for (u32 v = 0; v < outputMesh.vertexCount; ++v)
                {
                    const xatlas::Vertex& outputVertex = outputMesh.vertexArray[v];
                    if (outputVertex.atlasIndex != 0)
                    {
                        OLO_CORE_WARN("LightmapUnwrap::Generate: submesh {} vertex {} was not packed onto "
                                      "atlas page 0 (page {}) — likely a degenerate/unchartable face; refusing",
                                      r, v, outputVertex.atlasIndex);
                        return false;
                    }
                    if (outputVertex.xref >= range.m_VertexCount)
                    {
                        OLO_CORE_WARN("LightmapUnwrap::Generate: submesh {} xref {} out of range ({})",
                                      r, outputVertex.xref, range.m_VertexCount);
                        return false;
                    }

                    // xref preservation: the new vertex is a bit-exact copy of the original.
                    newVertices.Add(vertices[static_cast<i32>(range.m_BaseVertex + outputVertex.xref)]);
                    // xatlas uvs are in texels of the (actual) atlas — normalize; clamp only
                    // guards float edge cases at the border, values are already in range.
                    newLightmapUVs.Add(glm::vec2(std::clamp(outputVertex.uv[0] * invAtlasWidth, 0.0f, 1.0f),
                                                 std::clamp(outputVertex.uv[1] * invAtlasHeight, 0.0f, 1.0f)));
                }

                for (u32 i = 0; i < outputMesh.indexCount; ++i)
                {
                    const u32 relative = outputMesh.indexArray[i];
                    if (relative >= outputMesh.vertexCount)
                    {
                        OLO_CORE_WARN("LightmapUnwrap::Generate: submesh {} output index {} out of range ({})",
                                      r, relative, outputMesh.vertexCount);
                        return false;
                    }
                    // Indices stay GLOBAL into the rebuilt vertex array (engine contract).
                    newIndices.Add(newBaseVertex + relative);
                }

                newVertexCount = outputMesh.vertexCount;
                newIndexCount = outputMesh.indexCount;
            }

            if (!newSubmeshes.IsEmpty())
            {
                Submesh& patched = newSubmeshes[static_cast<i32>(r)];
                patched.m_BaseVertex = newBaseVertex;
                patched.m_BaseIndex = newBaseIndex;
                patched.m_VertexCount = newVertexCount;
                patched.m_IndexCount = newIndexCount;
            }
        }

        // ── Commit ───────────────────────────────────────────────────────────
        const bool hadShadowIndices = meshSource.HasShadowIndices();
        const u32 committedVertexCount = static_cast<u32>(newVertices.Num());

        meshSource.GetVertices() = MoveTemp(newVertices);
        meshSource.GetIndices() = MoveTemp(newIndices);
        // Keep the parallel bone-influence stream sized to the vertices (all zero weights —
        // real influences were refused above), matching the constructors' invariant. A source
        // that never had the stream keeps not having it.
        if (!meshSource.GetBoneInfluences().IsEmpty())
        {
            meshSource.GetBoneInfluences().SetNum(static_cast<i32>(committedVertexCount));
        }
        meshSource.SetLightmapUVs(MoveTemp(newLightmapUVs));
        // SetSubmeshes marks the source un-Built and recalculates mesh + submesh bounds
        // against the rebuilt arrays (positions unchanged, ranges moved). Also correct for
        // the no-submesh path (empty copy, same invalidation).
        meshSource.SetSubmeshes(newSubmeshes);
        // The old shadow indices referenced the old vertex order — regenerate them from the
        // rebuilt arrays (needs the NEW submesh ranges, so this runs after SetSubmeshes).
        if (hadShadowIndices)
        {
            MeshOptimization::GenerateShadowIndices(meshSource);
        }

        OLO_CORE_TRACE("LightmapUnwrap::Generate: unwrapped {} -> {} vertices, {} indices, {} charts, "
                       "{}x{} atlas, {:.1f}% utilization",
                       totalVertexCount, committedVertexCount, totalIndexCount, atlas->chartCount,
                       atlas->width, atlas->height,
                       atlas->utilization != nullptr ? atlas->utilization[0] * 100.0f : 0.0f);
        return true;
    }
} // namespace OloEngine
