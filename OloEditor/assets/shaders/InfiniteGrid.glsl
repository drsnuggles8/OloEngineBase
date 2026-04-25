#type vertex
#version 450 core

// Infinite grid shader - renders a grid on the XZ plane that extends to infinity
// Uses standard depth (near=0, far=1)

layout(location = 0) in vec3 a_Position;

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    // Drives the velocity output on scene FB RT3 so TAA reprojects the
    // world-static grid correctly under camera motion. The reconstructed
    // fragPos3D is static in world space, so the NDC delta captures exactly
    // the motion induced by the camera.
    mat4 u_PrevViewProjection;
};

layout(location = 0) out vec3 v_NearPoint;
layout(location = 1) out vec3 v_FarPoint;

// Unproject a point from clip space to world space
vec3 UnprojectPoint(float x, float y, float z, mat4 viewInverse, mat4 projInverse) {
    vec4 unprojectedPoint = viewInverse * projInverse * vec4(x, y, z, 1.0);
    return unprojectedPoint.xyz / unprojectedPoint.w;
}

void main() {
    mat4 viewInverse = inverse(u_View);
    mat4 projInverse = inverse(u_Projection);

    // Unproject to get near and far points on the grid plane
    // Standard depth: near plane is at z=-1 in NDC, far plane is at z=1
    v_NearPoint = UnprojectPoint(a_Position.x, a_Position.y, -1.0, viewInverse, projInverse);
    v_FarPoint = UnprojectPoint(a_Position.x, a_Position.y, 1.0, viewInverse, projInverse);

    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 450 core

layout(location = 0) in vec3 v_NearPoint;
layout(location = 1) in vec3 v_FarPoint;

layout(location = 0) out vec4 FragColor;
layout(location = 1) out int EntityID;
layout(location = 2) out vec2 o_ViewNormal;
// Scene FB RT3 velocity — grid is world-static, so NDC delta = camera motion.
layout(location = 3) out vec2 o_Velocity;

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    mat4 u_PrevViewProjection;
};

// Grid settings (hardcoded for now - could be passed via uniform block if needed)
const float c_GridScale = 1.0;

// Grid line rendering
vec4 Grid(vec3 fragPos3D, float scale, bool drawAxis) {
    vec2 coord = fragPos3D.xz * scale;
    vec2 derivative = fwidth(coord);
    vec2 grid = abs(fract(coord - 0.5) - 0.5) / derivative;
    float line = min(grid.x, grid.y);
    float minimumz = min(derivative.y, 1.0);
    float minimumx = min(derivative.x, 1.0);

    vec4 color = vec4(0.3, 0.3, 0.3, 1.0 - min(line, 1.0));

    // X axis (red) - when Z is near 0
    if (drawAxis && fragPos3D.z > -0.1 * minimumz && fragPos3D.z < 0.1 * minimumz) {
        color.rgb = vec3(1.0, 0.3, 0.3);
        color.a = 1.0;
    }
    // Z axis (blue) - when X is near 0
    if (drawAxis && fragPos3D.x > -0.1 * minimumx && fragPos3D.x < 0.1 * minimumx) {
        color.rgb = vec3(0.3, 0.3, 1.0);
        color.a = 1.0;
    }

    return color;
}

float ComputeDepth(vec3 pos) {
    vec4 clipSpacePos = u_ViewProjection * vec4(pos, 1.0);
    // Convert from NDC [-1, 1] to depth buffer range [0, 1]
    return (clipSpacePos.z / clipSpacePos.w) * 0.5 + 0.5;
}

float ComputeLinearDepth(vec3 pos) {
    float near = 0.01;
    float far = 1000.0;
    vec4 clipSpacePos = u_ViewProjection * vec4(pos, 1.0);
    float clipSpaceDepth = clipSpacePos.z / clipSpacePos.w;
    float linearDepth = (2.0 * near * far) / (far + near - clipSpaceDepth * (far - near));
    return linearDepth / far; // Normalize
}

void main() {
    // Calculate t for ray-plane intersection (Y = 0 plane)
    float t = -v_NearPoint.y / (v_FarPoint.y - v_NearPoint.y);

    // Calculate 3D position on the grid plane
    vec3 fragPos3D = v_NearPoint + t * (v_FarPoint - v_NearPoint);

    // Compute depth for depth testing
    float depth = ComputeDepth(fragPos3D);

    // Only render if the plane intersection is valid (t > 0) and in front of camera
    if (t > 0.0) {
        // Render grid at two scales for better visibility
        vec4 gridColor = Grid(fragPos3D, c_GridScale, true);
        gridColor += Grid(fragPos3D, c_GridScale * 0.1, true) * 0.5;

        // Distance-based fade
        float linearDepth = ComputeLinearDepth(fragPos3D);
        float fading = max(0.0, 1.0 - linearDepth * 2.0);

        // Apply fading
        gridColor.a *= fading;

        // Discard fully transparent fragments
        if (gridColor.a < 0.01) {
            discard;
        }

        FragColor = gridColor;
        gl_FragDepth = depth;
        EntityID = -1;  // Grid is not pickable
        o_ViewNormal = vec2(-2.0);

        // Scene FB RT3 velocity. The grid is anchored in world space, so
        // current/previous clip positions differ only by camera motion.
        vec4 clipCurr = u_ViewProjection     * vec4(fragPos3D, 1.0);
        vec4 clipPrev = u_PrevViewProjection * vec4(fragPos3D, 1.0);
        vec2 ndcCurr = clipCurr.xy / clipCurr.w;
        vec2 ndcPrev = clipPrev.xy / clipPrev.w;
        o_Velocity = (ndcCurr - ndcPrev) * 0.5;
    } else {
        discard;
    }
}
