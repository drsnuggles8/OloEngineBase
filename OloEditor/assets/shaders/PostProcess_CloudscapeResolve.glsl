// =============================================================================
// PostProcess_CloudscapeResolve.glsl — cloud temporal accumulation (pass B)
//
// Half-resolution: blends this frame's jittered raymarch (pass A) with the
// reprojected history through the shared kernel in
// include/TemporalResolve.glsl (issue #903). Until then this pass carried its
// own resolve — a componentwise RGB min/max clamp and a bare mix() — which was
// the engine's second implementation of the same four questions and the weaker
// one: no variance box, and a clamp that can move a rejected history onto a
// colour the neighbourhood never contained.
//
// The one thing that is genuinely this pass's own is that the signal is RGBA
// and alpha is TRANSMITTANCE, not a colour channel. See the block above
// OloTemporalResolveRGBA in the header, and the call site below.
//
// The graph extracts this pass's output into the history sink each frame
// (TAA RegisterHistoryTextureSink/ExtractHistoryTexture pattern), so history
// validity is handled by the pipeline: u_CloudMisc.x (temporal blend) is
// forced to 0 on the first frame / after invalidation.
// =============================================================================

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

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Cloud;

#include "include/NoiseCommon.glsl"
#include "include/CloudscapeCommon.glsl"

#include "include/BindlessHeap.glsl"
#ifdef OLO_BINDLESS
#define u_CloudCurrent OLO_HEAP_TEX_2D(0)  // pass A output (half-res) — TEX_DIFFUSE
#define u_CloudHistory OLO_HEAP_TEX_2D(1)  // last frame's resolve (half-res) — TEX_SPECULAR
#else
layout(binding = 0) uniform sampler2D u_CloudCurrent; // pass A output (half-res)
layout(binding = 1) uniform sampler2D u_CloudHistory; // last frame's resolve (half-res)
#endif

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    mat4 u_PrevViewProjection;
    vec3 u_RenderOrigin;
    float _padding1;
};

layout(std140, binding = 8) uniform MotionBlurUBO {
    mat4 u_InverseViewProjection;
    mat4 u_MB_PrevViewProjection; // ABSOLUTE world -> previous clip
};

// The shared temporal kernel (issue #706, adopted here by #903). This pass used
// to hand-roll the last three of the resolve's four questions — a componentwise
// RGB min/max clamp with no variance box, and a bare mix() — which is exactly
// the second implementation the header exists to remove.
#include "include/TemporalResolve.glsl"

// The clip box is mean +/- kClipGamma * stddev, intersected with the hard 3x3
// min/max. 1.25 is the value TAA uses and it transfers for the same reason it
// works there: the box is built from THIS frame's neighbourhood, so where the
// raymarch's per-frame step jitter makes the signal noisy the stddev is large
// and the box opens to admit it, and where the deck is smooth the stddev is
// small but so is the distance from the history to the mean. It is not a
// tightness knob applied to a fixed amount of noise.
const float kClipGamma = 1.25;

void main()
{
    vec4 current = texture(u_CloudCurrent, v_TexCoord);
    float blend = u_CloudMisc.x;
    if (blend <= 0.0 || u_CloudMisc.w < 0.5)
    {
        o_Cloud = current;
        return;
    }

    // -------------------------------------------------------------------------
    // 1) WHERE was this pixel last frame?
    //
    // Clouds have no depth buffer, so the reprojection anchor is the view ray's
    // cloud-layer midpoint (or a fixed far distance when the ray misses the
    // layer — sky rotation). Stable, and cheap to recompute here without
    // carrying depth through pass A.
    // -------------------------------------------------------------------------
    vec4 ndc = vec4(v_TexCoord * 2.0 - 1.0, 1.0, 1.0);
    vec4 worldFar = u_InverseViewProjection * ndc;
    worldFar.xyz /= worldFar.w;
    vec3 cameraPos = u_CameraPosition + u_RenderOrigin;
    vec3 rayDir = normalize((worldFar.xyz + u_RenderOrigin) - cameraPos);
    vec2 slab = cloudLayerIntersect(cameraPos, rayDir);
    float tMid = (slab.y > slab.x) ? 0.5 * (slab.x + slab.y) : 20000.0;
    vec3 worldMid = cameraPos + rayDir * tMid;

    // Reproject into last frame (absolute-world prev VP, camera-relative #429).
    bool inFrontOfPrevCamera;
    vec2 prevUV = OloTemporalReprojectWorldPoint(u_MB_PrevViewProjection, worldMid, inFrontOfPrevCamera);
    if (!inFrontOfPrevCamera)
    {
        o_Cloud = current;
        return;
    }

    // -------------------------------------------------------------------------
    // 2) Is that history the SAME SURFACE?
    //
    // Two rejections, and they are the only two available here. There is no
    // depth history to feed OloTemporalDepthConfidence: the anchor above is a
    // point on a fitted layer, not a surface the frame measured, and the pass
    // carries nothing from last frame but the resolved RGBA. So the disocclusion
    // half of the kernel is answered by an off-screen test and by the #987
    // occlusion gate, and a depth-confidence term would need pass A to publish a
    // depth channel first.
    // -------------------------------------------------------------------------
    if (!OloTemporalHistoryUVValid(prevUV))
    {
        o_Cloud = current;
        return;
    }

    // A RAY THAT GEOMETRY OCCLUDED HAS NO HISTORY (issue #987).
    //
    // The raymarch writes exactly (rgb = 0, a = 1) when the ray never reaches
    // the cloud layer — including when it stops on terrain. The neighbourhood
    // clip below cannot protect that case, because it widens the box with the
    // SKY texels next door: at an island's ridge the neighbourhood legitimately
    // contains cloud, so last frame's cloud stays inside the allowed range and
    // is blended onto a texel that is now solid rock. Measured on Drift: zero
    // cloud texels over terrain in the raymarch output, six in the resolved
    // buffer — six half-res texels, but they upsample into a clearly visible
    // white patch on the mountain.
    //
    // So: when this frame says "nothing in front of me", that is an answer, not
    // a missing sample. Take it and skip history entirely. Clear sky hits this
    // path too and is unaffected — clear blended with clear is clear.
    if (current.a >= 0.999 && all(lessThan(current.rgb, vec3(1.0e-4))))
    {
        o_Cloud = current;
        return;
    }

    // -------------------------------------------------------------------------
    // 3) Is that history's VALUE still plausible?   4) How much do I keep?
    //
    // Both from the shared kernel. rgb is variance-clipped in YCoCg; alpha is
    // transmittance and is clipped in its own 1-D space — see "Channels that
    // are NOT colour" in include/TemporalResolve.glsl for why it must not go
    // through the colour transform, and why the clamp that implements a 1-D
    // clip is not the componentwise clamp this pass just stopped doing.
    //
    // NO motion-scaled feedback here, deliberately, and this is the one place
    // the cloudscape does not want what TAA wants. OloTemporalMotionFeedback
    // exists because TAA's reprojection is least trustworthy exactly where
    // motion is largest — at the silhouettes of moving objects, which velocity
    // dilation only half-fixes. This pass has no moving objects and no velocity
    // buffer: a camera pan reprojects the layer analytically and is *correct*,
    // while the signal it is accumulating is a jittered raymarch that needs the
    // history most while the camera moves. Cutting feedback on motion here would
    // trade a ghost this pass does not have for noise it does.
    // -------------------------------------------------------------------------
    vec4 history = texture(u_CloudHistory, prevUV);

    vec2 texel = 1.0 / vec2(textureSize(u_CloudCurrent, 0));
    OloTemporalStats stats;
    OloTemporalScalarStats alphaStats;
    OLO_TEMPORAL_GATHER_3X3_RGBA(u_CloudCurrent, v_TexCoord, texel, stats, alphaStats);

    o_Cloud = OloTemporalResolveRGBA(current, history, stats, alphaStats, kClipGamma, blend, 1.0);
}
