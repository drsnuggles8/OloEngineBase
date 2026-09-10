#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMesh.h"

#include <vector>

namespace OloEngine
{
    // @brief The ray-tracing proxy of one virtual-mesh part (issue #1144).
    //
    // Virtual geometry is drawn from a cluster DAG that the GPU cuts anew every
    // frame. Nothing about that is expressible as a BLAS: an acceleration
    // structure is built from ONE index buffer, and the cut changes per view,
    // per frame. So virtualized entities never reached GPU Scene, therefore
    // never reached the TLAS, therefore were invisible to every ray-traced
    // effect in the engine — shadows, reflections and the path tracer all
    // traced a scene with the Nanite content missing from it.
    //
    // The proxy is UE's answer, and this is what it is: the DAG's COARSEST cut
    // — its root clusters — flattened into an ordinary indexed triangle mesh,
    // uploaded once at registration and registered in GPU Scene as ordinary
    // rigid geometry. Rays then hit approximately-right geometry, which for
    // shadows, ambient occlusion and rough reflections is what they needed.
    //
    // Three properties make the coarsest cut the right source, and they are
    // properties of the DAG rather than of this code:
    //
    //   * it is WATERTIGHT. The builder locks group-boundary vertices, so a cut
    //     partitions the surface exactly — the proxy has no cracks a shadow ray
    //     can leak through.
    //   * it is SMALL and bounded. Each DAG level halves the triangle count, so
    //     the root cut of a million-triangle mesh is a few thousand triangles.
    //     The BLAS is therefore cheap to build and cheap to trace, which is the
    //     whole reason not to trace the full-resolution mesh.
    //   * it is FIXED. It does not depend on the camera, so the BLAS is built
    //     once and never refits — the geometry classifies as Static and
    //     RayTracingScene's build-once policy applies unchanged.
    //
    // What it is NOT: an exact match for what the raster path drew this frame.
    // A close-up reflection of a virtual mesh reflects its silhouette, not its
    // micro-detail. That is the accepted trade in approach 1 of #1144; the
    // alternative (a per-frame BLAS rebuilt from the live cut) is a different
    // issue with a different cost.
    struct VirtualProxyMesh
    {
        // Compacted: only the vertices the cut's triangles actually reference,
        // in first-use order. A cut names a handful of the DAG's clusters, and
        // the DAG's vertex array holds every LOD level's vertices — carrying
        // the whole array would put a full-resolution vertex buffer on the GPU
        // to serve a few thousand triangles.
        std::vector<Vertex> Vertices;
        // Triangle list into Vertices. Always a multiple of 3.
        std::vector<u32> Indices;
        // The DAG's full-resolution triangle count, kept so a consumer can
        // report the reduction the proxy achieved rather than only its own
        // size — "1,412 of 871,414 triangles" is actionable where "1,412" is
        // not.
        u32 SourceTriangleCount = 0;

        [[nodiscard]] u32 TriangleCount() const
        {
            return static_cast<u32>(Indices.size() / 3);
        }

        [[nodiscard]] bool IsValid() const
        {
            return !Vertices.empty() && Indices.size() >= 3 && (Indices.size() % 3) == 0;
        }
    };

    // Flattens the coarsest cut of `dag` into a proxy mesh. Returns an invalid
    // proxy (IsValid() == false) for a DAG that is itself invalid or whose cut
    // produced no whole triangle — never a partially-built one, so a caller has
    // exactly one thing to test.
    //
    // Treats the DAG as UNTRUSTED. A cooked blob arrives from disk and the
    // deserializer's cross-referencing is the only thing between it and here;
    // this walk indexes three arrays through two levels of indirection, so
    // every offset is bounds-checked and an out-of-range cluster is dropped
    // rather than read.
    [[nodiscard]] VirtualProxyMesh BuildVirtualProxyMesh(const VirtualMesh& dag);
} // namespace OloEngine
