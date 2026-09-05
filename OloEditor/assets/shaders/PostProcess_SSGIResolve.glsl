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
layout(location = 1) out vec4 o_MomentsFirst;
layout(location = 2) out vec4 o_MomentsSecond;
layout(location = 3) out vec4 o_HistoryDiagnostics;
layout(location = 4) out vec4 o_ReprojectionDiagnostics;

layout(location = 0) in vec2 v_TexCoord;

#include "include/BindlessHeap.glsl"

#ifdef OLO_BINDLESS
#define u_StochasticSignal OLO_HEAP_TEX_2D(0)
#define u_History OLO_HEAP_TEX_2D(1)
#define u_SurfaceHistory OLO_HEAP_TEX_2D(2)
#define u_FirstMomentsHistory OLO_HEAP_TEX_2D(3)
#define u_SecondMomentsHistory OLO_HEAP_TEX_2D(4)
#define u_Guide OLO_HEAP_TEX_2D(5)
#define u_GVelocity OLO_HEAP_TEX_2D(46) // TEX_GBUFFER_VELOCITY
#else
layout(binding = 0) uniform sampler2D u_StochasticSignal;   // this frame's PRE-BLURRED signal (rgb) + view depth (a)
layout(binding = 1) uniform sampler2D u_History;  // last frame's resolved signal (rgb) + its view depth (a)
layout(binding = 2) uniform sampler2D u_SurfaceHistory; // last frame's oct normal + roughness + AO
layout(binding = 3) uniform sampler2D u_FirstMomentsHistory;
layout(binding = 4) uniform sampler2D u_SecondMomentsHistory;
// The current frame's guide plane at the TRACE band (issue #708), not the
// full-resolution G-Buffer this used to read. With a half-resolution trace the
// two are different resolutions, and a surface test run against full-res
// silhouettes the signal cannot see rejects history along every edge — a
// permanent noisy fringe that no amount of history length ever fixes. The trace
// writes this attachment and the graph extracts SSGISurfaceHistory from it, so
// both sides of the test are the same quantity at the same resolution.
layout(binding = 5) uniform sampler2D u_Guide;
layout(binding = 46) uniform sampler2D u_GVelocity; // G-Buffer RT3: current-minus-previous UV motion
#endif

#include "include/TemporalResolve.glsl"
#include "include/SurfaceHistory.glsl"

// The SAME std140 block PostProcess_SSGI.glsl declares — one upload feeds all
// three draws of the pass. Mirrored on the CPU by SSGIUBOData.
layout(std140, binding = 40) uniform SSGIParams
{
    mat4 u_Projection;
    mat4 u_InvProjection;
    mat4 u_View;
    vec4 u_RayParams;
    vec4 u_ShadeParams;
    vec4 u_ScreenParams;    // x = FULL-res width, y = height, z = 1/width, w = 1/height
    vec4 u_Flags;
    vec4 u_TemporalParams;  // x = Feedback, y = HasVelocity, z = HistoryUsable, w = ClipGamma
    vec4 u_TraceParams;     // x = trace width, y = trace height, z = 1/width, w = 1/height
    vec4 u_DenoiseParams;
    vec4 u_DenoiseGuide;
};

// Relative view-depth tolerance for the disocclusion test. 5% of the shading
// distance: loose enough that a grazing surface's own reprojection error does
// not read as a disocclusion, tight enough that a silhouette does.
const float SSGI_DEPTH_TOLERANCE = 0.05;

vec3 DecodeSurfaceNormal(vec2 encoded)
{
    vec3 n = vec3(encoded, 1.0 - abs(encoded.x) - abs(encoded.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0,
                                        n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

OloSurfaceHistoryRecord MakeSSGISurface(float depth, vec4 packedSurface, vec2 motion)
{
    OloSurfaceHistoryRecord result;
    result.LinearDepth = depth;
    result.GeometricNormal = DecodeSurfaceNormal(packedSurface.xy);
    result.ShadingNormal = result.GeometricNormal;
    result.Roughness = packedSurface.z;
    result.MaterialClass = 0u;
    result.Motion = motion;
    result.Instance = uvec2(0xffffffffu, 0u);
    result.Primitive = uvec2(0xffffffffu, 0u);
    result.Material = uvec2(0xffffffffu, 0u);
    result.Flags = 0u;
    result.HitDistance = 0.0;
    result.PrimitiveLocalIndex = 0xffffffffu;
    return result;
}

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
    bool historyAvailable = u_TemporalParams.z >= 0.5;
    // The TRACE band's texel, not the screen's: with a half-resolution trace
    // every neighbourhood gather and every velocity-dilation step below is
    // walking this pass's own targets, and stepping them by a full-res texel
    // would sample the same texel nine times and quietly disable both.
    vec2 texel = u_TraceParams.zw;
    vec2 velocity = vec2(0.0);
    if (historyAvailable && u_TemporalParams.y > 0.5)
        velocity = texture(u_GVelocity, DilatedVelocityUV(uv, texel)).rg;

    vec2 prevUV = uv - velocity;
    bool historySampleAvailable = historyAvailable && OloTemporalHistoryUVValid(prevUV);
    vec4 history = historySampleAvailable ? texture(u_History, prevUV) : current;
    vec4 currentSurfacePacked = texture(u_Guide, uv);
    vec4 previousSurfacePacked = historySampleAvailable ? texture(u_SurfaceHistory, prevUV) : currentSurfacePacked;
    OloSurfaceHistoryRecord currentSurface = MakeSSGISurface(current.a, currentSurfacePacked, velocity);
    OloSurfaceHistoryRecord previousSurface = MakeSSGISurface(history.a, previousSurfacePacked, vec2(0.0));
    OloSurfaceHistorySettings validitySettings;
    validitySettings.TestMask = OLO_SURFACE_TEST_SHADING_NORMAL |
                                OLO_SURFACE_TEST_ROUGHNESS |
                                OLO_SURFACE_TEST_MOTION;
    validitySettings.RelativeDepthThreshold = SSGI_DEPTH_TOLERANCE;
    validitySettings.GeometricNormalCosineThreshold = 0.85;
    validitySettings.ShadingNormalCosineThreshold = 0.75;
    validitySettings.RoughnessThreshold = 0.15;
    validitySettings.MotionThresholdPixels = 64.0;
    validitySettings.RelativeHitDistanceThreshold = 0.1;
    validitySettings.PixelSize = texel;
    uint rejectionReasons = OloEvaluateSurfaceHistory(
        currentSurface, previousSurface, prevUV, historyAvailable, validitySettings);
    float confidence = rejectionReasons == OLO_SURFACE_REJECT_NONE ? 1.0 : 0.0;

    OloTemporalStats stats;
    OLO_TEMPORAL_GATHER_3X3(u_StochasticSignal, uv, texel, stats);

    vec2 velocityPixels = velocity / max(texel, vec2(1.0e-8));
    float feedback = OloTemporalMotionFeedback(u_TemporalParams.x, velocityPixels, 1.0, 5.0, 0.5);

    vec3 resolved = OloTemporalResolve(current.rgb, history.rgb, stats, u_TemporalParams.w, feedback, confidence);

    vec4 previousFirstPacked = historySampleAvailable ? texture(u_FirstMomentsHistory, prevUV) : vec4(0.0);
    vec4 previousSecondPacked = historySampleAvailable ? texture(u_SecondMomentsHistory, prevUV) : vec4(0.0);
    OloTemporalMoments previousMoments;
    previousMoments.First = vec4(previousFirstPacked.rgb, 0.0);
    previousMoments.Second = vec4(previousSecondPacked.rgb, 0.0);
    previousMoments.HistoryLength = previousFirstPacked.a;
    bool historyAccepted = rejectionReasons == OLO_SURFACE_REJECT_NONE;
    OloTemporalMoments moments = OloAccumulateTemporalMoments(
        vec4(current.rgb, 0.0), previousMoments, historyAccepted, 255.0);
    vec3 variance = OloTemporalVariance(moments).rgb;

    // Indirect diffuse is a non-negative radiance; the variance clip can
    // undershoot on a hard edge, and a negative here would darken the composite
    // below the direct lighting. Alpha carries THIS frame's depth forward so
    // next frame's disocclusion test has something to compare against.
    o_Color = vec4(max(resolved, vec3(0.0)), current.a);
    o_MomentsFirst = vec4(moments.First.rgb, moments.HistoryLength);
    o_MomentsSecond = vec4(moments.Second.rgb, dot(variance, vec3(0.2126, 0.7152, 0.0722)));
    o_HistoryDiagnostics = vec4(float(rejectionReasons), moments.HistoryLength,
                                o_MomentsSecond.a, historyAccepted ? 1.0 : 0.0);
    bool disoccluded = (currentSurface.Flags & OLO_SURFACE_FLAG_DISOCCLUDED) != 0u ||
        (rejectionReasons & (OLO_SURFACE_REJECT_OFF_SCREEN | OLO_SURFACE_REJECT_DEPTH |
                             OLO_SURFACE_REJECT_GEOMETRIC_NORMAL | OLO_SURFACE_REJECT_SHADING_NORMAL |
                             OLO_SURFACE_REJECT_INSTANCE | OLO_SURFACE_REJECT_PRIMITIVE |
                             OLO_SURFACE_REJECT_MATERIAL)) != 0u;
    o_ReprojectionDiagnostics = vec4(prevUV,
        (currentSurface.Flags & OLO_SURFACE_FLAG_REACTIVE) != 0u ? 1.0 : 0.0,
        disoccluded ? 1.0 : 0.0);
}
