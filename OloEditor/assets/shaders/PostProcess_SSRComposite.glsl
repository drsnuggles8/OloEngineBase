#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5): on the Vulkan backend vertex data is PULLED —
// binding 57 is the engine-wide vertex-pull binding; the root struct carries
// this buffer's device address, so the SAME 20-byte {vec3 position, vec2 uv}
// stream the attribute path consumes is read by index instead. OLO_VULKAN is
// defined only on the Vulkan shaderc route; the GL branch below is untouched.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    int base = gl_VertexIndex * 5;
    vec3 position = vec3(b_Vertices.v[base + 0], b_Vertices.v[base + 1], b_Vertices.v[base + 2]);
    v_TexCoord = vec2(b_Vertices.v[base + 3], b_Vertices.v[base + 4]);
    gl_Position = vec4(position, 1.0);
}
#else
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 1.0);
}
#endif

#type fragment
#version 460 core

// SSR composite (issue #902) — draw C of SSRRenderPass.
//
// Adds the temporally-resolved reflection DELTA to the upstream lit colour.
// The delta is (reflection - base) * blend, so `base + delta` reproduces the
// old `mix(base, reflection, blend)` exactly — the same replace/mix resolve,
// just with the stochastic half of it accumulated first. This runs AFTER the
// resolve on purpose: compositing first and resolving the composite is the
// failure #902 exists to avoid.
//
// Unlike SSGI, SSR's intensity is NOT applied here. It rides inside `blend`
// upstream because the blend is clamped to [0,1] after the multiply, and
// hoisting the multiply out would change what an intensity above 1 does.

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

#include "include/BindlessHeap.glsl"

#ifdef OLO_BINDLESS
#define u_SceneColor OLO_HEAP_TEX_2D(0)
#define u_ResolvedSignal OLO_HEAP_TEX_2D(1)
#define u_Guide OLO_HEAP_TEX_2D(2)
#else
layout(binding = 0) uniform sampler2D u_SceneColor; // upstream lit HDR colour
layout(binding = 1) uniform sampler2D u_ResolvedSignal;   // resolved reflection delta (rgb)
// The SSR guide plane. Only its ALPHA is read here, and only by the tier debug
// view: SSR's own arbitration confidence (issue #1057). See PostProcess_SSR.glsl.
layout(binding = 2) uniform sampler2D u_Guide;
#endif

// The SAME std140 block PostProcess_SSR.glsl declares (SSRUBOData).
layout(std140, binding = 38) uniform SSRParams
{
    mat4 u_Projection;
    mat4 u_InvProjection;
    mat4 u_View;
    vec4 u_RayParams;
    vec4 u_ShadeParams;
    vec4 u_ScreenParams;
    vec4 u_Flags;        // x = DebugView (0/1), y = FrameIndex, z = TierDebugView (0/1), w = RayTierActive (0/1)
    vec4 u_HZBParams;
    vec4 u_TemporalParams;
    vec4 u_DenoiseParams; // #708: x = PreBlurRadius (px), y = unused, z = PostBlurMaxRadius, w = unused
    vec4 u_DenoiseGuide;  // #708: x = PlaneTolerance, y = NormalPower, z = RoughnessKnee, w = MaxRoughness
};

// The reflection hierarchy's debug view (issue #1057, ADR 0019 7): which tier
// answered this pixel. It lives HERE and nowhere else because this draw is the
// only point in the frame where every tier's confidence is simultaneously
// reachable — SSR's from the guide plane's alpha, the ray tier's from the alpha
// of the colour it handed us, and the probe/IBL tier's as whatever residual the
// two above them left.
//
// Colours are flat and maximally distinct on purpose; this is an inspection
// tool, not a shaded image.
const vec3 kTierColorPlanar = vec3(1.0, 0.0, 1.0);   // magenta - reserved, see below
const vec3 kTierColorSSR = vec3(0.0, 1.0, 0.0);      // green
const vec3 kTierColorRayQuery = vec3(1.0, 0.25, 0.0); // orange
const vec3 kTierColorProbeIBL = vec3(0.0, 0.35, 1.0); // blue

vec3 OloReflectionTierDebugColor(float ssrConfidence, float rayConfidence)
{
    // PLANAR IS STRUCTURALLY ZERO ON THIS PATH and that is a finding, not an
    // omission: PlanarReflectionRenderPass disables itself on the deferred path
    // (a replayed opaque bucket would capture the G-Buffer, not lit colour) and
    // its result is consumed only by Water.glsl, while SSR is deferred-only. So
    // the two never coexist in a frame. The tier keeps its seat and its colour
    // so the view does not silently renumber if a deferred planar resolve ever
    // lands. ADR 0019 6 is the long version.
    const float planarConfidence = 0.0;

    // ADR 0019 1's weights, top tier first:
    //     w_t = c_t * PRODUCT over the tiers ABOVE t of (1 - c_u)
    // with the bottom tier taking the entire remaining residual, which is what
    // makes the four weights sum to exactly one.
    float residual = 1.0;
    float wPlanar = residual * clamp(planarConfidence, 0.0, 1.0);
    residual -= wPlanar;
    float wSSR = residual * clamp(ssrConfidence, 0.0, 1.0);
    residual -= wSSR;
    float wRay = residual * clamp(rayConfidence, 0.0, 1.0);
    residual -= wRay;
    float wProbeIBL = residual;

    // The dominant tier, ties going to the higher one (it claimed first).
    vec3 color = kTierColorProbeIBL;
    float best = wProbeIBL;
    if (wRay > best)
    {
        best = wRay;
        color = kTierColorRayQuery;
    }
    if (wSSR > best)
    {
        best = wSSR;
        color = kTierColorSSR;
    }
    if (wPlanar > best)
    {
        color = kTierColorPlanar;
    }
    return color;
}

void main()
{
    vec4 sceneSample = texture(u_SceneColor, v_TexCoord);
    vec3 baseColor = sceneSample.rgb;
    vec3 reflectionDelta = texture(u_ResolvedSignal, v_TexCoord).rgb;

    if (u_Flags.z > 0.5) // the tier debug view
    {
        // c_ray rides in the ALPHA of the colour the ray-query tier handed us,
        // and ONLY while its own debug flag is set — every other pass in the
        // chain writes alpha 1.0, so reading it unconditionally would paint the
        // whole frame as "the ray tier answered". u_Flags.w is that guard.
        float rayConfidence = (u_Flags.w > 0.5) ? clamp(sceneSample.a, 0.0, 1.0) : 0.0;
        float ssrConfidence = clamp(texture(u_Guide, v_TexCoord).a, 0.0, 1.0);
        o_Color = vec4(OloReflectionTierDebugColor(ssrConfidence, rayConfidence), 1.0);
        return;
    }

    if (u_Flags.x > 0.5) // debug: the resolved reflection delta in isolation
    {
        // Clamped like the composite below, and for the same reason: the delta
        // is deliberately SIGNED (a reflection darker than the surface it
        // replaces is legitimate), but this branch writes straight into
        // SSRColor, which bloom and the tonemapper then consume. The pre-#902
        // debug output was non-negative by construction; keep that property
        // rather than making a debug toggle able to push negative HDR radiance
        // down the chain.
        o_Color = vec4(max(reflectionDelta, vec3(0.0)), 1.0);
        return;
    }

    // Radiance cannot go negative even though the delta can: a strong negative
    // delta on a nearly-black surface would otherwise push the composite below
    // zero and make the tonemapper's job undefined.
    o_Color = vec4(max(baseColor + reflectionDelta, vec3(0.0)), 1.0);
}
