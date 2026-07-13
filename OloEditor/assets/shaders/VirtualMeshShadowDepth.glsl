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

// Mirrors OloEngine::VirtualInstanceGpuRecord (224 B std430)
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

layout(std140, binding = 49) uniform VirtualDrawInfo {
    uint u_VirtualInstanceIndex;
    uint u_VirtualCommandBase;
    uint u_VirtualViewportWidth;
    uint u_VirtualViewportHeight;
};

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
