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

// SSGI GUIDED UPSCALE + COMPOSITE (issues #902, #708) — the last draw of
// SSGIRenderPass.
//
// Two jobs in one full-resolution draw:
//
//   1. UPSCALE (issue #708, stage 5). The whole denoiser chain before this runs
//      at the trace band, which is half resolution when the half-res trace is
//      on. This draw resolves the denoised half-res signal onto the full-res
//      surface, guided by the full-res depth and normal so the indirect light
//      lands on the geometry the viewer can actually see rather than being
//      stretched across every silhouette. Indirect diffuse is a smooth,
//      slowly-varying signal, which is exactly why half-res costs so little
//      here and why this final spatial step is effectively free denoising.
//
//      When the trace runs at full resolution the maths degenerates on its own:
//      the bilinear footprint collapses onto one texel with weight 1 and the
//      other three get weight 0, so there is no separate code path to keep
//      correct.
//
//   2. COMPOSITE (issue #902). Adds the resolved indirect diffuse to the
//      upstream lit colour. This runs AFTER the resolve on purpose:
//      compositing first and resolving the composite is the failure #902 exists
//      to avoid, because it would accumulate the base colour along with the
//      stochastic term.
//
// Intensity is applied HERE rather than inside the accumulated signal, so
// dragging the slider does not have to wait for the history to converge.

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

#include "include/BindlessHeap.glsl"

#ifdef OLO_BINDLESS
#define u_SceneColor OLO_HEAP_TEX_2D(0)
#define u_DenoisedSignal OLO_HEAP_TEX_2D(1)
#define u_Guide OLO_HEAP_TEX_2D(2)
#define u_DepthTexture OLO_HEAP_TEX_2D(19)  // TEX_POSTPROCESS_DEPTH
#define u_GBufferNormal OLO_HEAP_TEX_2D(44) // TEX_GBUFFER_NORMAL
#else
layout(binding = 0) uniform sampler2D u_SceneColor;     // upstream lit HDR colour (full res)
layout(binding = 1) uniform sampler2D u_DenoisedSignal; // trace band: rgb = indirect diffuse, a = view depth
layout(binding = 2) uniform sampler2D u_Guide;          // trace band: rg = oct world normal, b = roughness, a = AO
layout(binding = 19) uniform sampler2D u_DepthTexture;  // full-res scene depth (nonlinear, [0,1])
layout(binding = 44) uniform sampler2D u_GBufferNormal; // full-res RT1: rg = oct world normal, z = roughness, w = ao
#endif

#include "include/SpatialDenoise.glsl"

// The SAME std140 block PostProcess_SSGI.glsl declares (SSGIUBOData).
layout(std140, binding = 40) uniform SSGIParams
{
    mat4 u_Projection;
    mat4 u_InvProjection;
    mat4 u_View;
    vec4 u_RayParams;
    vec4 u_ShadeParams;  // x = Intensity, y = RayCount, z = EdgeFade, w = unused
    vec4 u_ScreenParams; // x = FULL-res width, y = height, z = 1/width, w = 1/height
    vec4 u_Flags;        // x = DebugView (0/1), y = FrameIndex, zw = pad
    vec4 u_TemporalParams;
    vec4 u_TraceParams;  // x = trace width, y = trace height, z = 1/width, w = 1/height
    vec4 u_DenoiseParams;
    vec4 u_DenoiseGuide; // x = PlaneTolerance, y = NormalPower, z = TargetHistoryLength, w = RayDistribution
};

const float SKY_DEPTH = 0.999999;

// Reconstruct view-space position from screen UV + nonlinear depth. Same form
// as PostProcess_SSGI.glsl's — this draw is the one place in the chain that has
// a real depth buffer rather than the signal's alpha, because it is the only
// one running at full resolution.
vec3 ViewPosFromDepth(vec2 uv, float depth)
{
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 view = u_InvProjection * ndc;
    return view.xyz / view.w;
}

void main()
{
    vec3 baseColor = texture(u_SceneColor, v_TexCoord).rgb;

    ivec2 fullSize = ivec2(max(u_ScreenParams.xy, vec2(1.0)));
    ivec2 traceSize = ivec2(max(u_TraceParams.xy, vec2(1.0)));
    ivec2 fullTexel = clamp(ivec2(gl_FragCoord.xy), ivec2(0), fullSize - 1);

    float depth = texelFetch(u_DepthTexture, fullTexel, 0).r;

    vec3 indirectDiffuse = vec3(0.0);
    if (depth < SKY_DEPTH) // the sky receives no GI, and has no normal to guide with
    {
        vec2 fullUV = (vec2(fullTexel) + 0.5) * u_ScreenParams.zw;
        vec3 positionVS = ViewPosFromDepth(fullUV, depth);
        float viewDepth = -positionVS.z;
        vec3 normalWS = OloDenoiseOctDecode(texelFetch(u_GBufferNormal, fullTexel, 0).xy);
        vec3 normalVS = normalize(mat3(u_View) * normalWS);

        // The trace-band coordinate this full-res pixel falls on, in texel
        // space with the half-texel offsets removed on both sides so the
        // footprint is centred correctly at any ratio.
        vec2 scale = u_TraceParams.xy / u_ScreenParams.xy;
        vec2 traceCoord = (vec2(fullTexel) + 0.5) * scale - 0.5;
        ivec2 base = ivec2(floor(traceCoord));
        vec2 frac = traceCoord - vec2(base);

        // A 2x2 joint bilateral upsample rather than the reference's 3x3 tent.
        // The tent is the right call for a denoiser whose signal is guaranteed
        // smooth; this chain has just spent a variance-guided post-blur
        // deciding where NOT to smooth, and a 3x3 tent here would undo that
        // decision at every pixel. The 2x2 footprint keeps the contact detail
        // the acceptance criteria are about.
        vec4 bilinear = vec4((1.0 - frac.x) * (1.0 - frac.y),
                             frac.x * (1.0 - frac.y),
                             (1.0 - frac.x) * frac.y,
                             frac.x * frac.y);
        const ivec2 offsets[4] = ivec2[4](ivec2(0, 0), ivec2(1, 0), ivec2(0, 1), ivec2(1, 1));

        float planeTolerance = u_DenoiseGuide.x;
        float normalPower = u_DenoiseGuide.y;

        vec3 accumulated = vec3(0.0);
        float weightSum = 0.0;
        // Fallback: the single tap with the best geometric agreement, used when
        // every weighted tap was rejected. Without it a pixel whose whole 2x2
        // footprint belongs to another surface — a thin object against a distant
        // background, most often — would composite a hard zero and read as a
        // black outline.
        vec3 fallbackColor = vec3(0.0);
        float fallbackScore = -1.0;

        for (int i = 0; i < 4; ++i)
        {
            ivec2 tap = clamp(base + offsets[i], ivec2(0), traceSize - 1);
            vec4 signalTap = texelFetch(u_DenoisedSignal, tap, 0);
            if (OloDenoiseIsSky(signalTap.a))
                continue;

            vec2 tapUV = (vec2(tap) + 0.5) * u_TraceParams.zw;
            vec3 tapPositionVS = OloDenoiseViewPosition(u_InvProjection, tapUV, signalTap.a);
            vec3 tapNormalWS = OloDenoiseOctDecode(texelFetch(u_Guide, tap, 0).xy);

            float geometryWeight = OloDenoisePlaneWeight(positionVS, normalVS, tapPositionVS,
                                                         planeTolerance, viewDepth) *
                                   OloDenoiseNormalWeight(normalWS, tapNormalWS, normalPower);
            if (geometryWeight > fallbackScore)
            {
                fallbackScore = geometryWeight;
                fallbackColor = signalTap.rgb;
            }

            float weight = bilinear[i] * geometryWeight;
            if (!(weight > 0.0))
                continue;

            accumulated += weight * signalTap.rgb;
            weightSum += weight;
        }

        indirectDiffuse = weightSum > 0.0 ? accumulated / weightSum : fallbackColor;
    }

    indirectDiffuse = max(indirectDiffuse, vec3(0.0)) * u_ShadeParams.x;

    if (u_Flags.x > 0.5) // debug: the resolved indirect-diffuse contribution in isolation
    {
        o_Color = vec4(indirectDiffuse, 1.0);
        return;
    }

    o_Color = vec4(baseColor + indirectDiffuse, 1.0);
}
