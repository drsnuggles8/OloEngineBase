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
layout(location = 7) out vec2 v_TexCoord2;              // baked lightmap uv2 (issue #867)
layout(location = 8) flat out vec4 v_LightmapScaleOffset; // its atlas region; all-zero = no lightmap

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
    v_TexCoord2 = FetchVirtualLightmapUV(inst, uint(gl_VertexIndex));
    v_LightmapScaleOffset = inst.LightmapScaleOffset;
    v_EntityID = inst.EntityID;
    v_ClipPosCurr = o.ClipPosCurr;
    v_ClipPosPrev = o.ClipPosPrev;

    gl_Position = o.ClipPosCurr;
}

#type fragment
#version 460 core

// THE VULKAN MATERIAL-HEAP ARM (ADR 0011 amendment (96), issue #805), and it is
// declared HERE rather than in the include below even though every declaration
// it affects lives there. VulkanShader asks for a `#define` of this token in the
// ENTRY SHADER'S OWN, PRE-INCLUDE text: a shared header cannot grant the arm,
// which is the narrowing PR #1120's review put in (a header that merely TESTS a
// token is not a shader that TAKES the route). So the stage body stays shared
// and the opt-in is stated once per entry point.
//
// BOTH ENTRY SHADERS MUST CARRY IT, and that is a correctness requirement rather
// than symmetry. VirtualGeometryPass picks between the MDI and mesh-shader
// pipelines PER INSTANCE inside one recording loop, calling
// CommandDispatch::UploadMaterialForDirectDraw after each rebind;
// Shader::ReadsMaterialHeapOffsets() is program state read at that moment. Two
// programs disagreeing about the arm would make the five material binds land for
// one route and be withheld for the other while the shared body samples the same
// way. Pinned by BindlessShaderPipeline.
// EntryShadersSharingAMaterialStageBodyAgreeOnTheHeapArm — spelled whole so it
// is greppable.
//
// THE DIRECTIVES MUST SIT HERE, before any other token: GLSL requires every
// `#extension` to precede all non-preprocessor tokens, and the include below
// cannot satisfy that. `#ifdef` is not a token, so the guard is legal and the GL
// tier — which compiles this same source WITHOUT the macro — never sees them.
#ifdef OLO_VULKAN
#extension GL_EXT_descriptor_heap : require
#extension GL_EXT_nonuniform_qualifier : require
#define OLO_MATERIAL_VULKAN_HEAP_READER 1
#endif

// The WHOLE stage body lives in the include: it is shared verbatim with
// VirtualMeshletGBuffer.glsl (the VK_EXT_mesh_shader path, issue #813) so the
// two raster pipelines cannot drift. Edit the include, never a copy here.
#include "include/VirtualGBufferFragment.glsl"
