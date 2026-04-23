// =============================================================================
// InfiniteGrid_GBuffer.glsl - Deferred G-Buffer variant of InfiniteGrid.glsl.
//
// Editor-only infinite ground grid (XZ plane) rendered as **unlit** into the
// G-Buffer. The grid computes its own `gl_FragDepth` from the ray-plane
// intersection, so depth-testing only writes fragments where nothing is in
// front of the grid. Alpha-fade is baked into the emissive colour
// (premultiplied) so distant grid lines fade to black - acceptable for a
// reference overlay.
// Selected by `Renderer3D::DrawInfiniteGrid` when the deferred path is active.
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

layout(location = 0) out vec3 v_NearPoint;
layout(location = 1) out vec3 v_FarPoint;
layout(location = 2) out mat4 v_View;
layout(location = 6) out mat4 v_Projection;

vec3 UnprojectPoint(float x, float y, float z, mat4 viewInverse, mat4 projInverse) {
    vec4 unprojectedPoint = viewInverse * projInverse * vec4(x, y, z, 1.0);
    return unprojectedPoint.xyz / unprojectedPoint.w;
}

void main() {
    mat4 viewInverse = inverse(u_View);
    mat4 projInverse = inverse(u_Projection);

    v_NearPoint = UnprojectPoint(a_Position.x, a_Position.y, -1.0, viewInverse, projInverse);
    v_FarPoint  = UnprojectPoint(a_Position.x, a_Position.y,  1.0, viewInverse, projInverse);

    v_View = u_View;
    v_Projection = u_Projection;

    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) in vec3 v_NearPoint;
layout(location = 1) in vec3 v_FarPoint;
layout(location = 2) in mat4 v_View;
layout(location = 6) in mat4 v_Projection;

layout(location = 0) out vec4 o_GBufferAlbedo;
layout(location = 1) out vec4 o_GBufferNormal;
layout(location = 2) out vec4 o_GBufferEmissive;
layout(location = 3) out vec2 o_GBufferVelocity;

const float c_GridScale = 1.0;

vec4 Grid(vec3 fragPos3D, float scale, bool drawAxis) {
    vec2 coord = fragPos3D.xz * scale;
    vec2 derivative = fwidth(coord);
    vec2 grid = abs(fract(coord - 0.5) - 0.5) / derivative;
    float line = min(grid.x, grid.y);
    float minimumz = min(derivative.y, 1.0);
    float minimumx = min(derivative.x, 1.0);

    vec4 color = vec4(0.3, 0.3, 0.3, 1.0 - min(line, 1.0));

    if (drawAxis && fragPos3D.z > -0.1 * minimumz && fragPos3D.z < 0.1 * minimumz) {
        color.rgb = vec3(1.0, 0.3, 0.3);
        color.a = 1.0;
    }
    if (drawAxis && fragPos3D.x > -0.1 * minimumx && fragPos3D.x < 0.1 * minimumx) {
        color.rgb = vec3(0.3, 0.3, 1.0);
        color.a = 1.0;
    }

    return color;
}

float ComputeDepth(vec3 pos) {
    vec4 clipSpacePos = v_Projection * v_View * vec4(pos, 1.0);
    return (clipSpacePos.z / clipSpacePos.w) * 0.5 + 0.5;
}

float ComputeLinearDepth(vec3 pos) {
    float near = 0.01;
    float far  = 1000.0;
    vec4 clipSpacePos = v_Projection * v_View * vec4(pos, 1.0);
    float clipSpaceDepth = clipSpacePos.z / clipSpacePos.w;
    float linearDepth = (2.0 * near * far) / (far + near - clipSpaceDepth * (far - near));
    return linearDepth / far;
}

void main() {
    float t = -v_NearPoint.y / (v_FarPoint.y - v_NearPoint.y);
    if (t <= 0.0)
        discard;

    vec3 fragPos3D = v_NearPoint + t * (v_FarPoint - v_NearPoint);

    vec4 gridColor = Grid(fragPos3D, c_GridScale, true);
    gridColor += Grid(fragPos3D, c_GridScale * 0.1, true) * 0.5;

    float linearDepth = ComputeLinearDepth(fragPos3D);
    float fading = max(0.0, 1.0 - linearDepth * 2.0);
    gridColor.a *= fading;

    if (gridColor.a < 0.01)
        discard;

    // Premultiply alpha into the emissive output so fading grid lines
    // attenuate correctly. RT2.a = 1.0 marks the fragment unlit so
    // `ComputeDeferredLit` returns the grid colour unshaded.
    vec3 emissive = gridColor.rgb * gridColor.a;

    o_GBufferAlbedo   = vec4(0.0);
    o_GBufferNormal   = vec4(0.0);
    o_GBufferEmissive = vec4(emissive, 1.0);
    o_GBufferVelocity = vec2(0.0);

    gl_FragDepth = ComputeDepth(fragPos3D);
}
