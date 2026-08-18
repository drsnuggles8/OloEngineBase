#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/BoundingVolume.h"
// Complete types, not forward declarations: VoxelQuadMesh holds Ref<> members,
// and Ref<T>'s destructor needs T complete in every TU that destroys one.
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Task/Task.h"
#include "OloEngine/Terrain/Voxel/VoxelGreedyMesher.h"
#include "OloEngine/Terrain/Voxel/VoxelQuad.h"

#include <glm/glm.hpp>
#include <memory>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    // One chunk's worth of packed quads, ready to draw instanced.
    struct VoxelQuadMesh
    {
        // Every chunk shares the same 6-index unit quad; only the instance
        // stream differs. Draw sites pass this as the command's indexCount.
        static constexpr u32 kIndexCount = 6;

        VoxelCoord ChunkCoord;
        BoundingBox Bounds; // terrain-local AABB of the whole chunk

        Ref<VertexArray> VAO;
        Ref<VertexBuffer> InstanceVBO;
        u32 QuadCount = 0;
        u32 InstanceCapacity = 0;

        // Terrain-local placement of the chunk: translate(chunkOrigin) *
        // scale(voxelSize). The quads themselves are in chunk-local VOXEL units,
        // which is what lets the whole record fit in 32 bits — the world offset
        // rides the model matrix instead of the vertex data.
        glm::mat4 ChunkTransform{ 1.0f };

        // Two triangles per quad, always.
        [[nodiscard]] u32 GetTriangleCount() const
        {
            return QuadCount * 2;
        }
    };

    // Owns the greedy meshes of one VoxelOverride and keeps them up to date.
    //
    // Meshing is dispatched to the task system (acceptance criterion 3): the
    // game thread only snapshots the padded neighbourhood and, later, uploads
    // finished quad buffers. The bitwise merge itself runs on a worker. See
    // VoxelNeighbourhood for why the snapshot is the thread-safety seam.
    class VoxelGreedyMeshBuilder : public RefCounted
    {
      public:
        VoxelGreedyMeshBuilder() = default;
        // Waits out any in-flight mesh job. Dropping the builder while a worker
        // is still meshing would leave the scheduler holding a task whose owner
        // is gone — the jobs themselves are shared_ptr-owned, but the handles
        // are not, and this is the one place the lifetime can end abruptly
        // (switching the mesher back to marching cubes nulls the Ref).
        ~VoxelGreedyMeshBuilder() override;

        // Game thread. Dispatches meshing for chunks that changed and uploads
        // any jobs that finished since the last call. Never blocks on a worker.
        void Update(VoxelOverride& voxels);

        // Blocks until every in-flight mesh job has been collected and uploaded.
        // For tests and offline capture — not for the frame loop.
        void FlushPending(VoxelOverride& voxels);

        [[nodiscard]] const std::unordered_map<VoxelCoord, VoxelQuadMesh, VoxelCoordHash>& GetMeshes() const
        {
            return m_Meshes;
        }

        [[nodiscard]] bool HasPendingWork() const
        {
            return !m_Pending.empty();
        }

        [[nodiscard]] u32 GetQuadCount() const;

        [[nodiscard]] u32 GetTriangleCount() const
        {
            return GetQuadCount() * 2;
        }

        void Clear();

      private:
        // Owned by the worker for the duration of the job; the game thread only
        // touches it once the task reports completion.
        struct MeshJob
        {
            VoxelNeighbourhood Neighbourhood;
            std::vector<PackedQuad> Quads;
        };

        struct PendingMesh
        {
            VoxelCoord Coord;
            Tasks::TTask<bool> Task;
            std::shared_ptr<MeshJob> Job;
        };

        void DispatchDirty(VoxelOverride& voxels);
        void CollectCompleted(f32 voxelSize, const VoxelOverride& voxels);
        void UploadMesh(const VoxelCoord& coord, const std::vector<PackedQuad>& quads,
                        f32 voxelSize, const VoxelOverride& voxels);
        void EnsureSharedGeometry();

        std::unordered_map<VoxelCoord, VoxelQuadMesh, VoxelCoordHash> m_Meshes;
        std::vector<PendingMesh> m_Pending;

        // The unit quad every chunk instances. Shared by every VAO — only the
        // per-chunk instance stream differs.
        Ref<VertexBuffer> m_UnitQuadVBO;
        Ref<IndexBuffer> m_UnitQuadIBO;
    };
} // namespace OloEngine
