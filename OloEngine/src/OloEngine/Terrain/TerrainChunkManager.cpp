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
            // NOSONAR cpp:S990 — the rule flags getenv because the pointer it
            // returns can be invalidated by a concurrent setenv/putenv, and
            // there is no portable thread-safe alternative in standard C++
            // (Windows has GetEnvironmentVariableA; POSIX has nothing, so any
            // cross-platform helper still bottoms out here).
            //
            // Be precise about why it is safe, because the obvious claim is
            // wrong: putenv DOES exist in this repo — OloEngineTest.cpp's main
            // calls _putenv_s/setenv to force a diagnostic on. That call cannot
            // race this one. This is a static initialiser, so it runs BEFORE
            // main and before any thread exists, and the value is consumed
            // immediately rather than stored, so no pointer outlives the call.
            //
            // (Renderer/RHI/RHIDescriptorHeap.cpp carries the same suppression
            // and scopes its claim to OloEngine/src + OloEditor/src, where it
            // does hold. Promoting that file-local IsTruthyEnvironmentVariable
            // helper into a shared header would reduce the engine's ~32 getenv
            // sites to one — worth doing, but not from a terrain branch while
            // the RHI is being edited elsewhere.)
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

        for (const auto* node : selectedNodes)
        {
            const TerrainChunk* chunk = FindChunkForNode(*node);
            if (chunk && chunk->IsBuilt())
            {
                TerrainRenderChunk rc;
                rc.Chunk = chunk;
                rc.LODData = m_Quadtree.GetChunkLODData(*node);
                m_SelectedChunks.push_back(rc);
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

    const TerrainChunk* TerrainChunkManager::FindChunkForNode(const TerrainQuadNode& node) const
    {
        OLO_PROFILE_FUNCTION();

        if (m_NumChunksX == 0 || m_NumChunksZ == 0)
        {
            return nullptr;
        }

        // Map the quadtree node center to a chunk grid coordinate
        f32 centerX = (node.MinX + node.MaxX) * 0.5f;
        f32 centerZ = (node.MinZ + node.MaxZ) * 0.5f;

        u32 cx = static_cast<u32>(centerX * static_cast<f32>(m_NumChunksX));
        u32 cz = static_cast<u32>(centerZ * static_cast<f32>(m_NumChunksZ));
        cx = std::min(cx, m_NumChunksX - 1);
        cz = std::min(cz, m_NumChunksZ - 1);

        sizet idx = static_cast<sizet>(cz) * m_NumChunksX + cx;
        return &m_Chunks[idx];
    }
} // namespace OloEngine
