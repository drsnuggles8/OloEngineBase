#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMesh.h"

#include <glm/vec4.hpp>

#include <cstddef>
#include <vector>

namespace OloEngine
{
    // GPU-side layouts for the virtualized-geometry cluster pipeline (issue #629).
    //
    // All structs are std430 mirrors consumed by VirtualClusterCull.comp and
    // VirtualMeshGBuffer.glsl — offsets and sizes are load-bearing; if any layout
    // changes the GLSL must change with it or the shaders read garbage.
    // Convention follows Instancing/InstanceData.h: explicit pad fields, 16-byte
    // multiple total size, static_assert-pinned.

    // One packed vertex, cluster-owned (clusters own their vertices so a later
    // streaming slice can page whole clusters without a shared indirection).
    // offset  size  field
    //      0    16  PositionU  (xyz = mesh-local position, w = TexCoord.x)
    //     16    16  NormalV    (xyz = mesh-local normal,   w = TexCoord.y)
    struct VirtualGpuVertex
    {
        glm::vec4 PositionU{ 0.0f };
        glm::vec4 NormalV{ 0.0f };
    };
    static_assert(sizeof(VirtualGpuVertex) == 32, "std430 mirror in VirtualMeshGBuffer.glsl expects 32-byte vertices");

    // One cluster record.
    // offset  size  field
    //      0    16  CullSphere  (xyz = mesh-local center, w = radius)
    //     16    16  Cone        (xyz = mesh-local axis, w = cutoff; cutoff >= 1 disables the test)
    //     32     4  VertexBase   (first slot in the pooled vertex array)
    //     36     4  IndexBase    (first index in the pooled cluster-local index buffer)
    //     40     4  IndexCount   (TriangleCount * 3)
    //     44     4  GroupIndex   (pooled group index — mesh base already applied)
    //     48     4  RefinedGroup (pooled; ~0u for LOD-0 clusters)
    //     52    12  _Pad0..2
    struct VirtualClusterGpuRecord
    {
        glm::vec4 CullSphere{ 0.0f };
        glm::vec4 Cone{ 0.0f, 0.0f, 0.0f, 1.0f };
        u32 VertexBase = 0;
        u32 IndexBase = 0;
        u32 IndexCount = 0;
        u32 GroupIndex = 0;
        u32 RefinedGroup = kNoRefinedGroup;
        u32 Lod = 0; // DAG level of the member group (0 = finest); for debug LOD viz (#629)
        u32 _Pad1 = 0;
        u32 _Pad2 = 0;

        static constexpr u32 kNoRefinedGroup = 0xFFFFFFFFu;
    };
    static_assert(sizeof(VirtualClusterGpuRecord) == 64, "std430 mirror in VirtualClusterCull.comp expects 64-byte cluster records");
    static_assert(sizeof(VirtualClusterGpuRecord) % 16 == 0, "std430 array stride must be a 16-byte multiple or the shader reads garbage");

    // One group record: the monotone LOD selection unit.
    // offset  size  field
    //      0    16  LODSphere (xyz = mesh-local center, w = radius)
    //     16     4  Error     (absolute object-space error; FLT_MAX marks terminal groups)
    //     20    12  _Pad0..2
    struct VirtualGroupGpuRecord
    {
        glm::vec4 LODSphere{ 0.0f };
        f32 Error = 0.0f;
        f32 _Pad0 = 0.0f;
        f32 _Pad1 = 0.0f;
        f32 _Pad2 = 0.0f;
    };
    static_assert(sizeof(VirtualGroupGpuRecord) == 32, "std430 mirror in VirtualClusterCull.comp expects 32-byte group records");

    // Per-frame instance record.
    // offset  size  field
    //      0    64  Transform      (render-origin-relative world transform)
    //     64    64  PrevTransform  (previous frame, same convention)
    //    128    64  NormalMatrix   (transpose(inverse(mat3(Transform))) in a mat4)
    //    192     4  ClusterBase / 196 ClusterCount / 200 GroupBase / 204 EntityID
    //    208     4  MaxScale / 212 ErrorThresholdPixels / 216 CommandBase / 220 Flags (bit0 = uniform scale)
    struct VirtualInstanceGpuRecord
    {
        glm::mat4 Transform{ 1.0f };
        glm::mat4 PrevTransform{ 1.0f };
        glm::mat4 NormalMatrix{ 1.0f };
        u32 ClusterBase = 0;
        u32 ClusterCount = 0;
        u32 GroupBase = 0;
        i32 EntityID = -1;
        f32 MaxScale = 1.0f;
        f32 ErrorThresholdPixels = 1.0f;
        u32 CommandBase = 0;
        u32 Flags = 0;

        static constexpr u32 kFlagUniformScale = 1u << 0;

        // The instance's material is alpha-MASKED (glTF MASK) or blended, so it must NOT be
        // software-rasterized.
        //
        // The compute rasterizer resolves depth with an atomic min on a packed uint64 and has
        // no texture access, so it cannot run the cutout test. A fully transparent leaf texel
        // therefore still WINS the depth race, and the material-resolve pass — which does run
        // the cutout — then discards it, leaving the geometry behind it unshaded. Sponza's
        // potted plants rendered as white speckle exactly this way.
        //
        // The hardware MDI path has a real fragment shader and discards before writing depth,
        // so masked clusters are routed there unconditionally (VirtualClusterCull.comp). This
        // is the same restriction UE5's Nanite has: masked materials are excluded from the
        // fast raster path.
        static constexpr u32 kFlagAlphaMasked = 1u << 1;
    };
    static_assert(sizeof(VirtualInstanceGpuRecord) == 224, "std430 mirror in VirtualClusterCull.comp expects 224-byte instance records");

    // Per-instance cull output header. The first field doubles as the
    // glMultiDrawElementsIndirectCount draw-count parameter (stride 16 keeps
    // each instance's count 4-byte aligned at offset instanceIndex * 16).
    struct VirtualDrawArgs
    {
        u32 DrawCount = 0;   // hardware-path visible-cluster count (parameter-buffer word)
        u32 TestedCount = 0; // clusters tested (stats)
        u32 CutSelected = 0; // clusters passing the DAG-cut rule before frustum/cone (stats)
        u32 SwCount = 0;     // clusters routed to the software rasterizer (stats)
    };
    static_assert(sizeof(VirtualDrawArgs) == 16, "GL_PARAMETER_BUFFER offsets assume 16-byte VirtualDrawArgs stride");

    // Per-draw record consumed by VirtualMeshGBuffer.glsl via gl_BaseInstance.
    struct VirtualVisibleCluster
    {
        u32 InstanceIndex = 0;
        u32 ClusterIndex = 0; // pooled cluster index
        u32 _Pad0 = 0;
        u32 _Pad1 = 0;
    };
    static_assert(sizeof(VirtualVisibleCluster) == 16, "std430 mirror in VirtualMeshGBuffer.glsl expects 16-byte visible records");

    // One streamable geometry page: a group's member-cluster geometry, which is
    // contiguous in the packed arrays by emission order. Terminal (root) pages
    // are pinned so a drawable fallback chain always exists under any budget.
    struct VirtualPageInfo
    {
        u32 GroupIndex = 0;   // mesh-local group this page belongs to
        u32 FirstCluster = 0; // mesh-local first member cluster
        u32 ClusterCount = 0;
        u32 VertexOffset = 0; // into VirtualMeshGpuData::Vertices
        u32 VertexCount = 0;
        u32 IndexOffset = 0; // into VirtualMeshGpuData::Indices
        u32 IndexCount = 0;
        bool Pinned = false;
    };

    // CPU-side packed geometry for one VirtualMesh, ready for pooled SSBO upload.
    // Vertices are cluster-owned (duplicated across clusters); Indices are
    // cluster-LOCAL (0..VertexCount-1) — the draw command's BaseVertex carries the
    // cluster's VertexBase so gl_VertexID lands on the right pooled vertex slot.
    // GroupIndex / RefinedGroup in Clusters are MESH-LOCAL here; the registry
    // applies pool bases at upload time. Cluster VertexBase/IndexBase are offsets
    // into THIS mesh's packed arrays; the streaming registry rebases them onto
    // live page slots whenever a page is loaded.
    struct VirtualMeshGpuData
    {
        std::vector<VirtualGpuVertex> Vertices;
        std::vector<u32> Indices;
        std::vector<VirtualClusterGpuRecord> Clusters;
        std::vector<VirtualGroupGpuRecord> Groups;
        std::vector<VirtualPageInfo> Pages; // one per group, ordered by group index

        [[nodiscard]] bool IsValid() const
        {
            return !Clusters.empty() && !Groups.empty();
        }
    };

    // Expands a built VirtualMesh into the GPU layout above. Pure CPU — headless
    // unit tests cover window tiling, index bounds, and bounds/cone fidelity.
    [[nodiscard]] VirtualMeshGpuData PackVirtualMeshForGpu(const VirtualMesh& mesh);
} // namespace OloEngine
