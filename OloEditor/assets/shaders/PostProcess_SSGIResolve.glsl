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

// SSGI temporal resolve (issue #902) — draw B of SSGIRenderPass.
//
// PostProcess_SSGI.glsl (draw A) now writes ONLY the stochastic term into
// SSGISignal: rgb = the one-bounce indirect diffuse estimate, a = the positive
// view-space depth of the shading point. This pass accumulates that signal
// against the previous frame's resolved signal and writes SSGIResolved, which
// the graph copies into the SSGIHistory sink and draw C composites.
//
// WHY THE SPLIT EXISTS AT ALL: SSGI used to composite straight into the scene
// colour, so its output target was not accumulable — temporally blending it
// would have smeared the base colour along with the noise. A dedicated signal
// attachment is the whole point of #902, and the composite MUST happen after
// this resolve, never before it.
//
// Every question a temporal resolve answers is answered by
// include/TemporalResolve.glsl, not re-derived here:
//   1. where was this pixel  -> G-Buffer velocity reprojection
//   2. same surface?         -> relative view-depth confidence, using the
//                               depth this pass itself stored in alpha last
//                               frame (there is no depth history buffer, and a
//                               fixed tolerance on device depth would be
//                               centimetres near the camera and kilometres far)
//   3. still plausible?      -> 3x3 neighbourhood variance CLIP (not a
//                               componentwise clamp — see the header)
//   4. how much do I keep?   -> motion-scaled feedback with a sub-pixel dead
//                               zone, so a stationary camera does not read as
//                               motion and bleed the current frame through

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

#include "include/BindlessHeap.glsl"

#ifdef OLO_BINDLESS
#define u_StochasticSignal OLO_HEAP_TEX_2D(0)
#define u_History OLO_HEAP_TEX_2D(1)
#define u_GVelocity OLO_HEAP_TEX_2D(46) // TEX_GBUFFER_VELOCITY
#else
layout(binding = 0) uniform sampler2D u_StochasticSignal;   // this frame's raw signal (rgb) + view depth (a)
layout(binding = 1) uniform sampler2D u_History;  // last frame's resolved signal (rgb) + its view depth (a)
layout(binding = 46) uniform sampler2D u_GVelocity; // G-Buffer RT3: current-minus-previous UV motion
#endif

#include "include/TemporalResolve.glsl"

// The SAME std140 block PostProcess_SSGI.glsl declares — one upload feeds all
// three draws of the pass. Mirrored on the CPU by SSGIUBOData.
layout(std140, binding = 40) uniform SSGIParams
{
    mat4 u_Projection;
    mat4 u_InvProjection;
    mat4 u_View;
    vec4 u_RayParams;
    vec4 u_ShadeParams;
    vec4 u_ScreenParams;    // x = width, y = height, z = 1/width, w = 1/height
    vec4 u_Flags;
    vec4 u_TemporalParams;  // x = Feedback, y = HasVelocity, z = HistoryUsable, w = ClipGamma
};

// Relative view-depth tolerance for the disocclusion test. 5% of the shading
// distance: loose enough that a grazing surface's own reprojection error does
// not read as a disocclusion, tight enough that a silhouette does.
const float SSGI_DEPTH_TOLERANCE = 0.05;

// Velocity dilation: reproject using the CLOSEST surface in the 3x3
// neighbourhood, so a foreground object drags its own history rather than the
// background's. Depth lives in the signal's alpha, so this needs no separate
// depth texture.
vec2 DilatedVelocityUV(vec2 uv, vec2 texel)
{
    vec2 bestUV = uv;
    float bestDepth = 1.0e30;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec2 sampleUV = uv + vec2(float(x), float(y)) * texel;
            float d = texture(u_StochasticSignal, sampleUV).a;
            if (d > 0.0 && d < bestDepth)
            {
                bestDepth = d;
                bestUV = sampleUV;
            }
        }
    }
    return bestUV;
}

void main()
{
    vec2 uv = v_TexCoord;
    vec4 current = texture(u_StochasticSignal, uv);

    // No usable history: first frame after a resize/enable, or the temporal
    // resolve is switched off for bisecting. Either way the correct answer is
    // the current frame — NOT a blend against whatever the buffer holds.
    if (u_TemporalParams.z < 0.5)
    {
        o_Color = current;
        return;
    }

    vec2 texel = u_ScreenParams.zw;
    vec2 velocity = vec2(0.0);
    if (u_TemporalParams.y > 0.5)
        velocity = texture(u_GVelocity, DilatedVelocityUV(uv, texel)).rg;

    vec2 prevUV = uv - velocity;
    if (!OloTemporalHistoryUVValid(prevUV))
    {
        o_Color = current;
        return;
    }

    vec4 history = texture(u_History, prevUV);

    float confidence = OloTemporalDepthConfidence(current.a, history.a, SSGI_DEPTH_TOLERANCE);

    OloTemporalStats stats;
    OLO_TEMPORAL_GATHER_3X3(u_StochasticSignal, uv, texel, stats);

    vec2 velocityPixels = velocity / max(texel, vec2(1.0e-8));
    float feedback = OloTemporalMotionFeedback(u_TemporalParams.x, velocityPixels, 1.0, 5.0, 0.5);

    vec3 resolved = OloTemporalResolve(current.rgb, history.rgb, stats, u_TemporalParams.w, feedback, confidence);

    // Indirect diffuse is a non-negative radiance; the variance clip can
    // undershoot on a hard edge, and a negative here would darken the composite
    // below the direct lighting. Alpha carries THIS frame's depth forward so
    // next frame's disocclusion test has something to compare against.
    o_Color = vec4(max(resolved, vec3(0.0)), current.a);
}
