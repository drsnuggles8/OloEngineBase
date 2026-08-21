// =============================================================================
// VirtualMeshletGBuffer.glsl — VK_EXT_mesh_shader raster path of the
// virtualized-geometry cluster pipeline (issue #813; classic path: issue #629).
//
// VULKAN-ONLY. This shader is loaded only when the device reports
// VK_EXT_mesh_shader; it never travels the GL tier (OpenGLShader::PreProcess
// fails the whole load on task/mesh markers by design). It replaces the
// per-instance glMultiDrawElementsIndirectCount replay of
// VirtualMeshGBuffer.glsl with one vkCmdDrawMeshTasksEXT(1, 1, 1) per instance
// per phase:
//   * task stage  — reads this instance+phase's VirtualDrawArgs.DrawCount (the
//     value the MDI path consumed as the indirect-count parameter), clamps it
//     to the instance's ClusterCount (the MDI arm's maxDrawCount bound), and
//     launches that many mesh workgroups. No payload.
//   * mesh stage  — workgroup i loads visible[u_VirtualCommandBase + i] (the
//     same compacted VisibleCluster records the cull wrote for the MDI path,
//     addressed the same way BaseInstance would have), then emits the cluster's
//     triangles directly from the pooled vertex/index SSBOs — no index-buffer
//     bind, no command buffer, no gl_BaseInstanceARB. The per-vertex transform
//     math is include/VirtualGBufferVertexStage.glsl, shared VERBATIM with the
//     MDI vertex stage so the two pipelines cannot drift.
//   * fragment    — include/VirtualGBufferFragment.glsl, shared VERBATIM with
//     VirtualMeshGBuffer.glsl for the same reason.
//
// Cluster fit: local_size 32 (the RTX 4090's preferred invocation count) and
// the OLO_MESHLET_MAX_* limits from VirtualGeometryGpuStructs.glsl, which
// mirror the C++ kMeshletMax* constants (test-pinned); the C++ routes any
// bigger cluster — and any instance whose cluster count could exceed the
// 65535-workgroup grid-dimension floor — to the classic MDI path.
// =============================================================================

#type task
#version 460 core
#extension GL_EXT_mesh_shader : require

#include "include/VirtualGeometryGpuStructs.glsl"

layout(local_size_x = 1) in;

layout(std430, binding = 37) readonly buffer VirtualDrawArgsBuf { VirtualDrawArgs args[]; };

// Per-draw info (binding 49 = UBO_VIRTUAL_DRAW) — same block and same uploaded
// contents as the MDI path. Included directly rather than via
// VirtualGBufferVertexStage.glsl because the task stage needs none of the
// vertex machinery.
#include "include/VirtualDrawInfo.glsl"

void main()
{
    // One task workgroup per instance per phase: fan out to one mesh workgroup
    // per visible cluster. DrawCount can be 0 — EmitMeshTasksEXT(0, 1, 1)
    // launches nothing, exactly like an MDI count of 0. The clamp mirrors the
    // MDI arm's maxDrawCount: a stale or corrupt GPU-written count must not
    // launch workgroups past the instance's segment of visible[].
    uint count = min(args[u_VirtualArgsSlot].DrawCount, u_VirtualMaxClusters);
    EmitMeshTasksEXT(count, 1u, 1u);
}

#type mesh
#version 460 core
#extension GL_EXT_mesh_shader : require

// Structs, camera/motion-blur/draw-info blocks, the vertex/instance SSBOs and
// TransformVirtualVertex all come from the shared include (see header comment).
#include "include/VirtualGBufferVertexStage.glsl"

layout(local_size_x = 32) in;
layout(triangles, max_vertices = OLO_MESHLET_MAX_VERTICES, max_primitives = OLO_MESHLET_MAX_PRIMITIVES) out;

layout(std430, binding = 33) readonly buffer VirtualClusters { VirtualCluster clusters[]; };
layout(std430, binding = 38) readonly buffer VirtualVisible { VisibleCluster visible[]; };
layout(std430, binding = 42) readonly buffer VirtualIndices { uint localIndices[]; }; // cluster-LOCAL, base = cluster.IndexBase

// The MDI twin declares `invariant gl_Position`; mirror it here so the two
// pipelines' position math stays reproducible where instances of both routes
// abut (a mixed meshlet-compatible / oversized-cook scene rasterizes adjoining
// surfaces through different pipelines).
out gl_MeshPerVertexEXT {
    invariant vec4 gl_Position;
} gl_MeshVerticesEXT[];

// Per-vertex varyings — the VirtualGBufferFragment.glsl contract, locations and
// types identical to VirtualMeshGBuffer.glsl's vertex stage. Flat varyings are
// written by every vertex of the workgroup (all with the same value), so flat
// provoking-vertex interpolation yields it regardless of provoking convention.
layout(location = 0) out vec3 v_WorldPos[];
layout(location = 1) out vec3 v_Normal[];
layout(location = 2) out vec2 v_TexCoord[];
layout(location = 3) out vec4 v_ClipPosCurr[];
layout(location = 4) out vec4 v_ClipPosPrev[];
layout(location = 5) flat out int v_EntityID[];
layout(location = 6) flat out uint v_DbgSlot[]; // visible-slot index (debug only)

void main()
{
    // Workgroup -> compacted VisibleCluster slot. u_VirtualCommandBase is this
    // instance's segment base in the phase's global visible[] numbering — the
    // same slot the MDI path put in each command's BaseInstance.
    uint slot = u_VirtualCommandBase + gl_WorkGroupID.x;
    VisibleCluster record = visible[slot];
    VirtualInstance inst = instances[record.InstanceIndex];
    VirtualCluster cluster = clusters[record.ClusterIndex];

    uint vertexCount = cluster.VertexCount;
    uint triCount = cluster.IndexCount / 3u;
    SetMeshOutputsEXT(vertexCount, triCount);

    // Vertex loop — the SHARED per-vertex transform (see header comment).
    // Stride via gl_WorkGroupSize.x so the loop tracks local_size_x.
    for (uint v = gl_LocalInvocationIndex; v < vertexCount; v += gl_WorkGroupSize.x)
    {
        VirtualGpuVertex vert = vertices[cluster.VertexBase + v];
        VirtualVertexOutputs o = TransformVirtualVertex(inst, vert);

        gl_MeshVerticesEXT[v].gl_Position = o.ClipPosCurr;
        v_WorldPos[v] = o.WorldPos;
        v_Normal[v] = o.Normal;
        v_TexCoord[v] = o.TexCoord;
        v_ClipPosCurr[v] = o.ClipPosCurr;
        v_ClipPosPrev[v] = o.ClipPosPrev;
        v_EntityID[v] = inst.EntityID;
        v_DbgSlot[v] = slot;
    }

    // Primitive loop — pooled cluster-local indices, three per triangle.
    for (uint p = gl_LocalInvocationIndex; p < triCount; p += gl_WorkGroupSize.x)
    {
        uint base = cluster.IndexBase + p * 3u;
        gl_PrimitiveTriangleIndicesEXT[p] = uvec3(localIndices[base + 0u],
                                                  localIndices[base + 1u],
                                                  localIndices[base + 2u]);
    }
}

#type fragment
#version 460 core

// The WHOLE stage body lives in the include: it is shared verbatim with
// VirtualMeshGBuffer.glsl (the classic MDI path) so the two raster pipelines
// cannot drift. Edit the include, never a copy here.
#include "include/VirtualGBufferFragment.glsl"
