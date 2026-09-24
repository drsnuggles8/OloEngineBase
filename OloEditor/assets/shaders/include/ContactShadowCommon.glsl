// =============================================================================
// ContactShadowCommon.glsl — screen-space contact shadows for the sun.
//
// The cascaded shadow map is coarse near contact points, so dynamic geometry
// often looks like it floats just above the surface it rests on. For a lit
// opaque pixel this reconstructs the view-space position from depth and marches
// a single short ray TOWARD the primary directional light against scene depth.
// If a nearby occluder crosses the ray within a thin thickness window — some
// on-screen surface sits between the shaded point and the light — the pixel is
// in contact shadow.
//
// WHAT IT OCCLUDES (issue #1336). The result is a VISIBILITY for ONE light: the
// primary directional light, which Scene packs at Lights[0]. DeferredLighting
// multiplies it into that light's visibility inside its loop, beside the
// CSM / VSM / ray-traced factor and the cloud shadow, so it darkens the sun's
// direct light and nothing else. It used to multiply the finished frame in a
// post pass, which also darkened ambient, emission, reflections, every other
// light and the traced indirect tiers — none of which the sun's occluder
// blocks. PostProcess_ContactShadow keeps only the debug view.
//
// Screen-space only: occluders that are off-screen or hidden behind a nearer
// surface are unknown, so rays that leave the screen contribute nothing and the
// shadow fades toward the screen border. Surfaces that face away from the light
// (N.L <= 0) are in form shadow already and are skipped — which is also what
// keeps a backlit leaf's or ear's transmission out of it.
//
// The caller defines OLO_CONTACT_SHADOW_TAP_DEPTH(uv) to return the depth
// buffer's [0,1] window depth at `uv` before including this file. The math is
// mirrored on the CPU by ContactShadowMathTest.
// =============================================================================

#ifndef CONTACT_SHADOW_COMMON_GLSL
#define CONTACT_SHADOW_COMMON_GLSL

#ifndef OLO_CONTACT_SHADOW_TAP_DEPTH
#error "Define OLO_CONTACT_SHADOW_TAP_DEPTH(uv) before including ContactShadowCommon.glsl"
#endif

// UBO_CONTACT_SHADOW (41), uploaded by RenderPipeline when contact shadows are
// on. An INSTANCE name, because its includers already declare camera blocks
// whose members (u_View, u_Projection) are in the global scope.
layout(std140, binding = 41) uniform ContactShadowParams
{
    mat4 Projection;
    mat4 InvProjection;
    mat4 View;
    vec4 LightDirection; // xyz = world TOWARD-light dir (normalized), w = HasDirectionalLight (0/1)
    vec4 RayParams;      // x = MaxSteps, y = MaxDistance (view units), z = Thickness, w = Stride (view units)
    vec4 ShadeParams;    // x = Intensity, y = EdgeFade (UV), z = Bias (depth-proportional), w = unused
    vec4 ScreenParams;   // x = width, y = height, z = 1/width, w = 1/height
    vec4 Flags;          // x = DebugView (0/1), yzw = pad
} u_ContactShadow;

const float OLO_CONTACT_SHADOW_SKY_DEPTH = 0.999999;
const int OLO_CONTACT_SHADOW_HARD_MAX_STEPS = 128; // loop-safety cap; must match kContactShadowMaxSteps
const float OLO_CONTACT_SHADOW_MIN_NDOTL = 0.01;   // skip surfaces facing away from the light (form shadow)

// Reconstruct view-space position from screen UV + nonlinear depth.
vec3 oloContactShadowViewPos(vec2 uv, float depth)
{
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 view = u_ContactShadow.InvProjection * ndc;
    return view.xyz / view.w;
}

// Project a view-space position back to screen UV.
vec2 oloContactShadowProjectToUV(vec3 viewPos)
{
    vec4 clip = u_ContactShadow.Projection * vec4(viewPos, 1.0);
    vec2 ndc = clip.xy / clip.w;
    return ndc * 0.5 + 0.5;
}

// Interleaved gradient noise (Jimenez 2014) — cheap per-pixel hash in [0,1).
// Jitters the march start so the discrete step pattern decorrelates between
// neighbouring pixels (banding -> noise the eye averages out).
float oloContactShadowIGN(vec2 p)
{
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// The primary directional light's contact-shadow VISIBILITY at this pixel:
// 1 = lit, (1 - intensity) at full occlusion. `depth` is the pixel's own window
// depth, `Nworld` its world normal, `fragCoord` its window position (for the
// jitter).
float oloContactShadowVisibility(vec2 uv, float depth, vec3 Nworld, vec2 fragCoord)
{
    // No directional light in the scene — nothing casts a contact shadow.
    if (u_ContactShadow.LightDirection.w < 0.5)
        return 1.0;
    if (depth >= OLO_CONTACT_SHADOW_SKY_DEPTH) // sky / background
        return 1.0;

    vec3 Lworld = normalize(u_ContactShadow.LightDirection.xyz);
    // Surfaces facing away from the light are in form shadow already.
    if (dot(Nworld, Lworld) <= OLO_CONTACT_SHADOW_MIN_NDOTL)
        return 1.0;

    vec3 Nview = normalize(mat3(u_ContactShadow.View) * Nworld);
    vec3 Lview = normalize(mat3(u_ContactShadow.View) * Lworld);

    vec3 P = oloContactShadowViewPos(uv, depth); // view-space position (z < 0)

    float maxSteps = u_ContactShadow.RayParams.x;
    float maxDist = u_ContactShadow.RayParams.y;
    float thickness = u_ContactShadow.RayParams.z;
    float stride = u_ContactShadow.RayParams.w;
    float intensity = u_ContactShadow.ShadeParams.x;
    float edge = u_ContactShadow.ShadeParams.y;
    float bias = u_ContactShadow.ShadeParams.z;

    // Depth-proportional bias along the normal so the first march step does not
    // self-intersect the originating surface.
    vec3 vStart = P + Nview * (bias * -P.z);

    // Per-pixel sub-step jitter along the ray: hard stair-step banding becomes
    // fine noise. Half a stride breaks the banding without turning the grazing
    // tail into heavy salt-and-pepper; TAA resolves the residue.
    vStart += Lview * (stride * oloContactShadowIGN(fragCoord) * 0.5);

    float occlusion = 0.0;
    float traveled = 0.0;
    for (int s = 0; s < OLO_CONTACT_SHADOW_HARD_MAX_STEPS; ++s)
    {
        if (s >= int(maxSteps))
            break;
        traveled += stride;
        if (traveled > maxDist)
            break;

        vec3 rayPos = vStart + Lview * traveled;
        vec2 rayUV = oloContactShadowProjectToUV(rayPos);
        if (rayUV.x < 0.0 || rayUV.x > 1.0 || rayUV.y < 0.0 || rayUV.y > 1.0)
            break; // left the screen — no on-screen occluder to find

        float sDepth = OLO_CONTACT_SHADOW_TAP_DEPTH(rayUV);
        if (sDepth >= OLO_CONTACT_SHADOW_SKY_DEPTH)
            continue; // sky behind the ray here — keep marching

        vec3 sPos = oloContactShadowViewPos(rayUV, sDepth);
        float delta = (-rayPos.z) - (-sPos.z); // > 0 => ray is behind the surface (occluded)

        if (delta > 0.0 && delta < thickness)
        {
            // A short-range grounding effect: a nearby occluder casts a strong
            // shadow that falls off quickly with occluder distance. The squared
            // falloff keeps it tight to the contact and lets the faint grazing
            // far end fade smoothly to nothing.
            float distFade = 1.0 - clamp(traveled / maxDist, 0.0, 1.0);
            distFade *= distFade;
            float edgeFade = 1.0;
            if (edge > 0.0)
            {
                edgeFade *= smoothstep(0.0, edge, rayUV.x) * smoothstep(0.0, edge, 1.0 - rayUV.x);
                edgeFade *= smoothstep(0.0, edge, rayUV.y) * smoothstep(0.0, edge, 1.0 - rayUV.y);
            }
            occlusion = distFade * edgeFade;
            break;
        }
        // delta <= 0 (ray still in front of the surface) or delta >= thickness
        // (the surface is the distant background, not a thin occluder): keep
        // marching toward the light.
    }

    return 1.0 - clamp(occlusion * intensity, 0.0, 1.0);
}

#endif // CONTACT_SHADOW_COMMON_GLSL
