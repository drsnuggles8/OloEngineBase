// =============================================================================
// VirtualMeshShadowDepth.glsl — depth-only shadow-caster path for virtualized
// geometry (issue #629). Same SSBO vertex pulling as VirtualMeshGBuffer.glsl,
// but the CameraMatrices UBO (binding 0) carries the SHADOW camera the shadow
// pass uploads per cascade, and the fragment stage is empty (depth-only;
// exempt from the fragment-output contract via the *Depth* name).
// =============================================================================

#type vertex
#version 460 core

// Mirrors OloEngine::VirtualGpuVertex (32 B std430)
struct VirtualGpuVertex {
    vec4 PositionU;
    vec4 NormalV;
};

// Mirrors OloEngine::VirtualInstanceGpuRecord (240 B std430)
struct VirtualInstance {
    mat4 Transform;
    mat4 PrevTransform;
    mat4 NormalMatrix;
    uint ClusterBase;
    uint ClusterCount;
    uint GroupBase;
    int  EntityID;
    float MaxScale;
    float ErrorThresholdPixels;
    uint CommandBase;
    uint Flags;
    // Baked lightmap atlas region (issue #867). Declared even where unused: the
    // std430 array stride IS the struct size, so omitting it makes every
    // instance after the first read the previous one's transform.
    vec4 LightmapScaleOffset;
};

layout(std430, binding = 39) readonly buffer VirtualVertices { VirtualGpuVertex vertices[]; };
layout(std430, binding = 35) readonly buffer VirtualInstances { VirtualInstance instances[]; };

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection; // shadow cascade light view-projection (render-origin-relative)
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

// Per-draw info (binding 49 = UBO_VIRTUAL_DRAW). This stage reads ONLY
// u_VirtualInstanceIndex; it never reads the viewport dimensions, and
// VirtualGeometryShadow.cpp uploads zero for every field it does not use.
//
// That "unused here" fact used to be expressed by naming the two slots
// `_vdPad0`/`_vdPad1` locally (#856), which kept the disagreement with
// VirtualVisibilityResolve.glsl — where the same bytes are real, populated
// viewport fields — from tripping the cross-shader layout test. #813 removed
// the disagreement itself instead: there is now one block, declared once in
// the include, so a stage that ignores a field simply ignores it and no local
// renaming is needed to keep the mirrors apart.
#include "include/VirtualDrawInfo.glsl"

void main()
{
    VirtualInstance inst = instances[u_VirtualInstanceIndex];
    VirtualGpuVertex vert = vertices[gl_VertexIndex];
    gl_Position = u_ViewProjection * (inst.Transform * vec4(vert.PositionU.xyz, 1.0));
}

#type fragment
#version 460 core

void main()
{
    // Depth-only
}
