// =============================================================================
// DepthPrepass.glsl - Depth-only prepass shader for opaque static meshes
//
// Bound by CommandDispatch in place of the full PBR forward / G-Buffer shader
// while the scene depth prepass is active, so covered fragments never run the
// expensive lighting shader — the prepass exists to ELIMINATE overdraw, not
// multiply the per-pixel cost by it.
//
// The position math and `invariant gl_Position` MUST stay bit-identical to
// the vertex stages of PBR_MultiLight.glsl / PBR_GBuffer.glsl: the color pass
// re-draws the same geometry with glDepthFunc(GL_LEQUAL) and depth writes
// off, so any rounding difference fails the depth test and punches holes.
// =============================================================================

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 Phase 7 (ADR 0011 §5): V1 engine-vertex pull. On the Vulkan route the
// vertex data is READ, not fetched -- binding 57 is the engine-wide vertex-pull
// binding and the root struct carries this buffer's device address. The stream
// is the engine `Vertex` (32 B: vec3 position @0, vec3 normal @12, vec2 uv @24),
// so the per-vertex stride is 8 floats. Pulled locals below main() carry the
// ATTRIBUTE NAMES, which keeps the body identical on both routes; the GL
// attribute branch is untouched.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;
#endif

// Camera UBO (binding 0)
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

// Instance transforms SSBO (binding 15)
// This shader's consuming stage never reads v_InstanceIndex — declare no
// varying (a written-but-unconsumed output is a per-pipeline Vulkan
// validation interface warning).
#define OLO_INSTANCE_NO_FORWARD 1
#include "include/InstanceBlock_Vertex.glsl"

// ROUTE PARITY, not a bindless conversion (issue #691 Phase 3, glsl-shaders §7a-bis).
// This shader declares no samplers, so it has nothing to convert and would
// never mention OLO_BINDLESS on its own. It must still follow the COLOUR pass
// onto the raw-GLSL route, because `invariant gl_Position` below is a promise
// between TWO PROGRAMS and `invariant` cannot keep it across two different
// compiler front-ends. Left behind on the SPIR-V route while PBR_MultiLight
// moved, this pass wrote depth the colour pass then failed LEQUAL against, in
// blotches, on curved surfaces only.
#define OLO_BINDLESS_ROUTE_PARITY 1

invariant gl_Position;

void main()
{
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 8;
    vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
#endif
    OLO_INSTANCE_FORWARD();
    // Same association as PBR_MultiLight / PBR_GBuffer: world position first,
    // then view-projection — required for invariant depth between the passes.
    vec3 worldPos = vec3(u_Model * vec4(a_Position, 1.0));
    gl_Position = u_ViewProjection * vec4(worldPos, 1.0);
}

#type fragment
#version 460 core

// Overdraw counter. During the normal depth prepass the colour mask is off, so
// this write is discarded and the shader stays depth-only. When the overdraw
// debug view (#519) replays this geometry with the colour mask on, depth test
// off and additive (GL_ONE, GL_ONE) blending, each covered fragment adds 1 to
// the accumulation target's red channel — the raw per-pixel overdraw count.
layout(location = 0) out vec4 o_OverdrawCount;

void main()
{
    // Depth is written by the rasterizer; colour writes are masked off in the
    // ordinary depth prepass (see above).
    o_OverdrawCount = vec4(1.0);
}
