#include "OloEnginePCH.h"
#include "OloEngine/Groom/GroomMeshSurface.h"

#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Renderer/MeshSource.h"

#include <cstddef>

namespace OloEngine
{
    u64 HashGroomSkeletonIdentity(const Skeleton* skeleton)
    {
        return skeleton != nullptr ? HashGroomSkeletonNames(skeleton->m_BoneNames) : 0u;
    }

    GroomSurfaceView MakeGroomSurfaceView(const MeshSource& surface, const Skeleton* skeleton)
    {
        GroomSurfaceView view;
        const auto& vertices = surface.GetVertices();
        const auto& indices = surface.GetIndices();
        if (vertices.IsEmpty() || indices.IsEmpty())
        {
            return view;
        }

        static_assert(offsetof(Vertex, Position) == 0, "GroomSurfaceView addresses positions at the start of a Vertex");
        view.PositionData = reinterpret_cast<const std::byte*>(vertices.GetData());
        view.PositionStride = static_cast<u32>(sizeof(Vertex));
        view.VertexCount = static_cast<u32>(vertices.Num());
        view.Indices = indices.GetData();
        view.IndexCount = static_cast<u32>(indices.Num());
        view.BoneCount = skeleton != nullptr ? static_cast<u32>(skeleton->m_FinalBoneMatrices.size()) : 0u;
        view.SkeletonNameHash = HashGroomSkeletonIdentity(skeleton);
        return view;
    }
} // namespace OloEngine
