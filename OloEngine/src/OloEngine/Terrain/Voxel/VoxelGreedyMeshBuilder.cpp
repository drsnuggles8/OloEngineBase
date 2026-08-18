#include "OloEnginePCH.h"
#include "OloEngine/Terrain/Voxel/VoxelGreedyMeshBuilder.h"

#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/VertexBuffer.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

namespace OloEngine
{
    namespace
    {
        // Corner (u, v) of the shared unit quad, wound so the index buffer
        // {0,1,2, 2,3,0} traverses it counter-clockwise in the (U, V) plane.
        // Together with VoxelFaceBasis' cross(U, V) == normal ordering that
        // makes every one of the six directions front-face outward.
        constexpr f32 kUnitQuadCorners[8] = {
            0.0f,
            0.0f, // 0
            1.0f,
            0.0f, // 1
            1.0f,
            1.0f, // 2
            0.0f,
            1.0f, // 3
        };

        constexpr u32 kMinInstanceCapacity = 256;

        BufferLayout InstanceLayout()
        {
            // ShaderDataType has no unsigned integer member, and Int is the
            // right storage anyway: the geometry word uses 28 of 32 bits so it
            // is always non-negative, and the shader re-reads it as uint.
            return BufferLayout{
                { ShaderDataType::Int, "a_QuadGeometry" },
                { ShaderDataType::Int, "a_QuadMaterial" },
            };
        }
    } // namespace

    VoxelGreedyMeshBuilder::~VoxelGreedyMeshBuilder()
    {
        Clear();
    }

    void VoxelGreedyMeshBuilder::Clear()
    {
        // Retire WITHOUT waiting: dropping this builder must never stall the
        // game thread on a worker.
        //
        // It is safe by construction rather than by luck. The task body
        // (DispatchDirty) captures exactly one thing — a shared_ptr to its own
        // MeshJob — and never `this`, so a job in flight holds no reference to
        // the builder, its mesh map, or its GPU buffers. Abandoning the handle
        // lets the job finish into memory nobody reads and free itself; the
        // scheduler holds its own reference for as long as it needs one.
        // DispatchDirty already relies on exactly this when it abandons a
        // snapshot that an edit has invalidated.
        //
        // The earlier version waited here purely to avoid "wasted" background
        // CPU, which bought a few microseconds of worker time at the cost of a
        // synchronous stall on the thread that renders — the wrong trade, and
        // one paid on every mesher switch, terrain regenerate and scene copy.
        // Tests that need determinism use FlushPending, which still waits.
        m_Pending.clear();
        m_Meshes.clear();
    }

    u32 VoxelGreedyMeshBuilder::GetQuadCount() const
    {
        u32 total = 0;
        for (const auto& [coord, mesh] : m_Meshes)
        {
            total += mesh.QuadCount;
        }
        return total;
    }

    void VoxelGreedyMeshBuilder::Update(VoxelOverride& voxels)
    {
        OLO_PROFILE_FUNCTION();

        DispatchDirty(voxels);
        CollectCompleted(voxels.GetVoxelSize(), voxels);
    }

    void VoxelGreedyMeshBuilder::FlushPending(VoxelOverride& voxels)
    {
        OLO_PROFILE_FUNCTION();

        DispatchDirty(voxels);
        for (auto& pending : m_Pending)
        {
            pending.Task.Wait();
        }
        CollectCompleted(voxels.GetVoxelSize(), voxels);
    }

    void VoxelGreedyMeshBuilder::DispatchDirty(VoxelOverride& voxels)
    {
        OLO_PROFILE_FUNCTION();

        std::vector<VoxelCoord> dirtyCoords;
        voxels.GetDirtyChunks(dirtyCoords);
        if (dirtyCoords.empty())
        {
            return;
        }

        // A chunk's boundary faces are culled against its neighbours, so editing
        // one chunk invalidates the meshes of the six that touch it. Rebuilding
        // them here — rather than setting their Dirty flag — keeps this a single
        // ring around the edit instead of a cascade across the whole volume.
        std::vector<VoxelCoord> rebuild = dirtyCoords;
        constexpr i32 kNeighbourOffsets[6][3] = {
            { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }
        };
        for (const auto& coord : dirtyCoords)
        {
            for (const auto& offset : kNeighbourOffsets)
            {
                VoxelCoord neighbour{ coord.X + offset[0], coord.Y + offset[1], coord.Z + offset[2] };
                if (!voxels.HasChunk(neighbour))
                {
                    continue;
                }
                if (std::ranges::find(rebuild, neighbour) == rebuild.end())
                {
                    rebuild.push_back(neighbour);
                }
            }
        }

        for (const auto& coord : rebuild)
        {
            // Already meshing this chunk? Its snapshot predates the edit, so
            // abandon it rather than uploading stale quads.
            //
            // Abandon, not Wait: blocking the game thread here would undo the
            // whole point of dispatching. Dropping the handle is safe because
            // the job is owned by a shared_ptr the running task also holds — it
            // finishes into memory nobody reads and frees itself.
            auto inFlight = std::ranges::find_if(m_Pending, [&coord](const PendingMesh& p)
                                                 { return p.Coord == coord; });
            if (inFlight != m_Pending.end())
            {
                m_Pending.erase(inFlight);
            }

            auto job = std::make_shared<MeshJob>();
            VoxelGreedyMesher::Gather(voxels, coord, job->Neighbourhood);

            // Reserve for the worst realistic case (a checkerboard is pathological
            // and grows past this, which is fine — the vector just reallocates).
            job->Quads.reserve(static_cast<sizet>(VoxelChunk::CHUNK_SIZE) * VoxelChunk::CHUNK_SIZE);

            auto task = Tasks::Launch("VoxelGreedyMesh", [job]() mutable -> bool
                                      {
                    VoxelGreedyMesher mesher;
                    mesher.Mesh(job->Neighbourhood, job->Quads);
                    return true; }, Tasks::ETaskPriority::BackgroundNormal);

            m_Pending.push_back({ coord, std::move(task), std::move(job) });
        }

        for (const auto& coord : dirtyCoords)
        {
            voxels.MarkChunkClean(coord);
        }
    }

    void VoxelGreedyMeshBuilder::CollectCompleted(f32 voxelSize, const VoxelOverride& voxels)
    {
        OLO_PROFILE_FUNCTION();

        auto it = m_Pending.begin();
        while (it != m_Pending.end())
        {
            if (!it->Task.IsCompleted())
            {
                ++it;
                continue;
            }

            if (it->Job->Quads.empty())
            {
                m_Meshes.erase(it->Coord);
            }
            else
            {
                UploadMesh(it->Coord, it->Job->Quads, voxelSize, voxels);
            }

            it = m_Pending.erase(it);
        }
    }

    void VoxelGreedyMeshBuilder::EnsureSharedGeometry()
    {
        if (m_UnitQuadVBO && m_UnitQuadIBO)
        {
            return;
        }

        m_UnitQuadVBO = VertexBuffer::Create(kUnitQuadCorners, static_cast<u32>(sizeof(kUnitQuadCorners)));
        m_UnitQuadVBO->SetLayout({ { ShaderDataType::Float2, "a_Corner" } });

        // IndexBuffer::Create takes a mutable pointer, so this cannot be a
        // namespace-scope constant.
        u32 indices[VoxelQuadMesh::kIndexCount] = { 0, 1, 2, 2, 3, 0 };
        m_UnitQuadIBO = IndexBuffer::Create(indices, VoxelQuadMesh::kIndexCount);
    }

    void VoxelGreedyMeshBuilder::UploadMesh(const VoxelCoord& coord, const std::vector<PackedQuad>& quads,
                                            f32 voxelSize, const VoxelOverride& voxels)
    {
        OLO_PROFILE_FUNCTION();

        EnsureSharedGeometry();

        VoxelQuadMesh& mesh = m_Meshes[coord];
        mesh.ChunkCoord = coord;
        mesh.Bounds = voxels.GetChunkBounds(coord);

        const f32 chunkWorldSize = static_cast<f32>(VoxelChunk::CHUNK_SIZE) * voxelSize;
        const glm::vec3 chunkOrigin(
            static_cast<f32>(coord.X) * chunkWorldSize,
            static_cast<f32>(coord.Y) * chunkWorldSize,
            static_cast<f32>(coord.Z) * chunkWorldSize);
        mesh.ChunkTransform = glm::scale(glm::translate(glm::mat4(1.0f), chunkOrigin), glm::vec3(voxelSize));

        const auto quadCount = static_cast<u32>(quads.size());
        const auto dataSize = static_cast<u32>(quads.size() * sizeof(PackedQuad));

        // Rebuild the VAO whenever the instance buffer is (re)created: adding a
        // second instance buffer to a live VAO would double-bind the attributes.
        if (!mesh.VAO || !mesh.InstanceVBO || mesh.InstanceCapacity < quadCount)
        {
            mesh.InstanceCapacity = std::max(quadCount, kMinInstanceCapacity);

            mesh.VAO = VertexArray::Create();
            mesh.VAO->AddVertexBuffer(m_UnitQuadVBO);
            mesh.VAO->SetIndexBuffer(m_UnitQuadIBO);

            mesh.InstanceVBO = VertexBuffer::Create(mesh.InstanceCapacity * static_cast<u32>(sizeof(PackedQuad)));
            mesh.InstanceVBO->SetLayout(InstanceLayout());
            mesh.VAO->AddInstanceBuffer(mesh.InstanceVBO);
        }

        mesh.InstanceVBO->SetData({ quads.data(), dataSize });
        mesh.QuadCount = quadCount;
    }
} // namespace OloEngine
