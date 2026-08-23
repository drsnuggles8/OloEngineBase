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

    // Mesh-shader raster path limits (issue #813). One mesh workgroup renders one
    // cluster, and VirtualMeshletGBuffer.glsl declares these as its
    // `max_vertices` / `max_primitives` layout constants — they must stay in
    // sync. Chosen to match the default cook config (VirtualMeshBuildConfig:
    // 128/128) and comfortably inside VK_EXT_mesh_shader's guaranteed 256/256
    // minima. A mesh cooked with a larger config is routed to the classic MDI
    // path per instance (MeshEntry::MeshletCompatible) rather than split.
    inline constexpr u32 kMeshletMaxVertices = 128;
    inline constexpr u32 kMeshletMaxTriangles = 128;
    // Launch ceiling for one instance's mesh-tasks grid: the task stage emits
    // one mesh workgroup per visible cluster in a single grid dimension, and
    // VK_EXT_mesh_shader guarantees only 65535 workgroups per dimension. An
    // instance whose cluster count could exceed that is routed to the MDI
    // path (which has no such ceiling) rather than clamped — a clamp would
    // silently drop clusters. The dev RTX 4090 reports 4M+, so this only ever
    // bites on min-spec drivers, which is exactly why it must be the SPEC
    // minimum and not a queried value baked on a generous device.
    inline constexpr u32 kMeshletMaxClustersPerInstance = 65535;

    // The per-instance route decision, as ONE named predicate rather than an
    // expression inlined in the draw loop (§13c: one predicate, one owner).
    //
    // The two gates are deliberately of different KINDS, which is why the
    // cluster ceiling is not folded into IsMeshletCompatible below:
    //   * meshletCompatible is a PER-PART property, stamped once at
    //     registration from the cooked cluster geometry;
    //   * frameClusterCount is a PER-FRAME property — how many clusters this
    //     instance's LOD selection actually produced this frame, which is what
    //     the task stage turns into workgroups.
    // A part with more than 65535 clusters in total may still select far fewer
    // in any given frame, so demoting it statically would throw away the mesh
    // path for exactly the large meshes it exists to serve. Bounding the live
    // count is both necessary and sufficient: the launch never exceeds the
    // dimension guarantee, and no cluster is ever silently dropped.
    [[nodiscard]] constexpr bool ShouldUseMeshRaster(bool meshRasterAvailable, bool meshletCompatible,
                                                     u32 frameClusterCount) noexcept
    {
        return meshRasterAvailable && meshletCompatible &&
               frameClusterCount <= kMeshletMaxClustersPerInstance;
    }

    // Per-draw info UBO (binding 49 = UBO_VIRTUAL_DRAW), mirroring the ONE GLSL
    // spelling in OloEditor/assets/shaders/include/VirtualDrawInfo.glsl. Every
    // virtual-geometry pipeline — G-Buffer, meshlet, visbuffer resolve, shadow
    // depth — uploads this whole struct; a pass with no use for a field writes 0.
    // Until #813 the G-Buffer stage declared offsets 8/12 as pads while the
    // resolve/shadow stages declared them as the viewport size, i.e. two structs
    // sharing one binding. Giving every field its own offset here (and in the
    // shared include) is what stops that from being expressible.
    // offset  size  field
    //      0     4  InstanceIndex
    //      4     4  CommandBase
    //      8     8  ViewportWidth / ViewportHeight (resolve + shadow)
    //     16     8  ArgsSlot / MaxClusters         (mesh task stage, #813)
    //     24     8  Pad0 / Pad1
    struct VirtualDrawInfoGpu
    {
        u32 InstanceIndex = 0;
        u32 CommandBase = 0;
        u32 ViewportWidth = 0;
        u32 ViewportHeight = 0;
        u32 ArgsSlot = 0;
        u32 MaxClusters = 0;
        u32 Pad0 = 0;
        u32 Pad1 = 0;
    };
    static_assert(sizeof(VirtualDrawInfoGpu) == 32,
                  "std140 mirror in include/VirtualDrawInfo.glsl expects a 32-byte block");
    static_assert(sizeof(VirtualDrawInfoGpu) % 16 == 0,
                  "std140 block size must be a 16-byte multiple");

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
    //     52     4  Lod
    //     56     4  VertexCount  (cluster-owned vertex window; mesh path SetMeshOutputsEXT, #813)
    //     60     4  Pad2
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
        // Number of cluster-owned vertices (VertexBase..VertexBase+VertexCount).
        // Was padding until #813; only VirtualMeshletGBuffer.glsl reads it (for
        // SetMeshOutputsEXT), so shaders that still declare it as `_Pad1` are
        // layout-identical and stay untouched.
        u32 VertexCount = 0;
        u32 Pad2 = 0;

        static constexpr u32 kNoRefinedGroup = 0xFFFFFFFFu;
    };
    static_assert(sizeof(VirtualClusterGpuRecord) == 64, "std430 mirror in VirtualClusterCull.comp expects 64-byte cluster records");
    static_assert(sizeof(VirtualClusterGpuRecord) % 16 == 0, "std430 array stride must be a 16-byte multiple or the shader reads garbage");

    // One group record: the monotone LOD selection unit.
    // offset  size  field
    //      0    16  LODSphere (xyz = mesh-local center, w = radius)
    //     16     4  Error     (absolute object-space error; FLT_MAX marks terminal groups)
    //     20    12  Pad0..2
    struct VirtualGroupGpuRecord
    {
        glm::vec4 LODSphere{ 0.0f };
        f32 Error = 0.0f;
        f32 Pad0 = 0.0f;
        f32 Pad1 = 0.0f;
        f32 Pad2 = 0.0f;
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

        // The instance's material is MaterialFlag::TwoSided — its BACK faces are visible.
        //
        // This has to reach the GPU, not just the CPU draw loop's glDisable(GL_CULL_FACE):
        //  * VirtualClusterCull.comp's normal-cone backface rejection drops a cluster whose
        //    triangles ALL face away from the camera. For a two-sided sheet (a foliage card,
        //    a banner, cloth) those faces are visible, so the cone test must be skipped —
        //    the classic path has no cone cull at all, so this was pure virtual-path-only
        //    geometry loss when such a sheet was viewed from behind.
        //  * VirtualClusterRaster.comp hard-culls negative-signed-area (back-facing)
        //    triangles. With this flag it rasterizes them instead, flipping the barycentric
        //    sign — otherwise an OPAQUE two-sided material silently lost its back faces on
        //    every cluster small enough to be software-rasterized (under the 24px default
        //    threshold, i.e. most of them). Foliage escaped that only by being alpha-masked
        //    and therefore hardware-only.
        static constexpr u32 kFlagTwoSided = 1u << 2;
    };
    static_assert(sizeof(VirtualInstanceGpuRecord) == 224, "std430 mirror in VirtualClusterCull.comp expects 224-byte instance records");

    // Per-instance cull output header. The first field doubles as the
    // glMultiDrawElementsIndirectCount draw-count parameter (stride 16 keeps
    // each instance's count 4-byte aligned at offset instanceIndex * 16).
    //
    // The args array holds TWO regions of `instanceCount` entries (issue #682):
    // [0, n) is the two-phase cull's phase 1, [n, 2n) is phase 2. Phase 2 needs
    // its own draw count because the phase-1 MDI has already read its parameter
    // word by the time phase 2 runs. Only phase 1 accumulates TestedCount /
    // CutSelected — the DAG cut is decided once per frame.
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
        u32 Pad0 = 0;
        u32 Pad1 = 0;
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

    // Mesh-shader raster eligibility (#813): true when EVERY cluster fits one
    // mesh workgroup's declared output limits (kMeshletMaxVertices /
    // kMeshletMaxTriangles). Pure CPU; the registry stamps the result onto
    // MeshEntry::MeshletCompatible at registration.
    [[nodiscard]] bool IsMeshletCompatible(const VirtualMeshGpuData& data);
} // namespace OloEngine
