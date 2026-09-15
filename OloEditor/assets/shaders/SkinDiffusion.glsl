#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5): on the Vulkan backend vertex data is PULLED —
// binding 57 is the engine-wide vertex-pull binding; the root struct carries
// this buffer's device address, so the SAME 20-byte {vec3 position, vec2 uv}
// stream the attribute path consumes is read by index instead.
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

// =============================================================================
// SkinDiffusion.glsl — one axis of the separable skin diffusion, issue #1241.
//
// THIS SHADER KNOWS NO PHYSICS. It is a weighted sum along one axis with a
// bilateral guard. Every physical decision — which diffusion profile, how
// transport albedo becomes a scaling, why the weights come from the profile's
// LINE spread function rather than from the profile itself — was made on the
// CPU in Renderer/SkinDiffusion.cpp, where a test can look at it, and arrives
// here as a table of (offset, per-channel weight). A diffusion profile evaluated
// in GLSL is a thing nothing can check.
//
// IT IS RUN TWICE. Pass 0 blurs horizontally into a scratch target; pass 1 blurs
// that vertically and outputs `blurred - original`, which the pass ADDS into
// scene colour. The difference, rather than the result, is what makes the whole
// feature fail safe: scene colour already holds the sharp composite, so a frame
// where this pass does not run is the #1231 frame and not a head with no diffuse
// lighting. See include/SkinDiffusionCommon.glsl.
//
// WHAT IT DOES NOT DO. It never touches the specular half — that half is in
// scene colour and was never handed over, which is the entire argument for
// #1231's split and the reason the issue's title is about sharp specular. It
// also does not renormalise by the weights it actually used: rejected energy
// goes back on the CENTRE tap instead, because a filter that renormalises
// per pixel turns every depth discontinuity into a brightness change.
// =============================================================================

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Result;

#include "include/SkinDiffusionCommon.glsl"
#include "include/BindlessHeap.glsl"

// Heap-bindless conversion (issue #691). The BODY below is byte-identical
// between the two variants — only these declarations move, and each names the
// same TEX_* constant SkinDiffusionPass binds with.
#ifdef OLO_BINDLESS
#define u_SkinDiffuseSource OLO_HEAP_TEX_2D(0)
#define u_SkinDiffuseOrigin OLO_HEAP_TEX_2D(1)
#define u_SceneDepth        OLO_HEAP_TEX_2D(19) // TEX_POSTPROCESS_DEPTH
#else
// Pass 0: the scene framebuffer's skin-diffuse attachment. Pass 1: the scratch
// target pass 0 wrote.
layout(binding = 0) uniform sampler2D u_SkinDiffuseSource;
// The UNBLURRED hand-off, both passes. Pass 1 subtracts it; pass 0 ignores it
// (it IS the source there, and binding the same texture twice is cheaper than a
// shader variant).
layout(binding = 1) uniform sampler2D u_SkinDiffuseOrigin;
// Non-linear depth, for the screen-space radius and the bilateral guard.
layout(binding = 19) uniform sampler2D u_SceneDepth;
#endif

// The tap table is sized to the HIGH tier and a lower tier fills fewer entries,
// so changing quality never resizes the block — which would mean a shader
// recompile mid-session. Must match kMaxSkinDiffusionTaps / kMaxSkinProfileSlots
// in Renderer/SkinDiffusion.h and Renderer/SkinProfile.h; SkinDiffusionUBOData
// in Renderer/PostProcessSettings.h static_asserts the resulting size.
#define OLO_SKIN_DIFFUSION_MAX_TAPS 25
#define OLO_SKIN_DIFFUSION_MAX_SLOTS 7

// Shares UBO binding 14 with SSS_Blur.glsl's SSSParams — see UBO_SSS in
// Renderer/ShaderBindingLayout.h. The two are different passes with different
// blocks and neither includes the other; the rule that has to hold is the
// within-shader one, and it does.
layout(std140, binding = 14) uniform SkinDiffusionParams {
    // x = tap count, y = axis (0 = horizontal, 1 = vertical),
    // z = target width in pixels, w = target height in pixels
    vec4 u_SkinDiffusionPass;
    // x = projection scale Y (P[1][1]),
    // y = P[2][2] and z = P[3][2] -- the depth linearisation pair, exactly as
    //     GTAO.comp's u_DepthLinearize{A,B}. Taken from the projection matrix
    //     rather than as near/far because that form works for any projection the
    //     camera actually has, including an infinite far plane,
    // w = depth rejection scale, as a multiple of the kernel's world support
    vec4 u_SkinDiffusionProjection;
    // Per slot: x = support radius MILLIMETRES (0 disables the slot),
    // y = radius scale, zw reserved
    vec4 u_SkinDiffusionSlots[OLO_SKIN_DIFFUSION_MAX_SLOTS];
    // Per slot, per tap: x = normalised offset in [-1, 1], yzw = channel weights
    vec4 u_SkinDiffusionTaps[OLO_SKIN_DIFFUSION_MAX_SLOTS * OLO_SKIN_DIFFUSION_MAX_TAPS];
};

// One world unit is one metre, so a millimetre is a thousandth of one. Mirrors
// kSkinWorldUnitsPerMillimetre in Renderer/SkinDiffusion.h — the ONE unit
// conversion in the feature, and the one SkinDiffusionTest pins.
#define OLO_SKIN_MM_TO_WORLD 0.001
// Mirrors kMaxSkinDiffusionRadiusPixels / kMinSkinDiffusionRadiusPixels.
#define OLO_SKIN_MAX_RADIUS_PIXELS 64.0
#define OLO_SKIN_MIN_RADIUS_PIXELS 0.5

// [0,1] device-Z to positive view-space distance, in world units. The same
// expression, from the same two projection coefficients, as GTAO.comp's
// LinearizeDepth -- two shaders disagreeing about what a depth texel means is
// the kind of difference that shows up as one effect's artefacts and gets
// chased in the other's file.
float linearizeDepth(float deviceZ)
{
    float ndc = deviceZ * 2.0 - 1.0;
    return u_SkinDiffusionProjection.z / (ndc + u_SkinDiffusionProjection.y);
}

void main()
{
    vec4 centre = texture(u_SkinDiffuseSource, v_TexCoord);
    int slot = oloSkinDiffusionSlot(centre.a);

    // Not skin, or a profile that does not diffuse. Pass 0 must still PROPAGATE
    // the texel (the vertical pass reads its output), and pass 1 must contribute
    // exactly nothing to the additive blend.
    bool verticalPass = u_SkinDiffusionPass.y > 0.5;
    if (slot >= OLO_SKIN_DIFFUSE_SLOT_NONE)
    {
        o_Result = verticalPass ? vec4(0.0) : centre;
        return;
    }

    float supportMM = u_SkinDiffusionSlots[slot].x * u_SkinDiffusionSlots[slot].y;
    float centreDepth = linearizeDepth(texture(u_SceneDepth, v_TexCoord).r);
    // A pixel at or behind the eye, or a projection this expression cannot
    // linearise, would divide the radius below by something meaningless. There
    // is nothing to diffuse there either way.
    if (!(centreDepth > 0.0))
    {
        o_Result = verticalPass ? vec4(0.0) : centre;
        return;
    }

    // THE PROJECTION. A length perpendicular to the view direction subtends
    // `length * (0.5 * viewportHeight * P[1][1]) / depth` pixels. Height and
    // P[1][1] both arrive from the CPU rather than being assumed, which is what
    // makes the radius follow a resolution change and a field-of-view change
    // instead of only looking like it does — SkinDiffusionTest pins the same
    // expression on the CPU side.
    float targetHeight = u_SkinDiffusionPass.w;
    float radiusPixels = (supportMM * OLO_SKIN_MM_TO_WORLD) *
                         (0.5 * targetHeight * u_SkinDiffusionProjection.x) / max(centreDepth, 1e-4);
    radiusPixels = clamp(radiusPixels, 0.0, OLO_SKIN_MAX_RADIUS_PIXELS);

    // Below half a texel the kernel cannot express anything a bilinear fetch has
    // not already done, and every tap would land on the centre.
    if (radiusPixels < OLO_SKIN_MIN_RADIUS_PIXELS)
    {
        o_Result = verticalPass ? vec4(0.0) : centre;
        return;
    }

    vec2 texelSize = vec2(1.0 / max(u_SkinDiffusionPass.z, 1.0), 1.0 / max(u_SkinDiffusionPass.w, 1.0));
    vec2 axis = verticalPass ? vec2(0.0, 1.0) : vec2(1.0, 0.0);
    vec2 stepUV = axis * texelSize * radiusPixels;

    // THE BILATERAL THRESHOLD IS IN WORLD UNITS AND SCALES WITH THE KERNEL. A
    // tap two scattering radii deeper than the centre is on another surface,
    // whatever the camera is doing; a threshold fixed in metres would mean
    // something different on a close-up and at conversational distance.
    float depthThreshold = supportMM * OLO_SKIN_MM_TO_WORLD * u_SkinDiffusionProjection.w;

    int tapCount = int(u_SkinDiffusionPass.x + 0.5);
    int tapBase = slot * OLO_SKIN_DIFFUSION_MAX_TAPS;

    vec3 accum = vec3(0.0);
    // The energy of every tap that was REJECTED, per channel. It goes back on
    // the centre sample rather than into a renormalising divide: rejecting a
    // tap means "no surface over there to have scattered light from", and the
    // light that would have come from there did not stop existing, it stayed
    // where it was. Renormalising instead would make a silhouette brighten.
    vec3 rejected = vec3(0.0);

    for (int i = 0; i < tapCount; ++i)
    {
        vec4 tap = u_SkinDiffusionTaps[tapBase + i];
        vec3 weight = tap.yzw;
        vec2 uv = v_TexCoord + stepUV * tap.x;

        // SCREEN EDGES. A tap off the target has no data at all, and clamping to
        // the border would smear the edge texel across the whole kernel — which
        // is exactly the artefact the fourth acceptance criterion is about. Off
        // the screen is a rejection like any other.
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        {
            rejected += weight;
            continue;
        }

        vec4 sampled = texture(u_SkinDiffuseSource, uv);

        // CROSS-OBJECT BLEEDING, GUARD ONE: the profile identity. Two heads with
        // different profiles overlapping on screen must not exchange light, and
        // neither must a head and the wall behind it. This is an exact integer
        // test, not a tolerance.
        if (oloSkinDiffusionSlot(sampled.a) != slot)
        {
            rejected += weight;
            continue;
        }

        // GUARD TWO: depth. Same profile, different surface — an ear in front of
        // a cheek, the far side of a nose. The slot test cannot see that.
        float tapDepth = linearizeDepth(texture(u_SceneDepth, uv).r);
        if (abs(tapDepth - centreDepth) > depthThreshold)
        {
            rejected += weight;
            continue;
        }

        accum += sampled.rgb * weight;
    }

    accum += centre.rgb * rejected;

    if (verticalPass)
    {
        // The DIFFERENCE, additively blended into scene colour by the pass. The
        // unblurred hand-off is what scene colour already contains; subtracting
        // it and adding the blur replaces one with the other without this shader
        // ever reading — or being able to disturb — the specular half.
        //
        // Alpha stays 0: scene colour's alpha is snow's transient SSS mask
        // (SnowCommon.glsl) and the blend is set up to leave it alone, but
        // writing a zero rather than a don't-care is what makes that true on
        // every backend.
        vec3 origin = texture(u_SkinDiffuseOrigin, v_TexCoord).rgb;
        o_Result = vec4(accum - origin, 0.0);
    }
    else
    {
        // Carry the identity through to the vertical pass. It is the SAME slot —
        // a horizontal blur cannot move a pixel onto another surface, because
        // every tap that was on another surface was rejected above.
        o_Result = vec4(accum, centre.a);
    }
}
