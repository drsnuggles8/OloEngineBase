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

// Screen-Space Global Illumination — one-bounce indirect *diffuse* (deferred path).
//
// For each opaque pixel, reconstruct its view-space position + normal from the
// G-Buffer, then cast a cosine-weighted hemisphere of short rays around the
// normal. Each ray linear-marches in view space against scene depth; on a hit
// (the ray passes just behind a visible surface, within a thickness tolerance)
// the lit scene colour at the hit UV is the incoming indirect radiance. The
// average radiance over the hemisphere, tinted by the receiver albedo, is the
// one-bounce indirect diffuse — so a saturated wall bleeds its colour onto a
// neutral floor. Unlike SSR (a replace/mix that substitutes a mirror
// reflection), indirect diffuse is *extra* bounced light, so it is ADDED to the
// lit colour, weighted by the SSGI intensity.
//
// Cosine-weighted importance sampling (Malley's method) means the Monte-Carlo
// estimator of the diffuse irradiance integral is simply the mean of the
// per-ray radiance: Lo = albedo * (1/N) * sum(Li). Rays that leave the screen or
// hit the sky see no on-screen light and contribute zero — screen-space GI can
// only gather what is already on screen, so it fades out at screen borders.
//
// The math here is mirrored on the CPU by ScreenSpaceGIMathTest, and the
// rendered frame is checked by SSGIVisualEvidenceTest.

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

layout(binding = 0) uniform sampler2D u_SceneColor;     // lit upstream HDR colour (indirect light source)
layout(binding = 19) uniform sampler2D u_DepthTexture;  // scene depth (nonlinear, [0,1])
layout(binding = 44) uniform sampler2D u_GBufferNormal; // RT1: rg = oct world normal, z = roughness, w = ao
layout(binding = 43) uniform sampler2D u_GBufferAlbedo; // RT0: rgb = albedo, a = metallic

layout(std140, binding = 40) uniform SSGIParams
{
    mat4 u_Projection;
    mat4 u_InvProjection;
    mat4 u_View;
    vec4 u_RayParams;    // x = MaxSteps, y = MaxDistance (view units), z = Thickness, w = Stride (view units)
    vec4 u_ShadeParams;  // x = Intensity, y = RayCount, z = EdgeFade (UV), w = unused
    vec4 u_ScreenParams; // x = width, y = height, z = 1/width, w = 1/height
    vec4 u_Flags;        // x = DebugView (0/1), yzw = pad
};

const float SKY_DEPTH = 0.999999;
const int HARD_MAX_STEPS = 64;  // loop-safety cap; must match kSSGIMaxSteps
const int HARD_MAX_RAYS = 32;   // loop-safety cap; must match kSSGIMaxRays
const float PI = 3.14159265359;
const float GOLDEN_RATIO_CONJ = 0.61803398875; // fract(golden ratio) — low-discrepancy azimuth rotation

// Octahedral decode — matches octEncodeGB() in PBR_GBuffer.glsl / OctDecode() in
// PostProcess_SSR.glsl.
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

// Branchless orthonormal basis around n (Duff et al. 2017, "Building an
// Orthonormal Basis, Revisited"). Mirrored in the CPU contract test.
void BuildBasis(vec3 n, out vec3 t, out vec3 b)
{
    float s = (n.z >= 0.0) ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float d = n.x * n.y * a;
    t = vec3(1.0 + s * n.x * n.x * a, s * d, -s * n.x);
    b = vec3(d, s + n.y * n.y * a, -n.y);
}

// Interleaved gradient noise (Jimenez 2014) — cheap per-pixel hash in [0,1).
float InterleavedGradientNoise(vec2 p)
{
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

void main()
{
    vec3 baseColor = texture(u_SceneColor, v_TexCoord).rgb;

    float depth = texture(u_DepthTexture, v_TexCoord).r;
    if (depth >= SKY_DEPTH) // sky / background — receives no GI
    {
        o_Color = vec4(baseColor, 1.0);
        return;
    }

    vec4 gN = texture(u_GBufferNormal, v_TexCoord);
    vec3 albedo = texture(u_GBufferAlbedo, v_TexCoord).rgb;

    // World normal -> view space.
    vec3 Nworld = OctDecode(gN.xy);
    vec3 Nview = normalize(mat3(u_View) * Nworld);

    vec3 P = ViewPosFromDepth(v_TexCoord, depth); // view-space position (z < 0)

    float maxSteps = u_RayParams.x;
    float maxDist = u_RayParams.y;
    float thickness = u_RayParams.z;
    float stride = u_RayParams.w;
    float intensity = u_ShadeParams.x;
    int rayCount = clamp(int(u_ShadeParams.y), 1, HARD_MAX_RAYS);
    float edge = u_ShadeParams.z;

    // Depth-proportional bias along the normal so the first march step does not
    // self-intersect the originating surface.
    vec3 vStart = P + Nview * (0.02 * -P.z);

    vec3 tangent;
    vec3 bitangent;
    BuildBasis(Nview, tangent, bitangent);

    // Per-pixel azimuth rotation decorrelates the hemisphere pattern between
    // neighbouring pixels, trading banding for noise the spatial average eats.
    float ign = InterleavedGradientNoise(gl_FragCoord.xy);

    vec3 indirect = vec3(0.0);

    for (int r = 0; r < HARD_MAX_RAYS; ++r)
    {
        if (r >= rayCount)
            break;

        // Cosine-weighted hemisphere sample (Malley's method). u1 is stratified
        // across rays; u2 is a golden-ratio-rotated, per-pixel-jittered azimuth.
        float u1 = (float(r) + 0.5) / float(rayCount);
        float u2 = fract(ign + float(r) * GOLDEN_RATIO_CONJ);
        float radius = sqrt(u1);
        float phi = 2.0 * PI * u2;
        vec3 localDir = vec3(radius * cos(phi), radius * sin(phi), sqrt(max(0.0, 1.0 - u1)));
        vec3 dir = normalize(tangent * localDir.x + bitangent * localDir.y + Nview * localDir.z);

        // Linear view-space march along the ray.
        float traveled = 0.0;
        for (int s = 0; s < HARD_MAX_STEPS; ++s)
        {
            if (s >= int(maxSteps))
                break;
            traveled += stride;
            if (traveled > maxDist)
                break;

            vec3 rayPos = vStart + dir * traveled;
            vec2 uv = ProjectToUV(rayPos);
            if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
                break; // left the screen — no on-screen light to gather

            float sDepth = texture(u_DepthTexture, uv).r;
            if (sDepth >= SKY_DEPTH)
                continue; // sky behind the ray here — keep marching

            vec3 sPos = ViewPosFromDepth(uv, sDepth);
            float delta = (-rayPos.z) - (-sPos.z); // > 0 => ray is behind the surface

            if (delta > 0.0 && delta < thickness)
            {
                // The ray grazed just behind a visible surface — gather its lit
                // colour as incoming indirect radiance, faded by screen-edge and
                // distance confidence.
                float edgeFade = 1.0;
                if (edge > 0.0)
                {
                    edgeFade *= smoothstep(0.0, edge, uv.x) * smoothstep(0.0, edge, 1.0 - uv.x);
                    edgeFade *= smoothstep(0.0, edge, uv.y) * smoothstep(0.0, edge, 1.0 - uv.y);
                }
                float distFade = 1.0 - clamp(traveled / maxDist, 0.0, 1.0);
                indirect += texture(u_SceneColor, uv).rgb * edgeFade * distFade;
                break;
            }
            else if (delta >= thickness)
            {
                // Behind a thick occluder — the surface blocks the ray; stop.
                break;
            }
            // delta <= 0: ray still in front of the surface — keep marching.
        }
    }

    // Cosine-weighted estimator: irradiance mean over ALL rays (misses = 0), then
    // tint by the receiver's diffuse albedo. Diffuse reflectance already folds the
    // 1/pi normalisation, so Lo = albedo * mean(Li).
    vec3 indirectDiffuse = albedo * (indirect / float(rayCount));

    if (u_Flags.x > 0.5) // debug: show the indirect-diffuse contribution in isolation
    {
        o_Color = vec4(indirectDiffuse * intensity, 1.0);
        return;
    }

    o_Color = vec4(baseColor + indirectDiffuse * intensity, 1.0);
}
