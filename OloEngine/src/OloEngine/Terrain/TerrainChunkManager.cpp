#include "OloEnginePCH.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Terrain/TerrainChunkManager.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Task/ParallelFor.h"

#include <atomic>
#include <cstdlib>

namespace OloEngine
{
    namespace
    {
        // Enabled unless the process opts out — the bisection lever for "is the
        // terrain wrong because of the GPU descent, or because of everything
        // else?". Only the exact value "1" disables, matching the scheduler's
        // OLO_GAMEPLAY_SCHEDULER_SEQUENTIAL.
        bool GpuDrivenLODDefault()
        {
            // This used to be a raw std::getenv under a long NOSONAR arguing
            // cpp:S990 was safe here. SonarCloud rejected the suppression and
            // failed the quality gate on it (D reliability on new code), which
            // was the right call for the wrong reason: the argument was sound,
            // but "every site justifies its own getenv" is not a scheme that
            // survives contact with 30 more sites.
            //
            // The engine now has exactly one getenv, in Core/Environment.cpp,
            // and the levers on top of it are enumerable — see
            // Core/DebugLevers.inl.
            return !Levers::TerrainCpuLod();
        }

        // Sequentially consistent by default rather than relaxed. A relaxed
        // toggle would be correct — the flag orders no other data — but it is
        // read once per terrain per frame, so the ordering buys nothing
        // measurable and the explicit argument is only a thing to get wrong.
        std::atomic<bool> s_GpuDrivenLODEnabled{ GpuDrivenLODDefault() };
    } // namespace

    void TerrainChunkManager::SetGpuDrivenLODEnabled(bool enabled)
    {
        s_GpuDrivenLODEnabled.store(enabled);
    }

    bool TerrainChunkManager::IsGpuDrivenLODEnabled()
    {
        return s_GpuDrivenLODEnabled.load();
    }

    void TerrainChunkManager::GenerateAllChunks(const TerrainData& terrainData,
                                                f32 worldSizeX, f32 worldSizeZ, f32 heightScale)
    {
        OLO_PROFILE_FUNCTION();

        u32 resolution = terrainData.GetResolution();
        if (resolution == 0)
        {
            OLO_CORE_WARN("TerrainChunkManager: Cannot generate chunks — heightmap resolution is 0");
            m_Chunks.clear();
            m_SelectedChunks.clear();
            m_NumChunksX = 0;
            m_NumChunksZ = 0;
            return;
        }

        // Determine chunk grid size based on heightmap resolution (ceil to ensure full coverage)
        m_NumChunksX = std::max(1u, (resolution + TerrainChunk::CHUNK_RESOLUTION - 1) / TerrainChunk::CHUNK_RESOLUTION);
        m_NumChunksZ = std::max(1u, (resolution + TerrainChunk::CHUNK_RESOLUTION - 1) / TerrainChunk::CHUNK_RESOLUTION);

        u32 totalChunks = m_NumChunksX * m_NumChunksZ;
        m_Chunks.resize(totalChunks);

        // Build chunk geometry in parallel (CPU only, no GL calls)
        ParallelFor("TerrainChunkBuild", static_cast<i32>(totalChunks),
                    [this, &terrainData, worldSizeX, worldSizeZ, heightScale](i32 index)
                    {
                        u32 cx = static_cast<u32>(index) % m_NumChunksX;
                        u32 cz = static_cast<u32>(index) / m_NumChunksX;
                        m_Chunks[static_cast<sizet>(index)].BuildGeometry(
                            terrainData, cx, cz, m_NumChunksX, m_NumChunksZ,
                            worldSizeX, worldSizeZ, heightScale);
                    });

        // Upload to GPU sequentially on the main/GL thread
        for (auto& chunk : m_Chunks)
        {
            chunk.UploadToGPU();
        }

        // Build quadtree for LOD selection
        // Max depth is ceil(log2) of number of chunks on one axis (so leaf = one chunk)
        u32 quadtreeDepth = 0;
        {
            u32 n = std::max(m_NumChunksX, m_NumChunksZ);
            while ((1u << quadtreeDepth) < n)
            {
                ++quadtreeDepth;
            }
        }
        quadtreeDepth = std::max(quadtreeDepth, 2u); // At least 2 levels
        m_Quadtree.Build(terrainData, worldSizeX, worldSizeZ, heightScale, quadtreeDepth);

        // The GPU tree is NOT clamped to TerrainLODConfig::MAX_LOD_LEVELS
        // (issue #714). That cap exists because the CPU descent's cost is a
        // per-frame single-threaded walk; the GPU descent is one dispatch per
        // level over a worklist the GPU sizes itself, so the useful depth is set
        // by the heightmap, not by a frame budget. Its ceiling is the node
        // pyramid's own limit.
        const u32 gpuDepth = std::clamp(quadtreeDepth, 2u, TerrainGPUQuadtree::kMaxDepth);
        if (!m_GPUQuadtree)
        {
            m_GPUQuadtree = Ref<TerrainGPUQuadtree>::Create();
        }
        m_GPUQuadtree->Build(TerrainQuadtree::BuildHeightPyramid(terrainData, heightScale, gpuDepth),
                             gpuDepth, worldSizeX, worldSizeZ);

        OLO_CORE_INFO("TerrainChunkManager: Built {}x{} chunks ({} total), quadtree depth {} (GPU depth {})",
                      m_NumChunksX, m_NumChunksZ, totalChunks, quadtreeDepth, gpuDepth);
    }

    void TerrainChunkManager::RebuildChunk(const TerrainData& terrainData, u32 chunkX, u32 chunkZ,
                                           f32 worldSizeX, f32 worldSizeZ, f32 heightScale)
    {
        OLO_PROFILE_FUNCTION();

        if (chunkX >= m_NumChunksX || chunkZ >= m_NumChunksZ)
        {
            return;
        }

        sizet index = static_cast<sizet>(chunkZ) * m_NumChunksX + chunkX;
        m_Chunks[index].Build(terrainData, chunkX, chunkZ, m_NumChunksX, m_NumChunksZ,
                              worldSizeX, worldSizeZ, heightScale);
    }

    void TerrainChunkManager::SelectVisibleChunks(const Frustum& frustum,
                                                  const glm::vec3& cameraPos,
                                                  const glm::mat4& viewProjection,
                                                  f32 viewportHeight)
    {
        OLO_PROFILE_FUNCTION();

        m_SelectedChunks.clear();

        // Whichever path runs LAST owns this frame's selection. Without this the
        // submission side would keep taking the GPU branch off a stale node list
        // the moment the GPU descent had ever succeeded once.
        if (m_GPUQuadtree)
        {
            m_GPUQuadtree->ClearDispatched();
        }

        // Run quadtree LOD selection
        m_Quadtree.SelectLOD(frustum, cameraPos, viewProjection, viewportHeight);

        const auto& selectedNodes = m_Quadtree.GetSelectedNodes();
        m_SelectedChunks.reserve(selectedNodes.size());

        // A selected node is NOT always one chunk. The tree is built to depth
        // ceil(log2(numChunks)) so that a LEAF is one chunk, but selection stops
        // wherever the screen-space error is already small enough — so a node
        // selected above the leaf level covers a 2^k x 2^k block of them.
        // Emitting only the chunk under its centre left the rest of that block
        // undrawn: a hole, growing with how coarse the selected node is, and
        // only visible on the CPU fallback path.
        //
        // The claim gate is the second half. m_NumChunksX is
        // ceil(resolution / CHUNK_RESOLUTION) and NOT necessarily a power of
        // two, so quadtree boundaries can fall inside a chunk and two adjacent
        // nodes can both overlap it. First node wins; without that the chunk
        // would be submitted twice with different LOD data.
        m_ChunkClaimed.assign(m_Chunks.size(), 0u);

        for (const auto* node : selectedNodes)
        {
            const std::optional<ChunkRange> range = RangeForNode(*node);
            if (!range)
            {
                continue;
            }

            const TerrainChunkLODData lodData = m_Quadtree.GetChunkLODData(*node);
            for (u32 cz = range->Z0; cz <= range->Z1; ++cz)
            {
                for (u32 cx = range->X0; cx <= range->X1; ++cx)
                {
                    const sizet idx = static_cast<sizet>(cz) * m_NumChunksX + cx;
                    if (m_ChunkClaimed[idx] != 0u)
                    {
                        continue;
                    }
                    const TerrainChunk& chunk = m_Chunks[idx];
                    if (!chunk.IsBuilt())
                    {
                        continue;
                    }
                    m_ChunkClaimed[idx] = 1u;

                    TerrainRenderChunk rc;
                    rc.Chunk = &chunk;
                    rc.LODData = lodData;
                    m_SelectedChunks.push_back(rc);
                }
            }
        }
    }

    bool TerrainChunkManager::DispatchGPULOD(const Frustum& frustum,
                                             const glm::vec3& cameraPos,
                                             const glm::mat4& viewProjection,
                                             f32 viewportHeight,
                                             f32 targetTriangleSize)
    {
        OLO_PROFILE_FUNCTION();

        if (!IsGpuDrivenLODEnabled() || !m_GPUQuadtree || !m_GPUQuadtree->IsBuilt())
        {
            return false;
        }

        TerrainGPUQuadtree::CullInputs inputs;
        inputs.ViewFrustum = frustum;
        inputs.CameraPos = cameraPos;
        inputs.ViewProjection = viewProjection;
        inputs.ViewportHeight = viewportHeight;
        inputs.TargetTriangleSize = targetTriangleSize;

        if (!m_GPUQuadtree->Dispatch(inputs))
        {
            return false;
        }

        // Nothing per-chunk is produced on this path. Clearing makes that
        // explicit: a consumer that still reads GetSelectedChunks() gets an
        // empty list and draws nothing, rather than silently re-drawing the
        // stale selection from whichever frame last ran the CPU descent.
        m_SelectedChunks.clear();
        return true;
    }

    Ref<TerrainGPUPicker>& TerrainChunkManager::GetOrCreateGPUPicker()
    {
        if (!m_GPUPicker)
        {
            m_GPUPicker = Ref<TerrainGPUPicker>::Create();
        }
        return m_GPUPicker;
    }

    void TerrainChunkManager::UpdateGPUPicking(const TerrainGPUPicker::TerrainInputs& terrain)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_GPUPicker)
        {
            return;
        }

        // Poll BEFORE dispatching, not after. Polling first retires whatever the
        // GPU finished since last frame, so this frame's consumer sees the
        // freshest answer the ring holds; polling after would also see it, but
        // would have to be told about the slot the dispatch just took and would
        // read the ring one frame later than it needs to.
        m_GPUPicker->Poll();

        if (!m_GPUQuadtree || !m_GPUQuadtree->IsBuilt())
        {
            return;
        }
        (void)m_GPUPicker->Dispatch(*m_GPUQuadtree, terrain);
    }

    void TerrainChunkManager::GetVisibleChunks(const Frustum& frustum,
                                               std::vector<const TerrainChunk*>& outChunks) const
    {
        OLO_PROFILE_FUNCTION();

        outChunks.clear();
        outChunks.reserve(m_Chunks.size());

        for (const auto& chunk : m_Chunks)
        {
            if (!chunk.IsBuilt())
            {
                continue;
            }

            const auto& bounds = chunk.GetBounds();
            if (frustum.IsBoxVisible(bounds.Min, bounds.Max))
            {
                outChunks.push_back(&chunk);
            }
        }
    }

    void TerrainChunkManager::GetAllChunks(std::vector<const TerrainChunk*>& outChunks) const
    {
        OLO_PROFILE_FUNCTION();

        outChunks.clear();
        outChunks.reserve(m_Chunks.size());

        for (const auto& chunk : m_Chunks)
        {
            if (chunk.IsBuilt())
            {
                outChunks.push_back(&chunk);
            }
        }
    }

    std::optional<TerrainChunkManager::ChunkRange> TerrainChunkManager::RangeForNode(const TerrainQuadNode& node) const
    {
        OLO_PROFILE_FUNCTION();

        if (m_NumChunksX == 0 || m_NumChunksZ == 0)
        {
            return std::nullopt;
        }

        // Node bounds are normalized [0,1] over the terrain footprint, the same
        // space the chunk grid divides. Scale into chunk indices and take the
        // INCLUSIVE cover.
        //
        // The epsilon is doing real work in both directions. A node edge that
        // should land exactly on a chunk boundary often computes to
        // 1.9999999 or 2.0000001 instead: without it, the first case drops the
        // last chunk of the range (a one-chunk hole along every node seam) and
        // the second pulls in a neighbouring chunk the adjacent node also
        // claims. It is small enough that it can only absorb representation
        // error, not a real fraction of a chunk.
        constexpr f32 kEdgeEpsilon = 1e-4f;

        auto rangeOnAxis = [](f32 minN, f32 maxN, u32 chunkCount, u32& lo, u32& hi)
        {
            const f32 count = static_cast<f32>(chunkCount);
            const f32 first = std::floor(minN * count + kEdgeEpsilon);
            const f32 last = std::ceil(maxN * count - kEdgeEpsilon) - 1.0f;
            const f32 maxIndex = static_cast<f32>(chunkCount - 1);

            lo = static_cast<u32>(std::clamp(first, 0.0f, maxIndex));
            hi = static_cast<u32>(std::clamp(last, 0.0f, maxIndex));
            // A degenerate or sub-chunk node collapses to the single chunk it
            // sits in rather than to an empty range.
            hi = std::max(hi, lo);
        };

        ChunkRange range;
        rangeOnAxis(node.MinX, node.MaxX, m_NumChunksX, range.X0, range.X1);
        rangeOnAxis(node.MinZ, node.MaxZ, m_NumChunksZ, range.Z0, range.Z1);
        return range;
    }
} // namespace OloEngine
