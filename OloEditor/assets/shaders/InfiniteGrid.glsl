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
};

layout(location = 0) out vec3 v_NearPoint;
layout(location = 1) out vec3 v_FarPoint;
layout(location = 2) out mat4 v_View;
layout(location = 6) out mat4 v_Projection;

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

    v_View = u_View;
    v_Projection = u_Projection;

    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 450 core

layout(location = 0) in vec3 v_NearPoint;
layout(location = 1) in vec3 v_FarPoint;
layout(location = 2) in mat4 v_View;
layout(location = 6) in mat4 v_Projection;

layout(location = 0) out vec4 FragColor;
layout(location = 1) out int EntityID;

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
    vec4 clipSpacePos = v_Projection * v_View * vec4(pos, 1.0);
    // Convert from NDC [-1, 1] to depth buffer range [0, 1]
    return (clipSpacePos.z / clipSpacePos.w) * 0.5 + 0.5;
}

float ComputeLinearDepth(vec3 pos) {
    float near = 0.01;
    float far = 1000.0;
    vec4 clipSpacePos = v_Projection * v_View * vec4(pos, 1.0);
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
    } else {
        discard;
    }
}
