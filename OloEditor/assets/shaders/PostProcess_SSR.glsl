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
// using hierarchical-Z traversal: a min-depth (nearest-surface) HZB pyramid lets
// the ray skip empty space in big coarse-cell steps, dropping to a linear step +
// binary-search refinement against full-res scene depth near a surface (#284). On
// a hit, sample the lit scene colour at the hit UV and composite it with a
// replace/mix blend (lerp toward the reflection by reflectance x confidence —
// not additive, which double-counts the IBL already in the base colour),
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
layout(binding = 35) uniform sampler2D u_MinHZB;        // min-depth (nearest-surface) HZB pyramid (#284)

layout(std140, binding = 38) uniform SSRParams
{
    mat4 u_Projection;
    mat4 u_InvProjection;
    mat4 u_View;
    vec4 u_RayParams;    // x = MaxSteps, y = MaxDistance, z = Thickness, w = Stride (view units)
    vec4 u_ShadeParams;  // x = Intensity, y = MaxRoughness, z = EdgeFade (UV), w = BinarySearchSteps
    vec4 u_ScreenParams; // x = width, y = height, z = 1/width, w = 1/height
    vec4 u_Flags;        // x = DebugView (0/1)
    vec4 u_HZBParams;    // xy = HZB UVFactor, z = HZB mip count, w = UseHiZ (0/1)
};

const float MIN_ROUGHNESS = 0.045;
const float SKY_DEPTH = 0.999999;
const int HARD_MAX_STEPS = 256;
const int HARD_MAX_BIN_STEPS = 32;
const int HARD_MAX_HZB_LEVELS = 16; // loop-safety cap on the HZB mip used for skipping

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

// Nearest-surface device-Z in the level-`lod` HZB cell covering `screenUV`.
// Point-sampled via texelFetch so the value is the exact conservative minimum
// of the covered block (bilinear would average, breaking the front-to-back
// skip guarantee).
float SampleMinHZB(vec2 screenUV, int lod)
{
    vec2 hzbUV = clamp(screenUV, vec2(0.0), vec2(1.0)) * u_HZBParams.xy;
    ivec2 sz = textureSize(u_MinHZB, lod);
    ivec2 c = clamp(ivec2(hzbUV * vec2(sz)), ivec2(0), sz - ivec2(1));
    return texelFetch(u_MinHZB, c, lod).r;
}

// Binary-search the surface crossing between `lo` (in front) and `hi` (behind)
// against full-res scene depth, returning the refined hit UV.
vec2 RefineCrossing(vec3 lo, vec3 hi, int steps)
{
    for (int b = 0; b < HARD_MAX_BIN_STEPS; ++b)
    {
        if (b >= steps)
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
    return ProjectToUV(hi);
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
    vec3 vStart = P + Nview * (0.02 * -P.z);

    bool hit = false;
    vec2 hitUV = vec2(0.0);
    float traveled = 0.0;

    // -----------------------------------------------------------------------
    // Screen-space hierarchical-Z traversal (#284).
    //
    // The reflection ray is projected to a screen-space segment and marched
    // ACROSS THE SCREEN, cell by cell, against the min-depth (nearest-surface)
    // HZB pyramid. Progressing in screen space (not depth) is what makes HiZ
    // robust: at each cell we compare the ray's depth at the cell's far edge to
    // the cell's nearest surface. If the ray is still in front there, the whole
    // cell is empty (nothing nearer can be hit) so we jump to the next cell and
    // climb to a coarser mip — large empty stretches cost a handful of steps.
    // Otherwise the ray reaches a surface inside the cell: descend toward mip 0,
    // where the precise crossing test + binary refine run against full-res scene
    // depth. The pyramid only skips provably-empty cells, so it never invents or
    // drops a hit — a stale/imperfect pyramid costs steps, not correctness.
    // UseHiZ off (no usable pyramid) collapses maxLevel to 0 → 1px linear march.
    int maxLevel = (u_HZBParams.w > 0.5) ? clamp(int(u_HZBParams.z) - 1, 0, HARD_MAX_HZB_LEVELS) : 0;

    // Clip the view-space segment to stay just in front of the eye (z < 0), so
    // the projected endpoints never cross the w = 0 singularity.
    float segLen = maxDist;
    if (R.z > 1.0e-6)
        segLen = min(segLen, (-1.0e-3 - vStart.z) / R.z);
    vec3 vEnd = vStart + R * segLen;

    vec4 hStart = u_Projection * vec4(vStart, 1.0);
    vec4 hEnd = u_Projection * vec4(vEnd, 1.0);

    vec2 uvStart = (hStart.xy / hStart.w) * 0.5 + 0.5;
    vec2 uvEnd = (hEnd.xy / hEnd.w) * 0.5 + 0.5;

    // March parameter t in [0,1] along the VIEW-space segment; one tInc step
    // spans about one screen pixel. Stepping the view parameter (rather than an
    // exact screen-pixel parameter) samples depth at the v1 linear march's
    // view-space granularity — that is what keeps grazing reflections matching
    // v1 rather than under-sampling depth where a surface is near-parallel to the
    // ray. The screen-pixel-sized increment keeps the HiZ cell stepping aligned
    // to the pyramid (one mip-0 texel ≈ one viewport pixel).
    float pixLen = length((uvEnd - uvStart) * u_ScreenParams.xy);
    if (hStart.w > 0.0 && hEnd.w > 0.0 && pixLen >= 1.0)
    {
        float tInc = 1.0 / pixLen;
        int level = 0;
        float t = 0.0;
        float prevT = 0.0;

        for (int i = 0; i < HARD_MAX_STEPS; ++i)
        {
            if (i >= int(maxSteps) || t >= 1.0)
                break;

            vec4 clip = mix(hStart, hEnd, t);
            vec2 uv = (clip.xy / clip.w) * 0.5 + 0.5;
            if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
                break;

            // Size the cell step in LOCAL screen pixels: perspective makes
            // pixels-per-t vary along the ray, so a global average would
            // occasionally over-skip a cell and miss a surface. Level 0 keeps the
            // global, view-uniform increment so grazing surfaces are still sampled
            // at the v1 linear march's depth granularity.
            vec4 clipProbe = mix(hStart, hEnd, min(t + tInc, 1.0));
            vec2 uvProbe = (clipProbe.xy / clipProbe.w) * 0.5 + 0.5;
            float localPixPerT = max(length((uvProbe - uv) * u_ScreenParams.xy) / tInc, 1.0e-3);
            float cellStepT = (level == 0) ? tInc : (exp2(float(level)) / localPixPerT);

            // Ray device-Z at the cell entry and at its far edge (one cell ahead).
            // Depth is monotonic along the segment, so the deeper of the two
            // bounds the ray's depth across the whole cell.
            float tExit = min(t + cellStepT, 1.0);
            vec4 clipExit = mix(hStart, hEnd, tExit);
            float rayDZ = (clip.z / clip.w) * 0.5 + 0.5;
            float rayDZExit = (clipExit.z / clipExit.w) * 0.5 + 0.5;
            float rayDZFar = max(rayDZ, rayDZExit);

            float cellMin = SampleMinHZB(uv, level);

            if (cellMin >= SKY_DEPTH || rayDZFar < cellMin)
            {
                // Empty cell: the ray stays in front of its nearest surface the
                // whole way across — jump to the next cell and climb a mip.
                prevT = t;
                t = tExit;
                level = min(level + 1, maxLevel);
            }
            else if (level > 0)
            {
                // A surface lies somewhere in this cell — descend to localise.
                level -= 1;
            }
            else
            {
                // Finest level: precise crossing test against full-res scene depth.
                float sDepth = texture(u_DepthTexture, uv).r;
                if (sDepth < SKY_DEPTH)
                {
                    vec3 rayPos = mix(vStart, vEnd, t);
                    vec3 sPos = ViewPosFromDepth(uv, sDepth);
                    float deltaL = (-rayPos.z) - (-sPos.z); // > 0 => behind surface
                    if (deltaL > 0.0 && deltaL < thickness)
                    {
                        hitUV = RefineCrossing(mix(vStart, vEnd, prevT), rayPos, binSteps);
                        hit = true;
                        traveled = segLen * t;
                        break;
                    }
                }
                // No crossing at this pixel (in front, or behind a thick
                // occluder): step one pixel and keep marching.
                prevT = t;
                t += tInc;
            }
        }
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
