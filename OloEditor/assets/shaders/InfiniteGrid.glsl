#type vertex
#version 450 core

// Infinite grid shader - renders a grid on the XZ plane that extends to infinity
// Uses standard depth (near=0, far=1)

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5, amendment (76)): vertex pull from the engine-wide
// binding 57. This draw site is Renderer3D::DrawInfiniteGrid's
// FullscreenQuadVAO (Renderer3DLifecycle.cpp) — a bare {vec3 a_Position} NDC
// quad at 12-byte stride, so the stride is 3 floats, NOT the 8-float engine
// Vertex. The GL attribute branch below is untouched.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;
#endif

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
    vec3 u_RenderOrigin; // camera-relative render origin (issue #429)
    float _padding1;
    // Reconstruction flavour of u_Projection (#691): the ndc z = ±1
    // unprojection and the *0.5+0.5 depth remap in this shader are
    // GL-convention math — the rasterizer flavour double-applies the remap
    // on Vulkan. Identical to u_Projection on GL.
    mat4 u_ProjectionForReconstruction;
};

layout(location = 0) out vec3 v_NearPoint;
layout(location = 1) out vec3 v_FarPoint;

// Unproject a point from clip space to world space
vec3 UnprojectPoint(float x, float y, float z, mat4 viewInverse, mat4 projInverse) {
    vec4 unprojectedPoint = viewInverse * projInverse * vec4(x, y, z, 1.0);
    return unprojectedPoint.xyz / unprojectedPoint.w;
}

void main() {
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 3;
    vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
#endif
    mat4 viewInverse = inverse(u_View);
    // Reconstruction flavour: the ±1 ndc z below is GL-convention.
    mat4 projInverse = inverse(u_ProjectionForReconstruction);

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
    vec3 u_RenderOrigin; // camera-relative render origin (issue #429)
    float _padding1;
    // Reconstruction flavour (#691) — see the vertex stage's note.
    mat4 u_ProjectionForReconstruction;
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
    // Reconstruction flavour composed with the view (#691): the
    // *0.5+0.5 below is the GL remap, so u_ViewProjection (rasterizer
    // flavour, z already remapped on Vulkan) would double-apply it and write
    // a gl_FragDepth in [0.5,1] — the grid would float above everything.
    vec4 clipSpacePos = u_ProjectionForReconstruction * u_View * vec4(pos, 1.0);
    // Convert from NDC [-1, 1] to depth buffer range [0, 1]
    return (clipSpacePos.z / clipSpacePos.w) * 0.5 + 0.5;
}

float ComputeLinearDepth(vec3 pos) {
    float near = 0.01;
    float far = 1000.0;
    // Same flavour note as ComputeDepth above.
    vec4 clipSpacePos = u_ProjectionForReconstruction * u_View * vec4(pos, 1.0);
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
        // Camera-relative (issue #429): fragPos3D is reconstructed via the
        // relative inverse-view, so it is render-relative. The grid lines/axes
        // are world-anchored (lines on integer world coords, coloured axes at
        // the true world origin), so feed Grid() the absolute world position.
        // Depth/velocity below keep the relative fragPos3D (relative VP).
        vec3 fragPos3DAbs = fragPos3D + u_RenderOrigin;
        // Render grid at two scales for better visibility
        vec4 gridColor = Grid(fragPos3DAbs, c_GridScale, true);
        gridColor += Grid(fragPos3DAbs, c_GridScale * 0.1, true) * 0.5;

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

        // Break the coplanar tie with scene ground geometry.
        //
        // The grid lies on Y=0, and scenes routinely put a ground plane on Y=0
        // too. The ground mesh gets the rasteriser's INTERPOLATED depth; this
        // shader writes gl_FragDepth from a position that was unprojected and
        // then re-projected. Those two paths compute the same surface and
        // disagree by ~1 ULP, so the depth test resolves it per-pixel on float
        // noise. NVIDIA happens to resolve it consistently; Mesa/radeonsi does
        // not, and the grid lines break into dashes toward the horizon
        // ("z-precision-dashed", RMSE ~22 against the NVIDIA goldens).
        //
        // Nudging the grid a hair toward the camera makes it win the tie on
        // every vendor instead of by luck. 1e-5 in [0,1] depth is ~170 quanta
        // of a 24-bit buffer -- comfortably decisive, and far too small to lift
        // the grid visibly off the ground.
        //
        // GOLDEN COUPLING: the grid dominates the near-black ground band of the
        // Atmosphere_Night* visual goldens (AtmosphereVisualEvidenceTest.cpp),
        // so changing this bias / the grid's depth path MUST rebake those
        // goldens in the SAME PR (--olo-golden-rebase) -- see issue #754,
        // where this bias landed (dfd100ef) without a rebake and left the night
        // ground band drifting alongside the star-hash change.
        const float kCoplanarBias = 1e-5;
        gl_FragDepth = clamp(depth - kCoplanarBias, 0.0, 1.0);
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
