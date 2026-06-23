#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

// Screen-Space Contact Shadows — short-range hard shadows for the sun (deferred path).
//
// The cascaded shadow map is coarse near contact points, so dynamic geometry
// often looks like it floats just above the surface it rests on. For each lit
// opaque pixel this pass reconstructs the view-space position from depth and
// marches a single short ray TOWARD the primary directional light against scene
// depth. If a nearby occluder crosses the ray within a thin thickness window —
// i.e. some on-screen surface sits between the shaded point and the light — the
// pixel is in contact shadow and is darkened. The shadow factor MULTIPLIES the
// lit colour: contact shadows occlude direct light (unlike SSGI, which ADDS
// bounced light, or SSR, which REPLACES with a reflection).
//
// Screen-space only: occluders that are off-screen or hidden behind a nearer
// surface are unknown, so rays that leave the screen contribute nothing and the
// shadow fades toward the screen border. Surfaces that face away from the light
// (N·L <= 0) are already in form shadow handled by the lighting pass, so they
// are skipped.
//
// The math here is mirrored on the CPU by ContactShadowMathTest, and the
// rendered frame is checked by ContactShadowVisualEvidenceTest.

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

layout(binding = 0) uniform sampler2D u_SceneColor;     // lit upstream HDR colour
layout(binding = 19) uniform sampler2D u_DepthTexture;  // scene depth (nonlinear, [0,1])
layout(binding = 44) uniform sampler2D u_GBufferNormal; // RT1: rg = oct world normal, z = roughness, w = ao

layout(std140, binding = 41) uniform ContactShadowParams
{
    mat4 u_Projection;
    mat4 u_InvProjection;
    mat4 u_View;
    vec4 u_LightDirection; // xyz = world TOWARD-light dir (normalized), w = HasDirectionalLight (0/1)
    vec4 u_RayParams;      // x = MaxSteps, y = MaxDistance (view units), z = Thickness, w = Stride (view units)
    vec4 u_ShadeParams;    // x = Intensity, y = EdgeFade (UV), z = Bias (depth-proportional), w = unused
    vec4 u_ScreenParams;   // x = width, y = height, z = 1/width, w = 1/height
    vec4 u_Flags;          // x = DebugView (0/1), yzw = pad
};

const float SKY_DEPTH = 0.999999;
const int HARD_MAX_STEPS = 128; // loop-safety cap; must match kContactShadowMaxSteps
const float MIN_NDOTL = 0.01;   // skip surfaces facing away from the light (form shadow)

// Octahedral decode — matches octEncodeGB() in PBR_GBuffer.glsl / OctDecode() in
// PostProcess_SSGI.glsl.
vec3 OctDecode(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

// Reconstruct view-space position from screen UV + nonlinear depth.
vec3 ViewPosFromDepth(vec2 uv, float depth)
{
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 view = u_InvProjection * ndc;
    return view.xyz / view.w;
}

// Project a view-space position back to screen UV.
vec2 ProjectToUV(vec3 viewPos)
{
    vec4 clip = u_Projection * vec4(viewPos, 1.0);
    vec2 ndc = clip.xy / clip.w;
    return ndc * 0.5 + 0.5;
}

// Interleaved gradient noise (Jimenez 2014) — cheap per-pixel hash in [0,1).
// Used to jitter the march start so the discrete step pattern decorrelates
// between neighbouring pixels (banding -> noise the eye averages out).
float InterleavedGradientNoise(vec2 p)
{
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

void main()
{
    vec3 baseColor = texture(u_SceneColor, v_TexCoord).rgb;

    // No directional light in the scene — nothing casts a contact shadow.
    if (u_LightDirection.w < 0.5)
    {
        o_Color = vec4(baseColor, 1.0);
        return;
    }

    float depth = texture(u_DepthTexture, v_TexCoord).r;
    if (depth >= SKY_DEPTH) // sky / background — receives no contact shadow
    {
        o_Color = vec4(baseColor, 1.0);
        return;
    }

    vec4 gN = texture(u_GBufferNormal, v_TexCoord);
    vec3 Nworld = OctDecode(gN.xy);
    vec3 Lworld = normalize(u_LightDirection.xyz);

    // Surfaces facing away from the light are in form shadow (handled by the
    // lighting pass); contact shadows only matter where the surface faces the
    // light but a nearby occluder blocks it.
    float NdotL = dot(Nworld, Lworld);
    if (NdotL <= MIN_NDOTL)
    {
        o_Color = vec4(baseColor, 1.0);
        return;
    }

    vec3 Nview = normalize(mat3(u_View) * Nworld);
    vec3 Lview = normalize(mat3(u_View) * Lworld);

    vec3 P = ViewPosFromDepth(v_TexCoord, depth); // view-space position (z < 0)

    float maxSteps = u_RayParams.x;
    float maxDist = u_RayParams.y;
    float thickness = u_RayParams.z;
    float stride = u_RayParams.w;
    float intensity = u_ShadeParams.x;
    float edge = u_ShadeParams.y;
    float bias = u_ShadeParams.z;

    // Depth-proportional bias along the normal so the first march step does not
    // self-intersect the originating surface.
    vec3 vStart = P + Nview * (bias * -P.z);

    // Per-pixel sub-step jitter along the ray decorrelates the discrete step
    // pattern between neighbouring pixels, trading hard stair-step banding for
    // fine noise. A half-stride amplitude is enough to break the banding without
    // turning the grazing tail (sparse long-ray hits) into heavy salt-and-pepper;
    // any residual grazing noise is what the engine's TAA resolves in normal use.
    float ign = InterleavedGradientNoise(gl_FragCoord.xy);
    vStart += Lview * (stride * ign * 0.5);

    float occlusion = 0.0;
    float traveled = 0.0;

    for (int s = 0; s < HARD_MAX_STEPS; ++s)
    {
        if (s >= int(maxSteps))
            break;
        traveled += stride;
        if (traveled > maxDist)
            break;

        vec3 rayPos = vStart + Lview * traveled;
        vec2 uv = ProjectToUV(rayPos);
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
            break; // left the screen — no on-screen occluder to find

        float sDepth = texture(u_DepthTexture, uv).r;
        if (sDepth >= SKY_DEPTH)
            continue; // sky behind the ray here — keep marching

        vec3 sPos = ViewPosFromDepth(uv, sDepth);
        float delta = (-rayPos.z) - (-sPos.z); // > 0 => ray is behind the surface (occluded)

        if (delta > 0.0 && delta < thickness)
        {
            // A surface sits between the shaded point and the light. Contact
            // shadows are a SHORT-range grounding effect: a nearby occluder casts
            // a strong shadow that falls off quickly with occluder distance. The
            // squared falloff keeps the shadow tight to the contact and lets the
            // faint, grazing far end fade smoothly to nothing (rather than the
            // stochastic hard-edge speckle a linear cutoff leaves on long rays).
            float distFade = 1.0 - clamp(traveled / maxDist, 0.0, 1.0);
            distFade *= distFade;
            float edgeFade = 1.0;
            if (edge > 0.0)
            {
                edgeFade *= smoothstep(0.0, edge, uv.x) * smoothstep(0.0, edge, 1.0 - uv.x);
                edgeFade *= smoothstep(0.0, edge, uv.y) * smoothstep(0.0, edge, 1.0 - uv.y);
            }
            occlusion = distFade * edgeFade;
            break;
        }
        // delta <= 0 (ray still in front of the surface) or delta >= thickness
        // (the surface is the distant background, not a thin occluder): keep
        // marching toward the light.
    }

    // Multiplicative darkening: 1 = fully lit, (1 - intensity) at full occlusion.
    float shadowFactor = 1.0 - clamp(occlusion * intensity, 0.0, 1.0);

    if (u_Flags.x > 0.5) // debug: show the shadow factor as greyscale
    {
        o_Color = vec4(vec3(shadowFactor), 1.0);
        return;
    }

    o_Color = vec4(baseColor * shadowFactor, 1.0);
}
