#include "OloEnginePCH.h"
#include "OloEngine/Terrain/TerrainGPUQuadtree.h"

#include "OloEngine/Renderer/Buffer.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Terrain/TerrainVertex.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

namespace OloEngine
{
    namespace
    {
        // Must match TerrainNodeSelect.comp / TerrainSeamMap.comp — and
        // TerrainCullArgs.comp, which is where the division actually happens
        // (it turns a node count into a group count without asking the CPU).
        constexpr u32 kSelectWorkgroupSize = 64;
        static_assert(kSelectWorkgroupSize == 64,
                      "TerrainCullArgs.comp hard-codes /64 when writing the dispatch arguments");
        // Must match TerrainLODMap.comp's local_size_x/y.
        constexpr u32 kLODMapWorkgroupSize = 8;

        // How often PollOverflow() is allowed to do its GPU->CPU read. Once
        // every few seconds is plenty for a diagnostic, and anything tighter
        // reintroduces the stall this class removes.
        constexpr u32 kOverflowPollInterval = 240;

        // C++ twin of TerrainCullState in include/TerrainCullParams.glsl. The
        // byte offsets of the two uvec3 members are passed straight to
        // DispatchComputeIndirect, so they are part of the contract, not an
        // implementation detail.
        struct TerrainGpuCullState
        {
            u32 PendingCount = 0;
            u32 NextCount = 0;
            u32 VisibleCount = 0;
            u32 OverflowFlags = 0;
            u32 SelectDispatchX = 0;
            u32 SelectDispatchY = 0;
            u32 SelectDispatchZ = 0;
            u32 _StatePad0 = 0;
            u32 SeamDispatchX = 0;
            u32 SeamDispatchY = 0;
            u32 SeamDispatchZ = 0;
            u32 _StatePad1 = 0;
        };
        static_assert(sizeof(TerrainGpuCullState) == 48, "TerrainGpuCullState must match the std430 TerrainCullState block");
        static_assert(offsetof(TerrainGpuCullState, SelectDispatchX) == 16, "SelectDispatch offset is part of the DispatchComputeIndirect contract");
        static_assert(offsetof(TerrainGpuCullState, SeamDispatchX) == 32, "SeamDispatch offset is part of the DispatchComputeIndirect contract");

        constexpr u32 kSelectDispatchOffset = 16;
        constexpr u32 kSeamDispatchOffset = 32;

        constexpr u32 kOverflowNodes = 1u;
        constexpr u32 kOverflowVisible = 2u;

        // DrawElementsIndirectCommand padded to the std430 block in
        // TerrainCullArgs.comp (5 live fields + 3 pad uints).
        struct TerrainDrawArgsPOD
        {
            u32 Count = 0;
            u32 InstanceCount = 0;
            u32 FirstIndex = 0;
            u32 BaseVertex = 0;
            u32 BaseInstance = 0;
            u32 Pad0 = 0;
            u32 Pad1 = 0;
            u32 Pad2 = 0;
        };
        static_assert(sizeof(TerrainDrawArgsPOD) == 32, "TerrainDrawArgsPOD must match the std430 TerrainDrawArgs block");

        // The shared unit-space patch grid. Process-wide because it carries no
        // terrain-specific data — every visible node of every terrain instances
        // the same [0,1]^2 topology and derives its world rect in the vertex
        // stage from the node coord.
        Ref<VertexArray> s_PatchMesh;
        u32 s_PatchIndexCount = 0;

        void BuildSharedPatchMesh()
        {
            constexpr u32 K = TerrainGPUQuadtree::kPatchGridResolution;
            constexpr u32 vertsPerSide = K + 1;

            std::vector<TerrainVertex> vertices;
            vertices.reserve(static_cast<sizet>(vertsPerSide) * vertsPerSide);
            for (u32 z = 0; z < vertsPerSide; ++z)
            {
                for (u32 x = 0; x < vertsPerSide; ++x)
                {
                    const f32 u = static_cast<f32>(x) / static_cast<f32>(K);
                    const f32 v = static_cast<f32>(z) / static_cast<f32>(K);
                    // Position.xz IS the unit grid coordinate: the vertex stage
                    // rounds it back to an integer grid index to apply the seam
                    // snapping, so it must reproduce x/K exactly. Y is unused —
                    // the tess_eval stage displaces from the heightmap.
                    vertices.emplace_back(glm::vec3(u, 0.0f, v), glm::vec2(u, v), glm::vec3(0.0f, 1.0f, 0.0f));
                }
            }

            std::vector<u32> indices;
            indices.reserve(static_cast<sizet>(K) * K * 6);
            for (u32 z = 0; z < K; ++z)
            {
                for (u32 x = 0; x < K; ++x)
                {
                    const u32 topLeft = z * vertsPerSide + x;
                    const u32 topRight = topLeft + 1;
                    const u32 bottomLeft = (z + 1) * vertsPerSide + x;
                    const u32 bottomRight = bottomLeft + 1;

                    // Same winding as TerrainChunk::BuildGeometry — the tess_eval
                    // stage declares `ccw`, so a flipped patch here would be
                    // back-face culled and the terrain would vanish.
                    indices.push_back(topLeft);
                    indices.push_back(bottomLeft);
                    indices.push_back(topRight);

                    indices.push_back(topRight);
                    indices.push_back(bottomLeft);
                    indices.push_back(bottomRight);
                }
            }

            s_PatchMesh = VertexArray::Create();
            auto vbo = VertexBuffer::Create(vertices.data(), static_cast<u32>(vertices.size() * sizeof(TerrainVertex)));
            vbo->SetLayout(TerrainVertex::GetLayout());
            s_PatchMesh->AddVertexBuffer(vbo);

            auto ibo = IndexBuffer::Create(indices.data(), static_cast<u32>(indices.size()));
            s_PatchMesh->SetIndexBuffer(ibo);
            s_PatchIndexCount = static_cast<u32>(indices.size());
        }
    } // namespace

    TerrainGPUQuadtree::TerrainGPUQuadtree() = default;
    TerrainGPUQuadtree::~TerrainGPUQuadtree() = default;

    u32 TerrainGPUQuadtree::TotalNodeCount(u32 maxDepth)
    {
        // sum(4^i, i = 0..maxDepth) == (4^(maxDepth+1) - 1) / 3
        u32 total = 0;
        for (u32 level = 0; level <= maxDepth; ++level)
        {
            total += 1u << (2u * level);
        }
        return total;
    }

    u32 TerrainGPUQuadtree::LevelOffset(u32 level)
    {
        return ((1u << (2u * level)) - 1u) / 3u;
    }

    u32 TerrainGPUQuadtree::PackNode(u32 level, u32 nx, u32 ny)
    {
        return (level << 28u) | ((ny & 0x3FFFu) << 14u) | (nx & 0x3FFFu);
    }

    const Ref<VertexArray>& TerrainGPUQuadtree::GetSharedPatchMesh()
    {
        if (!s_PatchMesh)
        {
            BuildSharedPatchMesh();
        }
        return s_PatchMesh;
    }

    u32 TerrainGPUQuadtree::GetSharedPatchIndexCount()
    {
        if (!s_PatchMesh)
        {
            BuildSharedPatchMesh();
        }
        return s_PatchIndexCount;
    }

    void TerrainGPUQuadtree::ReleaseSharedPatchMesh()
    {
        s_PatchMesh = nullptr;
        s_PatchIndexCount = 0;
    }

    RHI::ResourceHandle TerrainGPUQuadtree::GetDrawArgsHandle() const
    {
        return m_DrawArgsBuffer ? m_DrawArgsBuffer->GetRHIHandle() : RHI::ResourceHandle{};
    }

    RHI::ResourceHandle TerrainGPUQuadtree::GetVisibleNodesHandle() const
    {
        return m_VisibleNodes ? m_VisibleNodes->GetRHIHandle() : RHI::ResourceHandle{};
    }

    RHI::ResourceHandle TerrainGPUQuadtree::GetNodeBoundsHandle() const
    {
        return m_NodeBoundsBuffer ? m_NodeBoundsBuffer->GetRHIHandle() : RHI::ResourceHandle{};
    }

    void TerrainGPUQuadtree::EnsureShaders()
    {
        if (m_ShadersLoaded || m_ShaderLoadFailed)
        {
            return;
        }

        m_SelectShader = ComputeShader::Create("assets/shaders/compute/TerrainNodeSelect.comp");
        m_ArgsShader = ComputeShader::Create("assets/shaders/compute/TerrainCullArgs.comp");
        m_LODMapShader = ComputeShader::Create("assets/shaders/compute/TerrainLODMap.comp");
        m_SeamShader = ComputeShader::Create("assets/shaders/compute/TerrainSeamMap.comp");

        const bool ok = m_SelectShader && m_SelectShader->IsValid() &&
                        m_ArgsShader && m_ArgsShader->IsValid() &&
                        m_LODMapShader && m_LODMapShader->IsValid() &&
                        m_SeamShader && m_SeamShader->IsValid();
        if (!ok)
        {
            // Non-fatal: the caller keeps the CPU quadtree path, which still
            // produces a correct (if slower) frame.
            OLO_CORE_ERROR("TerrainGPUQuadtree: compute shader load failed — falling back to the CPU LOD path");
            m_ShaderLoadFailed = true;
            return;
        }
        m_ShadersLoaded = true;
    }

    bool TerrainGPUQuadtree::EnsureBuffers(u32 maxDepth)
    {
        const u32 totalNodes = TotalNodeCount(maxDepth);
        const u32 lodMapResolution = 1u << maxDepth;
        // The worst case for one level is 4^maxDepth nodes, but a camera never
        // splits the whole tree; cap and let the overflow flag report it.
        const u32 nodeListCapacity = std::min(1u << (2u * maxDepth), kMaxNodeListEntries);

        const bool shapeUnchanged = m_MaxDepth == maxDepth && m_NodeBoundsBuffer &&
                                    m_TotalNodes == totalNodes && m_NodeListCapacity == nodeListCapacity;
        if (shapeUnchanged)
        {
            return true;
        }

        using SBL = ShaderBindingLayout;
        m_NodeBoundsBuffer = StorageBuffer::Create(totalNodes * static_cast<u32>(sizeof(glm::vec2)),
                                                   SBL::SSBO_TERRAIN_NODE_BOUNDS, StorageBufferUsage::DynamicDraw);
        m_NodeListA = StorageBuffer::Create(nodeListCapacity * static_cast<u32>(sizeof(u32)),
                                            SBL::SSBO_TERRAIN_NODE_LIST_IN, StorageBufferUsage::DynamicCopy);
        m_NodeListB = StorageBuffer::Create(nodeListCapacity * static_cast<u32>(sizeof(u32)),
                                            SBL::SSBO_TERRAIN_NODE_LIST_OUT, StorageBufferUsage::DynamicCopy);
        m_CullStateBuffer = StorageBuffer::Create(static_cast<u32>(sizeof(TerrainGpuCullState)),
                                                  SBL::SSBO_TERRAIN_CULL_STATE, StorageBufferUsage::DynamicCopy);
        m_VisibleNodes = StorageBuffer::Create(kMaxVisibleNodes * static_cast<u32>(sizeof(glm::uvec2)),
                                               SBL::SSBO_TERRAIN_VISIBLE_NODES, StorageBufferUsage::DynamicCopy);
        m_SplitMap = StorageBuffer::Create(totalNodes * static_cast<u32>(sizeof(u32)),
                                           SBL::SSBO_TERRAIN_SPLIT_MAP, StorageBufferUsage::DynamicCopy);
        m_LODMap = StorageBuffer::Create(lodMapResolution * lodMapResolution * static_cast<u32>(sizeof(u32)),
                                         SBL::SSBO_TERRAIN_LOD_MAP, StorageBufferUsage::DynamicCopy);
        m_DrawArgsBuffer = StorageBuffer::Create(static_cast<u32>(sizeof(TerrainDrawArgsPOD)),
                                                 SBL::SSBO_TERRAIN_DRAW_ARGS, StorageBufferUsage::DynamicCopy);

        if (!m_NodeBoundsBuffer || !m_NodeListA || !m_NodeListB || !m_CullStateBuffer ||
            !m_VisibleNodes || !m_SplitMap || !m_LODMap || !m_DrawArgsBuffer)
        {
            OLO_CORE_ERROR("TerrainGPUQuadtree: GPU buffer allocation failed for depth {}", maxDepth);
            m_NodeBoundsBuffer = nullptr;
            return false;
        }

        // Seed the draw args with the patch index count so a draw issued before
        // the first Dispatch() reads instanceCount 0 rather than uninitialised
        // memory (it would draw garbage patches for exactly one frame).
        TerrainDrawArgsPOD seed{};
        seed.Count = GetSharedPatchIndexCount();
        m_DrawArgsBuffer->SetData(&seed, static_cast<u32>(sizeof(seed)), 0);

        m_MaxDepth = maxDepth;
        m_TotalNodes = totalNodes;
        m_NodeListCapacity = nodeListCapacity;
        m_LODMapResolution = lodMapResolution;
        return true;
    }

    void TerrainGPUQuadtree::Build(const std::vector<glm::vec2>& nodeMinMaxY, u32 maxDepth,
                                   f32 worldSizeX, f32 worldSizeZ)
    {
        OLO_PROFILE_FUNCTION();

        m_HasDispatched = false;
        m_OverflowWarned = false;

        if (maxDepth == 0 || maxDepth > kMaxDepth)
        {
            OLO_CORE_WARN("TerrainGPUQuadtree::Build: depth {} outside [1, {}] — GPU LOD disabled", maxDepth, kMaxDepth);
            m_MaxDepth = 0;
            m_NodeBoundsBuffer = nullptr;
            return;
        }

        const u32 expected = TotalNodeCount(maxDepth);
        if (nodeMinMaxY.size() != expected)
        {
            OLO_CORE_ERROR("TerrainGPUQuadtree::Build: node pyramid has {} entries, expected {} for depth {}",
                           nodeMinMaxY.size(), expected, maxDepth);
            m_MaxDepth = 0;
            m_NodeBoundsBuffer = nullptr;
            return;
        }

        if (!EnsureBuffers(maxDepth))
        {
            m_MaxDepth = 0;
            return;
        }

        m_WorldSizeX = worldSizeX;
        m_WorldSizeZ = worldSizeZ;
        m_NodeBoundsBuffer->SetData(nodeMinMaxY.data(),
                                    static_cast<u32>(nodeMinMaxY.size() * sizeof(glm::vec2)), 0);
    }

    void TerrainGPUQuadtree::UploadCullParams(const CullInputs& inputs)
    {
        UBOStructures::TerrainCullUBO params{};

        // Order must match Frustum::Planes — the shader indexes 0..5 blindly.
        using enum Frustum::Planes;
        constexpr std::array<Frustum::Planes, 6> kOrder = { Near, Far, Left, Right, Top, Bottom };
        for (sizet i = 0; i < kOrder.size(); ++i)
        {
            const Plane& plane = inputs.ViewFrustum.GetPlane(kOrder[i]);
            params.FrustumPlanes[i] = glm::vec4(plane.Normal, plane.Distance);
        }

        // Same expression as TerrainQuadtree::CalculateScreenSpaceError — kept
        // on the CPU so the two paths cannot disagree by a rounding step.
        const f32 projScale = inputs.ViewProjection[1][1] * inputs.ViewportHeight * 0.5f;
        params.CameraAndProjScale = glm::vec4(inputs.CameraPos, projScale);
        params.SizeAndTarget = glm::vec4(m_WorldSizeX, m_WorldSizeZ, inputs.TargetTriangleSize, 0.0f);
        params.LevelParams = glm::uvec4(m_MaxDepth, kMaxVisibleNodes, kPatchGridResolution, kMaxSeamDelta);
        params.BufferParams = glm::uvec4(m_NodeListCapacity, m_LODMapResolution, m_TotalNodes,
                                         GetSharedPatchIndexCount());

        if (!m_CullParamsUBO)
        {
            m_CullParamsUBO = UniformBuffer::Create(UBOStructures::TerrainCullUBO::GetSize(),
                                                    ShaderBindingLayout::UBO_TERRAIN_CULL);
        }
        // Upload then bind, in that order: on the Vulkan route every SetData
        // mints a fresh arena address, so a Bind() hoisted above the upload
        // would publish the previous frame's allocation (ADR 0011 §4).
        m_CullParamsUBO->SetData(&params, UBOStructures::TerrainCullUBO::GetSize());
        m_CullParamsUBO->Bind();
    }

    bool TerrainGPUQuadtree::Dispatch(const CullInputs& inputs)
    {
        OLO_PROFILE_FUNCTION();

        if (!IsBuilt())
        {
            return false;
        }

        EnsureShaders();
        if (!m_ShadersLoaded)
        {
            return false;
        }

        // The patch mesh has to exist before the args kernel writes its index
        // count into the draw command.
        GetSharedPatchIndexCount();

        UploadCullParams(inputs);

        // Reset the persistent state. Level 0 has exactly one node (the root),
        // so the first dispatch is direct — every later level's group count is
        // written by TerrainCullArgs.comp and read back through
        // DispatchComputeIndirect without a CPU round trip.
        TerrainGpuCullState state{};
        state.PendingCount = 1;
        state.SelectDispatchX = 1;
        state.SelectDispatchY = 1;
        state.SelectDispatchZ = 1;
        state.SeamDispatchY = 1;
        state.SeamDispatchZ = 1;
        m_CullStateBuffer->SetData(&state, static_cast<u32>(sizeof(state)), 0);

        const u32 rootNode = PackNode(0, 0, 0);
        m_NodeListA->SetData(&rootNode, static_cast<u32>(sizeof(rootNode)), 0);

        // A stale split flag would make the LOD map descend past a node that did
        // not split this frame, so the whole map must start clean.
        m_SplitMap->ClearData();

        using SBL = ShaderBindingLayout;
        RenderCommand::BindStorageBuffer(SBL::SSBO_TERRAIN_NODE_BOUNDS, m_NodeBoundsBuffer->GetRHIHandle());
        RenderCommand::BindStorageBuffer(SBL::SSBO_TERRAIN_CULL_STATE, m_CullStateBuffer->GetRHIHandle());
        RenderCommand::BindStorageBuffer(SBL::SSBO_TERRAIN_VISIBLE_NODES, m_VisibleNodes->GetRHIHandle());
        RenderCommand::BindStorageBuffer(SBL::SSBO_TERRAIN_SPLIT_MAP, m_SplitMap->GetRHIHandle());
        RenderCommand::BindStorageBuffer(SBL::SSBO_TERRAIN_LOD_MAP, m_LODMap->GetRHIHandle());
        RenderCommand::BindStorageBuffer(SBL::SSBO_TERRAIN_DRAW_ARGS, m_DrawArgsBuffer->GetRHIHandle());

        StorageBuffer* listIn = m_NodeListA.get();
        StorageBuffer* listOut = m_NodeListB.get();

        for (u32 level = 0; level <= m_MaxDepth; ++level)
        {
            RenderCommand::BindStorageBuffer(SBL::SSBO_TERRAIN_NODE_LIST_IN, listIn->GetRHIHandle());
            RenderCommand::BindStorageBuffer(SBL::SSBO_TERRAIN_NODE_LIST_OUT, listOut->GetRHIHandle());

            m_SelectShader->Bind();
            if (level == 0)
            {
                RenderCommand::DispatchCompute(1, 1, 1);
            }
            else
            {
                RenderCommand::DispatchComputeIndirect(m_CullStateBuffer->GetRHIHandle(), kSelectDispatchOffset);
            }
            // The args kernel reads the counters this dispatch atomically bumped.
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);

            m_ArgsShader->Bind();
            RenderCommand::DispatchCompute(1, 1, 1);
            // Command: the next iteration's DispatchComputeIndirect (and, on the
            // last pass, glDrawElementsIndirect) sources its arguments from the
            // buffer this kernel just wrote.
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::Command);

            std::swap(listIn, listOut);
        }

        // Split map -> per-texel selected level.
        m_LODMapShader->Bind();
        const u32 lodGroups = (m_LODMapResolution + kLODMapWorkgroupSize - 1) / kLODMapWorkgroupSize;
        RenderCommand::DispatchCompute(lodGroups, lodGroups, 1);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);

        // Per-visible-node seam deltas, dispatched from the GPU's own count.
        m_SeamShader->Bind();
        RenderCommand::DispatchComputeIndirect(m_CullStateBuffer->GetRHIHandle(), kSeamDispatchOffset);
        // ShaderStorage for the vertex stage's read of the node list, Command
        // for glDrawElementsIndirect's read of the draw arguments.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::Command);

        m_HasDispatched = true;
        PollOverflow();
        return true;
    }

    void TerrainGPUQuadtree::PollOverflow()
    {
        if (m_OverflowWarned || !m_CullStateBuffer)
        {
            return;
        }
        if (++m_FramesSinceOverflowPoll < kOverflowPollInterval)
        {
            return;
        }
        m_FramesSinceOverflowPoll = 0;

        u32 flags = 0;
        m_CullStateBuffer->GetData(&flags, static_cast<u32>(sizeof(flags)),
                                   static_cast<u32>(offsetof(TerrainGpuCullState, OverflowFlags)));
        if (flags == 0)
        {
            return;
        }

        // Warn once — a persistent overflow would otherwise spam every poll, and
        // the fix (raise the caps or lower the depth) is a one-time decision.
        m_OverflowWarned = true;
        if ((flags & kOverflowNodes) != 0)
        {
            OLO_CORE_WARN("TerrainGPUQuadtree: per-level node list overflowed {} entries — some terrain nodes were dropped. "
                          "Lower the quadtree depth or raise kMaxNodeListEntries.",
                          m_NodeListCapacity);
        }
        if ((flags & kOverflowVisible) != 0)
        {
            OLO_CORE_WARN("TerrainGPUQuadtree: visible node list overflowed {} entries — some terrain patches were dropped. "
                          "Raise the target triangle size or kMaxVisibleNodes.",
                          kMaxVisibleNodes);
        }
    }
} // namespace OloEngine
