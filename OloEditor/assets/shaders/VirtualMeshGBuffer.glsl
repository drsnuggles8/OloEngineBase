// =============================================================================
// VirtualMeshGBuffer.glsl — hardware raster path of the virtualized-geometry
// cluster pipeline (Nanite-style cluster LOD DAG, issue #629).
//
// Draws the clusters selected by VirtualClusterCull.comp through one
// glMultiDrawElementsIndirectCount call per virtual-mesh instance. There are
// no vertex attributes: geometry is pulled from the cluster vertex SSBO via
// gl_VertexIndex (the pooled cluster-local index buffer + per-command
// BaseVertex land it on the right pooled slot), and per-draw data comes from
// the VirtualDrawInfo UBO (one update per MDI call) + gl_DrawID indexing the
// instance's command segment.
//
// Fragment stage mirrors PBR_GBuffer.glsl exactly (same material UBO, same
// texture slots, same MRT encodings) so virtual geometry inherits deferred
// PBR + shadows + GTAO + SSR unchanged.
// =============================================================================

#type vertex
#version 460 core

// gl_BaseInstance keys this draw's VisibleCluster record (the cull wrote it as
// each command's BaseInstance) — used only by the debug visualization.
#extension GL_ARB_shader_draw_parameters : require

// Structs, camera/motion-blur/draw-info blocks, the vertex/instance SSBOs and
// the per-vertex transform math all live in the include: it is shared VERBATIM
// with VirtualMeshletGBuffer.glsl's mesh stage (issue #813) so the two
// hardware raster pipelines cannot drift. Edit the include, never a copy here.
#include "include/VirtualGBufferVertexStage.glsl"

layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord;
layout(location = 3) out vec4 v_ClipPosCurr;
layout(location = 4) out vec4 v_ClipPosPrev;
layout(location = 5) flat out int v_EntityID;
layout(location = 6) flat out uint v_DbgSlot; // gl_BaseInstance -> VisibleCluster record (debug only)

invariant gl_Position;

void main()
{
    VirtualInstance inst = instances[u_VirtualInstanceIndex];
    VirtualGpuVertex vert = vertices[gl_VertexIndex];
    v_DbgSlot = uint(gl_BaseInstanceARB);

    VirtualVertexOutputs o = TransformVirtualVertex(inst, vert);
    v_WorldPos = o.WorldPos;
    v_Normal = o.Normal;
    v_TexCoord = o.TexCoord;
    v_EntityID = inst.EntityID;
    v_ClipPosCurr = o.ClipPosCurr;
    v_ClipPosPrev = o.ClipPosPrev;

    gl_Position = o.ClipPosCurr;
}

#type fragment
#version 460 core

// The WHOLE stage body lives in the include: it is shared verbatim with
// VirtualMeshletGBuffer.glsl (the VK_EXT_mesh_shader path, issue #813) so the
// two raster pipelines cannot drift. Edit the include, never a copy here.
#include "include/VirtualGBufferFragment.glsl"
