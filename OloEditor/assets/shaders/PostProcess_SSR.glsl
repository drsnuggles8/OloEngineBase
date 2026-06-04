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

// Screen-Space Reflections (deferred path).
//
// For each opaque pixel, reconstruct its view-space position from depth, build
// the view-space reflection vector from the G-Buffer world normal, and march it
// against the scene depth buffer (linear steps + binary-search refinement). On
// a hit, sample the lit scene colour at the hit UV and composite it additively,
// weighted by Fresnel, roughness fade, and screen-edge / distance / facing
// fades. Reflections of opaque geometry can only contain what is already on
// screen, so off-screen rays fade out gracefully.
//
// The math here is mirrored on the CPU by ScreenSpaceReflectionMathTest, and the
// rendered frame is checked by SSRVisualEvidenceTest.

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

layout(binding = 0) uniform sampler2D u_SceneColor;     // lit upstream HDR colour (reflection source)
layout(binding = 19) uniform sampler2D u_DepthTexture;  // scene depth (nonlinear, [0,1])
layout(binding = 44) uniform sampler2D u_GBufferNormal; // RT1: rg = oct world normal, z = roughness, w = ao
layout(binding = 43) uniform sampler2D u_GBufferAlbedo; // RT0: rgb = albedo, a = metallic

layout(std140, binding = 38) uniform SSRParams
{
    mat4 u_Projection;
    mat4 u_InvProjection;
    mat4 u_View;
    vec4 u_RayParams;    // x = MaxSteps, y = MaxDistance, z = Thickness, w = Stride (view units)
    vec4 u_ShadeParams;  // x = Intensity, y = MaxRoughness, z = EdgeFade (UV), w = BinarySearchSteps
    vec4 u_ScreenParams; // x = width, y = height, z = 1/width, w = 1/height
    vec4 u_Flags;        // x = DebugView (0/1)
};

const float MIN_ROUGHNESS = 0.045;
const float SKY_DEPTH = 0.999999;
const int HARD_MAX_STEPS = 256;
const int HARD_MAX_BIN_STEPS = 32;

// Octahedral decode — matches octEncodeGB() in PBR_GBuffer.glsl.
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

void main()
{
    vec3 baseColor = texture(u_SceneColor, v_TexCoord).rgb;

    float depth = texture(u_DepthTexture, v_TexCoord).r;
    if (depth >= SKY_DEPTH) // sky / background — nothing to reflect from
    {
        o_Color = vec4(baseColor, 1.0);
        return;
    }

    vec4 gN = texture(u_GBufferNormal, v_TexCoord);
    float roughness = max(gN.z, MIN_ROUGHNESS);
    float maxRoughness = u_ShadeParams.y;

    // Rougher-than-cutoff surfaces receive no SSR (sharp mirror reflections only).
    float roughFade = 1.0 - smoothstep(maxRoughness * 0.75, maxRoughness, roughness);
    if (roughFade <= 0.0)
    {
        o_Color = vec4(baseColor, 1.0);
        return;
    }

    vec4 gA = texture(u_GBufferAlbedo, v_TexCoord);
    vec3 albedo = gA.rgb;
    float metallic = gA.a;

    // World normal -> view space.
    vec3 Nworld = OctDecode(gN.xy);
    vec3 Nview = normalize(mat3(u_View) * Nworld);

    vec3 P = ViewPosFromDepth(v_TexCoord, depth); // view-space position (z < 0)
    vec3 V = normalize(P);                         // eye -> fragment direction
    vec3 R = normalize(reflect(V, Nview));         // reflected ray direction

    float maxSteps = u_RayParams.x;
    float maxDist = u_RayParams.y;
    float thickness = u_RayParams.z;
    float stride = u_RayParams.w;
    int binSteps = int(u_ShadeParams.w);

    // Depth-proportional bias along the normal so the first step does not
    // self-intersect the originating surface.
    vec3 rayPos = P + Nview * (0.02 * -P.z);
    vec3 prevPos = rayPos;
    float traveled = 0.0;
    float stepLen = stride;

    bool hit = false;
    vec2 hitUV = vec2(0.0);

    for (int i = 0; i < HARD_MAX_STEPS; ++i)
    {
        if (i >= int(maxSteps))
            break;

        prevPos = rayPos;
        rayPos += R * stepLen;
        traveled += stepLen;
        if (traveled > maxDist)
            break;
        if (rayPos.z >= 0.0) // crossed in front of the eye
            break;

        vec2 uv = ProjectToUV(rayPos);
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
            break;

        float sDepth = texture(u_DepthTexture, uv).r;
        if (sDepth >= SKY_DEPTH) // marched over the sky; keep going
        {
            stepLen *= 1.04;
            continue;
        }

        vec3 sPos = ViewPosFromDepth(uv, sDepth);
        // Positive linear depths: rayL/sceneL = distance in front of camera.
        float deltaL = (-rayPos.z) - (-sPos.z); // > 0 => ray is behind the surface

        if (deltaL > 0.0 && deltaL < thickness)
        {
            // Refine the crossing between prevPos (in front) and rayPos (behind).
            vec3 lo = prevPos;
            vec3 hi = rayPos;
            for (int b = 0; b < HARD_MAX_BIN_STEPS; ++b)
            {
                if (b >= binSteps)
                    break;
                vec3 mid = (lo + hi) * 0.5;
                vec2 muv = ProjectToUV(mid);
                float md = texture(u_DepthTexture, muv).r;
                vec3 mpos = ViewPosFromDepth(muv, md);
                float mdl = (-mid.z) - (-mpos.z);
                if (mdl > 0.0)
                    hi = mid;
                else
                    lo = mid;
            }
            hitUV = ProjectToUV(hi);
            hit = true;
            break;
        }

        stepLen *= 1.04; // gentle acceleration in empty space
    }

    if (!hit)
    {
        o_Color = vec4(baseColor, 1.0);
        return;
    }

    // ---- Confidence fades -------------------------------------------------
    float edge = u_ShadeParams.z;
    float edgeFade = 1.0;
    if (edge > 0.0)
    {
        edgeFade *= smoothstep(0.0, edge, hitUV.x) * smoothstep(0.0, edge, 1.0 - hitUV.x);
        edgeFade *= smoothstep(0.0, edge, hitUV.y) * smoothstep(0.0, edge, 1.0 - hitUV.y);
    }

    float distFade = 1.0 - clamp(traveled / maxDist, 0.0, 1.0);

    // Fade rays that reflect back toward the camera (content likely off-screen).
    float backFacing = dot(R, -V); // ~1 = straight back at the eye
    float facingFade = 1.0 - smoothstep(0.25, 0.6, backFacing);

    // Fresnel (Schlick): dielectrics ~0.04, metals reflect strongly.
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    float cosTheta = clamp(dot(-V, Nview), 0.0, 1.0);
    vec3 fresnel = F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
    float fresnelScalar = dot(fresnel, vec3(0.299, 0.587, 0.114)); // perceptual reflectance

    // The reflected radiance, tinted by the surface's specular colour: metals
    // colour their reflection by albedo, dielectrics reflect untinted.
    vec3 reflColor = texture(u_SceneColor, hitUV).rgb;
    vec3 reflTint = mix(vec3(1.0), albedo, metallic);
    vec3 reflTarget = reflColor * reflTint;

    // Blend factor: how much of the reflection REPLACES the lit colour, gated by
    // surface reflectance and geometric confidence. Standard SSR resolve — a
    // lerp, not an add: baseColor on a reflective surface already contains the
    // IBL/background reflection, so adding the SSR reflection on top double-counts
    // and washes out (sky + object). Replacing it lets the on-screen object
    // reflection occlude the background reflection the way a real mirror does.
    // Metals (high F0) approach a full mirror; dielectrics get a faint,
    // grazing-weighted sheen; an SSR miss leaves baseColor untouched.
    float blend = clamp(fresnelScalar * roughFade * edgeFade * distFade * facingFade * u_ShadeParams.x, 0.0, 1.0);

    if (u_Flags.x > 0.5) // debug: show the (blended) reflection contribution in isolation
    {
        o_Color = vec4(reflTarget * blend, 1.0);
        return;
    }

    o_Color = vec4(mix(baseColor, reflTarget, blend), 1.0);
}
