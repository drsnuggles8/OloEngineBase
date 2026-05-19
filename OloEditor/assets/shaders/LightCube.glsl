#type vertex
#version 450 core

layout(location = 0) in vec3 a_Position;

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    mat4 u_PrevViewProjection;
};

#include "include/InstanceBlock_Vertex.glsl"

layout(location = 0) out vec4 v_ClipPosCurr;
layout(location = 1) out vec4 v_ClipPosPrev;

void main()
{
    OLO_INSTANCE_FORWARD();
    vec4 worldPos = u_Model * vec4(a_Position, 1.0);
    vec4 clipCurr = u_ViewProjection * worldPos;
    vec4 prevWorldPos = u_PrevModel * vec4(a_Position, 1.0);
    vec4 clipPrev = u_PrevViewProjection * prevWorldPos;

    v_ClipPosCurr = clipCurr;
    v_ClipPosPrev = clipPrev;

    gl_Position = clipCurr;
}

#type fragment
#version 450 core

layout(location = 0) in vec4 v_ClipPosCurr;
layout(location = 1) in vec4 v_ClipPosPrev;

layout(location = 0) out vec4 FragColor;
// Scene FB RT3 velocity — light gizmos are rigid objects so per-object
// motion plus camera motion fully describes their screen-space trajectory.
layout(location = 3) out vec2 o_Velocity;

void main()
{
    FragColor = vec4(1.0);

    vec2 ndcCurr = v_ClipPosCurr.xy / v_ClipPosCurr.w;
    vec2 ndcPrev = v_ClipPosPrev.xy / v_ClipPosPrev.w;
    o_Velocity = (ndcCurr - ndcPrev) * 0.5;
}
