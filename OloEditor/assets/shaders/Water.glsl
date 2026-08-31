// OLO_NORMAL_MAP_TBN_EXEMPT: the water surface is not a normal-mapped IMPORTED MESH. Its
// tangent frame comes from the ANALYTIC Gerstner wave derivatives (an exact dP/du, dP/dv the
// wave sum already produces), not from screen-space UV derivatives, and u_NormalMap0/1 are
// scrolling detail-wave maps blended in that frame — there is nothing for PBRCommon's
// derivative TBN to do here, and no per-submesh material to drift from. See RenderPathDrift.
// =============================================================================
// Water.glsl - Gerstner wave water surface rendering
// Vertex-displaced water plane with Fresnel reflection, SSR, tessellation
// =============================================================================

#type vertex
#version 460 core
// Stage body shared with Water_Depth.glsl — the surface-depth capture must
// displace identically to this color pass (see the include's header).
#include "include/WaterVertexStage.glsl"

// =============================================================================
// Tessellation Control Shader — adaptive tessellation for water surface
// =============================================================================
#type tess_control
#version 460 core
#include "include/WaterTessControlStage.glsl"

// =============================================================================
// Tessellation Evaluation Shader — Gerstner displacement on subdivided mesh
// =============================================================================
#type tess_evaluation
#version 460 core
#include "include/WaterTessEvalStage.glsl"

#type fragment
#version 460 core

#include "include/PBRCommon.glsl"

layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord;
layout(location = 3) in vec3 v_ViewDir;
layout(location = 4) in vec3 v_Tangent;
layout(location = 5) in vec3 v_Bitangent;
layout(location = 6) in float v_WaveHeight;
layout(location = 7) in vec3 v_PrevWorldPos;

// MRT outputs matching SceneRenderPass format
layout(location = 0) out vec4 o_Color;
layout(location = 1) out int o_EntityID;
layout(location = 2) out vec2 o_ViewNormal;
// Scene FB RT3 velocity. Captures camera, per-object motion, AND the
// per-fragment wave reprojection: v_PrevWorldPos carries the Gerstner sum
// re-evaluated at `u_NormalMapSpeed.z * u_WaveParams.y` (prev time * speed)
// in the vertex / tessellation stage, so TAA resolves moving waves cleanly.
layout(location = 3) out vec2 o_Velocity;

// Octahedral encode: unit normal -> RG16F [-1,1]^2
vec2 octEncode(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return n.xy;
}

// Camera UBO (binding 0)
layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    mat4 u_PrevViewProjection;
    vec3 u_RenderOrigin; // camera-relative render origin (issue #429)
    float _padding1;
    // Reconstruction flavour of u_Projection (#691) — for the SSR
    // marcher's unproject and the near/far row extraction below, which apply
    // GL-convention depth math themselves. Identical to u_Projection on GL.
    mat4 u_ProjectionForReconstruction;
};

// Instance SSBO (binding 15). Water is single-instance — the tess_eval
// stage uses InstanceBlock_Single (no v_InstanceIndex output), so the
// fragment stage must match it rather than declaring `flat in int
// v_InstanceIndex` with no producer (link error).
#include "include/InstanceBlock_Single.glsl"

// Water UBO (binding 23)
layout(std140, binding = 23) uniform WaterParams
{
    vec4 u_WaveParams;
    vec4 u_WaveDir0;
    vec4 u_WaveDir1;
    vec4 u_WaterColor;             // rgb = shallow, a = transparency
    vec4 u_WaterDeepColor;         // rgb = deep,    a = reflectivity
    vec4 u_VisualParams;           // x = FresnelPower, y = SpecularIntensity, z = NormalMapTiling, w = NoiseIntensity
    vec4 u_NormalMapScroll;        // xy = scroll0 offset, zw = scroll1 offset
    vec4 u_NormalMapSpeed;         // x = speed0, y = speed1, z = PrevTime, w = renderFromBelow
    vec4 u_LightDirection;         // xyz = directional light dir, w = unused
    vec4 u_ScreenParams;           // x = width, y = height, z = 1/width, w = 1/height
    vec4 u_DepthRefractionParams;  // x = depthSoftening, y = refrDistortion, z = refrHeightFactor, w = unused
    vec4 u_RefractionColor;        // rgb = tint color, w = unused
    vec4 u_FoamParams;             // x = heightStart, y = fadeDistance, z = tiling, w = brightness
    vec4 u_FoamParams2;            // x = angleExponent, y = shorelinePower, z = sssIntensity, w = unused
    vec4 u_SSSColor;               // rgb = subsurface color, w = foamCoverage (#943)
    vec4 u_SSRParams;              // x = maxSteps (0=disabled), y = stepSize, z = maxDistance, w = thickness
    vec4 u_TessParams;             // x = tessellationFactor, y = minTessDist, z = maxTessDist, w = frustumCullEnable
    vec4 u_FFTParams;              // x = useFFT (0/1), y = 1/patchSize, z = heightScale, w = horizontalScale
    // Boat / actor wake foam field (issue #967). C++ twin:
    // UBOStructures::WaterUBO::WakeFieldParams / WakeFieldParams2. Declared in
    // EVERY stage of the water programs, identically, because GL requires a
    // uniform block shared across a program's stages to be declared the same
    // way in each — appending to only the stage that reads it is a link error,
    // not a silent mismatch. Only the fragment stage actually reads them.
    vec4 u_WakeFieldParams;        // xy = field window centre (world XZ), z = 1/fieldExtent, w = intensity (<=0 disables)
    vec4 u_WakeFieldParams2;       // x = wake fade start (m), y = wake fade end (m), z = edge-fade start, w = unused

    // Boat / actor wake SHAPE (issue #968). C++ twin:
    // UBOStructures::WaterUBO::WakeShapeParams / WakeHulls; GLSL evaluator:
    // include/WaterWakeCommon.glsl. Declared in EVERY stage of the water
    // programs, identically, for the same reason the #967 fields above are: GL
    // requires a uniform block shared across a program's stages to be declared
    // the same way in each, so appending to only the stages that read it is a
    // LINK error rather than a silent mismatch. Read by the vertex and
    // tess-eval stages, which is where the surface is displaced.
    vec4 u_WakeShapeParams;        // x = live hull count, y = height scale (<=0 disables), z = hull flatten strength, w = reserved
    // 80 = WaterWake::kHullVec4Count (4 hulls x 20 vec4). The layout is
    // WaterWake.h's, verbatim; WATER_WAKE_* in WaterWakeCommon.glsl mirrors the
    // offsets so nothing here indexes it by a bare literal.
    vec4 u_WakeHulls[80];
};

// Environment map for reflection (same slot as PBR shaders)
#include "include/BindlessHeap.glsl"
#ifdef OLO_BINDLESS
#define u_EnvironmentMap OLO_HEAP_TEX_CUBE(9)  // TEX_ENVIRONMENT
#else
layout(binding = 9) uniform samplerCube u_EnvironmentMap;
#endif

// FFT ocean displacement (binding 50): a-channel carries the Jacobian-based foam.
#ifdef OLO_BINDLESS
#define u_FFTDisplacement OLO_HEAP_TEX_2D(50)  // TEX_WATER_FFT_DISPLACEMENT
#else
layout(binding = 50) uniform sampler2D u_FFTDisplacement;
#endif

// Scrolling normal maps and noise texture
#ifdef OLO_BINDLESS
#define u_NormalMap0 OLO_HEAP_TEX_2D(36)  // TEX_WATER_NORMAL_0
#define u_NormalMap1 OLO_HEAP_TEX_2D(37)  // TEX_WATER_NORMAL_1
#define u_NoiseMap OLO_HEAP_TEX_2D(38)  // TEX_WATER_NOISE
#else
layout(binding = 36) uniform sampler2D u_NormalMap0;
layout(binding = 37) uniform sampler2D u_NormalMap1;
layout(binding = 38) uniform sampler2D u_NoiseMap;
#endif

// Depth and refraction textures (bound by WaterRenderPass)
#ifdef OLO_BINDLESS
#define u_SceneDepth OLO_HEAP_TEX_2D(39)  // TEX_WATER_DEPTH
#define u_RefractionTexture OLO_HEAP_TEX_2D(40)  // TEX_WATER_REFRACTION
#define u_FoamTexture OLO_HEAP_TEX_2D(41)  // TEX_WATER_FOAM
#else
layout(binding = 39) uniform sampler2D u_SceneDepth;
layout(binding = 40) uniform sampler2D u_RefractionTexture;
layout(binding = 41) uniform sampler2D u_FoamTexture;
#endif

// Scene view-space normals for SSR (bound by WaterRenderPass)
#ifdef OLO_BINDLESS
#define u_SceneNormals OLO_HEAP_TEX_2D(22)  // TEX_SCENE_NORMALS
#else
layout(binding = 22) uniform sampler2D u_SceneNormals;
#endif

// Boat / actor wake foam field (issue #967) — the world-anchored, toroidally
// stored disturbance field written by compute/WaterDisturbance_Update.comp and
// published by WaterDisturbanceSystem::BindFieldTexture. RG16F; only .r is read.
#ifdef OLO_BINDLESS
#define u_WaterDisturbance OLO_HEAP_TEX_2D(70)  // TEX_WATER_DISTURBANCE
#else
layout(binding = 70) uniform sampler2D u_WaterDisturbance;
#endif
#include "include/WaterDisturbanceCommon.glsl"

// Planar reflection — the opaque scene re-rendered from a mirrored, oblique-
// clipped camera by PlanarReflectionRenderPass. u_PlanarReflectionVP projects a
// world position into that target; Params.x gates the whole feature so a stale
// texture is never sampled when the reflection pass is disabled.
layout(std140, binding = 43) uniform PlanarReflectionParams
{
    mat4 u_PlanarReflectionVP;   // world -> mirrored reflection clip space
    vec4 u_PlanarReflectionData; // x = enabled (0/1), y = intensity, z = distortion, w = unused
};
#ifdef OLO_BINDLESS
#define u_PlanarReflectionTexture OLO_HEAP_TEX_2D(52)  // TEX_WATER_PLANAR_REFLECTION
#else
layout(binding = 52) uniform sampler2D u_PlanarReflectionTexture;
#endif

// Sample the planar reflection at this surface point, perturbing the projected
// UV by the surface normal so ripples break up the mirror. Returns rgb in .rgb
// and a [0,1] confidence in .a (0 when disabled or the sample falls off-screen).
vec4 samplePlanarReflection(vec3 worldPos, vec3 normal)
{
    if (u_PlanarReflectionData.x < 0.5)
        return vec4(0.0);

    vec4 clip = u_PlanarReflectionVP * vec4(worldPos, 1.0);
    if (clip.w <= 0.0)
        return vec4(0.0);

    vec2 reflUV = (clip.xy / clip.w) * 0.5 + 0.5;
    // The reflection target is rendered with a mirrored camera, so its X is
    // already flipped relative to the main view — no extra flip needed.
    reflUV += normal.xz * u_PlanarReflectionData.z;

    // Fade out toward the screen edge so the mirror doesn't smear where the
    // reflected geometry runs off the reflection viewport.
    vec2 edge = smoothstep(vec2(0.0), vec2(0.06), reflUV) *
                (1.0 - smoothstep(vec2(0.94), vec2(1.0), reflUV));
    float confidence = edge.x * edge.y;
    if (confidence <= 0.0)
        return vec4(0.0);

    vec3 reflColor = texture(u_PlanarReflectionTexture, clamp(reflUV, vec2(0.001), vec2(0.999))).rgb;
    return vec4(reflColor, confidence * clamp(u_PlanarReflectionData.y, 0.0, 1.0));
}

// Linearize a depth buffer value to view-space depth
// Normalize with a zero/NaN-safe fallback. normalize(v) of a zero-length or
// non-finite vector yields NaN; a single NaN water pixel escaping this shader
// snowballs through the bloom pyramid's 13-tap downsample/upsample chain into
// a ~300 px black block on screen (scene + NaN = NaN) and poisons the
// auto-exposure histogram. `len2 > eps` is false for NaN too, so one predicate
// covers both degenerate cases.
vec3 safeNormalize(vec3 v, vec3 fallback)
{
    float len2 = dot(v, v);
    return (len2 > 1e-12) ? v * inversesqrt(len2) : fallback;
}

float linearizeDepth(float d, float nearPlane, float farPlane)
{
    return nearPlane * farPlane / (farPlane - d * (farPlane - nearPlane));
}

// Procedural hash-based noise for water detail (no texture needed)
float hash2D(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// Smooth 2D value noise
float valueNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f); // smoothstep interpolation

    float a = hash2D(i);
    float b = hash2D(i + vec2(1.0, 0.0));
    float c = hash2D(i + vec2(0.0, 1.0));
    float d = hash2D(i + vec2(1.0, 1.0));

    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Procedural tangent-space normal from value noise (central differences).
// Tangent-space normal maps use +Z as the surface normal; putting the unit
// component in Y turns a missing-map fallback sideways through the TBN and
// makes it dominate the ocean lighting as a blocky, near-horizontal normal.
vec3 proceduralNormal(vec2 uv, float strength)
{
    float eps = 0.02;
    float h0 = valueNoise(uv);
    float hx = valueNoise(uv + vec2(eps, 0.0));
    float hy = valueNoise(uv + vec2(0.0, eps));
    return normalize(vec3(-(hx - h0) * strength, -(hy - h0) * strength, 1.0));
}

// smoothstep() whose transition is widened by the value's own screen-space
// footprint — analytic anti-aliasing for a threshold on a procedural field
// (issue #943).
//
// Every noise field in this shader is evaluated per pixel from world position,
// at frequencies (sparkle at 3/7/13 cycles per metre, the foam pattern at 3 and
// 7.4) that fall far below one sample per pixel long before the horizon. Put a
// narrow smoothstep across an undersampled field and neighbouring pixels land on
// opposite sides of the threshold essentially at random, which is what turns the
// distant sea into hard-edged flats and blobs.
//
// fwidth(x) is how much the field moves between adjacent pixels, so padding the
// edges by it makes the transition exactly as wide as the pixel can resolve:
// unchanged where the field is well sampled (fwidth ~ 0), and flattening toward
// the field's local average as it becomes subpixel — which is the correct filtered
// answer rather than a coin flip.
float smoothstepAA(float edge0, float edge1, float x)
{
    float w = fwidth(x);
    return smoothstep(edge0 - w, edge1 + w, x);
}

// FBM noise for foam and detail (3 octaves)
float fbmNoise(vec2 p)
{
    float v = 0.0;
    float a = 0.5;
    mat2 rot = mat2(0.8, 0.6, -0.6, 0.8);
    for (int i = 0; i < 3; i++)
    {
        v += a * valueNoise(p);
        p = rot * p * 2.0;
        a *= 0.5;
    }
    return v;
}

// =============================================================================
// Screen Space Reflections (SSR)
// Ray-marches in view space, samples scene color at hit point
// =============================================================================

// Reconstruct view-space position from screen UV and depth
vec3 viewPosFromDepth(vec2 uv, float depth)
{
    // Reconstruction flavour (#691): the depth*2-1 below is the GL
    // unmap, so the matrix must carry GL-convention z rows — the rasterizer
    // flavour would double-apply the remap on Vulkan.
    // NDC: [-1, 1]
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    // Inverse projection to view space
    mat4 invProj = inverse(u_ProjectionForReconstruction);
    vec4 viewPos = invProj * ndc;
    return viewPos.xyz / viewPos.w;
}

// Project view-space position to screen UV
vec2 projectToScreen(vec3 viewPos)
{
    // xy-only use, but paired with viewPosFromDepth above — keep the same
    // flavour (the two agree on rows 0/1 in both flavours).
    vec4 clipPos = u_ProjectionForReconstruction * vec4(viewPos, 1.0);
    vec2 ndc = clipPos.xy / clipPos.w;
    return ndc * 0.5 + 0.5;
}

// SSR ray march: returns vec4(color.rgb, confidence)
vec4 screenSpaceReflection(vec3 worldPos, vec3 reflectDirWorld)
{
    float maxSteps = u_SSRParams.x;
    float stepSize = u_SSRParams.y;
    float maxDistance = u_SSRParams.z;
    float thickness = u_SSRParams.w;

    // Convert to view space
    vec3 viewPos = (u_View * vec4(worldPos, 1.0)).xyz;
    vec3 viewReflect = normalize(mat3(u_View) * reflectDirWorld);

    // March through screen space
    vec3 rayPos = viewPos;
    float totalDistance = 0.0;
    int steps = int(maxSteps);

    for (int i = 0; i < steps; i++)
    {
        rayPos += viewReflect * stepSize;
        totalDistance += stepSize;

        if (totalDistance > maxDistance)
        {
            break;
        }

        // Project ray position to screen
        vec2 hitUV = projectToScreen(rayPos);

        // Off-screen check
        if (hitUV.x < 0.0 || hitUV.x > 1.0 || hitUV.y < 0.0 || hitUV.y > 1.0)
        {
            break;
        }

        // Sample scene depth at this screen position
        float sceneDepthSample = texture(u_SceneDepth, hitUV).r;
        vec3 sceneViewPos = viewPosFromDepth(hitUV, sceneDepthSample);

        // Check if ray is behind scene geometry
        float depthDelta = rayPos.z - sceneViewPos.z;

        if (depthDelta > 0.0 && depthDelta < thickness)
        {
            // Hit! Sample scene color
            vec3 hitColor = texture(u_RefractionTexture, hitUV).rgb;

            // Compute confidence: fade at screen edges, fade with distance
            float edgeFade = 1.0;
            edgeFade *= smoothstep(0.0, 0.05, hitUV.x) * smoothstep(1.0, 0.95, hitUV.x);
            edgeFade *= smoothstep(0.0, 0.05, hitUV.y) * smoothstep(1.0, 0.95, hitUV.y);

            float distFade = 1.0 - clamp(totalDistance / maxDistance, 0.0, 1.0);
            float stepFade = 1.0 - clamp(float(i) / float(steps), 0.0, 1.0);

            float confidence = edgeFade * distFade * stepFade;
            return vec4(hitColor, confidence);
        }

        // Adaptive step size: increase as we go further
        stepSize *= 1.02;
    }

    return vec4(0.0); // No hit: zero confidence
}

void main()
{
    // Per-fragment waterline side selection (§7.2). When render-from-below is
    // enabled (u_NormalMapSpeed.w > 0.5) the water draws double-sided; keep only
    // the face whose side the camera is actually on relative to THIS fragment:
    // a fragment above the eye shows its underside, one below shows its top.
    // This avoids both the see-through holes of single-sided culling and the
    // interleaved-sheet mess of naive double-siding when the camera straddles
    // the waterline. (With render-from-below off the draw is back-culled and
    // this branch is inert.)
    if (u_NormalMapSpeed.w > 0.5)
    {
        bool cameraBelowFragment = v_WorldPos.y > u_CameraPosition.y;
        if (cameraBelowFragment == gl_FrontFacing)
            discard;
    }

    // Interpolated vertex vectors can cross zero length at isolated pixels
    // (opposing per-vertex directions inside one triangle) — plain
    // normalize() emits the single-pixel NaNs that bloom then amplifies.
    vec3 gerstnerNormal = safeNormalize(v_Normal, vec3(0.0, 1.0, 0.0));
    vec3 viewDir = safeNormalize(v_ViewDir, vec3(0.0, 1.0, 0.0));

    // Underside shading: kept back faces (camera below this fragment) face away
    // from the viewer, so flip the shading normal to face the camera before the
    // Fresnel / reflection / cheap-underside path below.
    if (!gl_FrontFacing)
    {
        gerstnerNormal = -gerstnerNormal;
    }

    // --- Normal Detail ---
    float tiling = u_VisualParams.z; // NormalMapTiling
    float time = u_WaveParams.x * u_WaveParams.y;

    // Build TBN matrix from vertex outputs
    vec3 T = safeNormalize(v_Tangent, vec3(1.0, 0.0, 0.0));
    vec3 B = safeNormalize(v_Bitangent, vec3(0.0, 0.0, 1.0));
    vec3 N = gerstnerNormal;
    mat3 TBN = mat3(T, B, N);

    // Check if normal map textures are actually bound (non-black check)
    // When unbound, OpenGL returns (0,0,0,0) → (0*2-1) = (-1,-1,-1) = BAD
    vec2 uv0 = (v_WorldPos.xz + u_RenderOrigin.xz) * tiling + u_NormalMapScroll.xy;
    vec2 uv1 = (v_WorldPos.xz + u_RenderOrigin.xz) * tiling * 0.7 + u_NormalMapScroll.zw;

    vec4 nm0Sample = texture(u_NormalMap0, uv0);
    vec4 nm1Sample = texture(u_NormalMap1, uv1);

    // Detect unbound textures: if ALL channels are exactly 0, use procedural fallback
    bool hasNormalMap0 = (nm0Sample.r + nm0Sample.g + nm0Sample.b) > 0.001;
    bool hasNormalMap1 = (nm1Sample.r + nm1Sample.g + nm1Sample.b) > 0.001;

    vec3 n0, n1;
    if (hasNormalMap0)
    {
        n0 = nm0Sample.rgb * 2.0 - 1.0;
    }
    else
    {
        // Procedural scrolling normal detail (2 octaves at different scales)
        n0 = proceduralNormal(uv0 * 8.0, 1.5);
    }

    if (hasNormalMap1)
    {
        n1 = nm1Sample.rgb * 2.0 - 1.0;
    }
    else
    {
        n1 = proceduralNormal(uv1 * 12.0, 1.2);
    }

    // Blend the two normal maps in tangent space. Two opposing samples sum to
    // ~zero at isolated pixels — the safeNormalize fallbacks keep those from
    // emitting NaNs instead of a flat normal.
    vec3 blendedTangentNormal = safeNormalize(n0 + n1, vec3(0.0, 0.0, 1.0));
    vec3 normalMapWorld = safeNormalize(TBN * blendedTangentNormal, gerstnerNormal);
    // Blend strength: stronger at close range for micro-detail — and since #943
    // that is what the code does, not just what this comment said.
    //
    // The detail normal is sampled at u_VisualParams.z (tiling) * 8 and * 12
    // cycles per METRE. Past a few tens of metres that is hundreds of cycles per
    // pixel: it does not average out, it MOIRES, into large smooth lobes that
    // tilted the shading normal 30-60 degrees off vertical on a sea whose real
    // slope is a couple of degrees. Blended in at a flat 0.6 those lobes swing
    // the reflection across the sky gradient and read as the hard-edged khaki
    // patches this issue is about — the surface was being shaded by aliasing.
    //
    // So fade the detail out as it stops being resolvable. fwidth() on the UV is
    // how much of the pattern one pixel spans; one cycle of the coarser octave
    // is 1/8 of a UV unit, so beyond ~0.12 there is less than a cycle per pixel
    // and the detail is noise. Near water keeps the full 0.6 and looks unchanged;
    // distance falls back to the Gerstner normal, which is the honest answer for
    // a surface whose detail the frame cannot resolve.
    float detailFootprint = max(length(fwidth(uv0)), length(fwidth(uv1)));
    float detailBlend = 0.6 * (1.0 - smoothstep(0.03, 0.12, detailFootprint));
    vec3 normal = safeNormalize(mix(gerstnerNormal, normalMapWorld, detailBlend), gerstnerNormal);

    // --- Underside (camera submerged) ---
    // Cheap, stable shading for the surface seen from below. Screen-space
    // reflection and refraction are sampled from an above-water frame of
    // reference and produce flickering garbage from underneath, so the
    // underside is just a tinted surface with a soft cubemap rim. The whole-
    // scene underwater tint is applied separately in the tone-map pass. §7.2.
    if (!gl_FrontFacing)
    {
        float NdotVu = max(dot(normal, viewDir), 0.0);
        vec3 underColor = mix(u_WaterColor.rgb, u_WaterDeepColor.rgb, 0.5);
        vec3 cubemapU = texture(u_EnvironmentMap, reflect(-viewDir, normal)).rgb;
        // Subtle rim toward the cubemap at grazing angles so it isn't flat.
        // (1-NdotVu)^4 as a multiply chain — avoids pow()'s exp2/log2 per pixel.
        float rimBase = 1.0 - NdotVu;
        float rimBase2 = rimBase * rimBase;
        float rim = rimBase2 * rimBase2;
        vec3 underFinal = mix(underColor, cubemapU, rim * 0.25);

        o_Color = vec4(underFinal, 1.0);
        o_EntityID = u_EntityID;
        o_ViewNormal = octEncode(normalize(mat3(u_View) * normal));
        vec4 clipCurrU = u_ViewProjection     * vec4(v_WorldPos,     1.0);
        vec4 clipPrevU = u_PrevViewProjection * vec4(v_PrevWorldPos, 1.0);
        o_Velocity = (clipCurrU.xy / clipCurrU.w - clipPrevU.xy / clipPrevU.w) * 0.5;
        return;
    }

    // --- Screen-space UV ---
    vec2 screenUV = gl_FragCoord.xy * u_ScreenParams.zw;

    // --- Depth Softening ---
    // GL-convention row extraction — needs the reconstruction flavour (#691)
    // The rasterizer flavour's remapped z rows break this formula.
    float nearPlane = u_ProjectionForReconstruction[3][2] / (u_ProjectionForReconstruction[2][2] - 1.0);
    float farPlane  = u_ProjectionForReconstruction[3][2] / (u_ProjectionForReconstruction[2][2] + 1.0);

    float sceneDepthRaw = texture(u_SceneDepth, screenUV).r;
    float sceneDepthLinear = linearizeDepth(sceneDepthRaw, nearPlane, farPlane);
    float waterDepthLinear = linearizeDepth(gl_FragCoord.z, nearPlane, farPlane);
    float depthDifference = max(sceneDepthLinear - waterDepthLinear, 0.0);

    float depthSoftening = u_DepthRefractionParams.x;
    float depthFade = smoothstep(0.0, depthSoftening, depthDifference);

    // View-INDEPENDENT water-column depth: reconstruct the seafloor's world
    // height under this fragment. The screen-space depthDifference collapses at
    // grazing angles (making the surface look transparent / foam flicker), so
    // both opacity and shoreline foam use this vertical depth instead. Computed
    // once here and reused below.
    //
    // `hasFloorBehind` is false when there's no opaque geometry behind the
    // surface (open ocean / sky at the far plane). There's no bottom to see in
    // that case, so the water must read as fully opaque (and grow no shoreline
    // foam) — otherwise the far-plane reconstruction yields a near-zero depth
    // and the open ocean turns transparent. This is the deep-water default.
    bool hasFloorBehind = sceneDepthRaw < 0.9999;
    vec3 floorViewPos = viewPosFromDepth(screenUV, sceneDepthRaw);
    float floorWorldY = (inverse(u_View) * vec4(floorViewPos, 1.0)).y;
    float verticalWaterDepth = max(v_WorldPos.y - floorWorldY, 0.0);

    // --- Refraction ---
    float refrDistortion = u_DepthRefractionParams.y;
    float refrHeightFactor = u_DepthRefractionParams.z;

    vec2 refractionOffset = normal.xz * refrDistortion * (1.0 + v_WaveHeight * refrHeightFactor);
    vec2 refractionUV = clamp(screenUV + refractionOffset, vec2(0.001), vec2(0.999));
    vec3 refractionSample = texture(u_RefractionTexture, refractionUV).rgb;

    vec3 refrTint = u_RefractionColor.rgb;
    float refrDepthTint = exp(-depthDifference * 0.5);
    vec3 refractedColor = refractionSample * mix(vec3(1.0), refrTint, 1.0 - refrDepthTint);

    // --- Fresnel ---
    float NdotV = max(dot(normal, viewDir), 0.0);
    float fresnelPower = u_VisualParams.x;
    float reflectivity = u_WaterDeepColor.a;
    // Schlick-style Fresnel with artist power control
    float fresnelFactor = reflectivity + (1.0 - reflectivity) * pow(1.0 - NdotV, fresnelPower);

    // Reflection: SSR with cubemap fallback
    vec3 reflectDir = reflect(-viewDir, normal);
    // textureGrad, not texture (#943). Reflectivity is used as Fresnel F0 and is
    // authored near 1 on every sea, so this cubemap fetch IS most of what the
    // surface shows. On a rippled sea at a grazing angle `reflectDir` sweeps a
    // wide solid angle inside a single pixel, so a point sample lands on one
    // side or the other of the sky's vertical gradient per pixel and the result
    // is the hard-edged flats of #943 / the "plateaus" of #898 — an aliased
    // reflection, not an aliased foam edge.
    //
    // Handing the hardware the actual screen-space derivatives of the reflected
    // direction lets it pick a mip matching that footprint: sharp where the
    // normal field is well sampled (near field, calm water), progressively
    // filtered as the footprint widens. That is only possible because the sky
    // bakes now build a mip chain (SkyCubemapBake.cpp); against a single-level
    // cubemap this degrades to exactly the old point sample rather than
    // breaking, so a sky without mips is still correct, just unfiltered.
    vec3 cubemapReflection =
        textureGrad(u_EnvironmentMap, reflectDir, dFdx(reflectDir), dFdy(reflectDir)).rgb;
    vec3 reflectionColor = cubemapReflection;

    // Screen Space Reflections (when enabled: u_SSRParams.x > 0)
    if (u_SSRParams.x > 0.0)
    {
        vec4 ssrResult = screenSpaceReflection(v_WorldPos, reflectDir);
        // Blend SSR with cubemap fallback based on confidence
        reflectionColor = mix(cubemapReflection, ssrResult.rgb, ssrResult.a);
    }

    // Planar (mirror) reflection takes priority where available — it is a true
    // re-render of the scene, not a screen-space / cubemap approximation. Blend
    // it over the cubemap/SSR result by its confidence (edge fade) and intensity.
    vec4 planar = samplePlanarReflection(v_WorldPos, normal);
    reflectionColor = mix(reflectionColor, planar.rgb, planar.a);

    // Blend shallow and deep water colors based on view angle + depth
    vec3 shallowColor = u_WaterColor.rgb;
    vec3 deepColor = u_WaterDeepColor.rgb;
    float depthColorBlend = 1.0 - exp(-depthDifference * 0.3);
    float viewDepthBlend = max(depthColorBlend, 1.0 - NdotV); // deep color at grazing angles too
    vec3 waterBaseColor = mix(shallowColor, deepColor, viewDepthBlend);

    // Refraction opacity from the VIEW-INDEPENDENT water-column depth, so the
    // surface reads as opaque from every angle (it never goes see-through at
    // grazing). Only genuinely shallow water (a metre or so — true shorelines,
    // against pillars) lets the bottom show; anything deeper is fully opaque
    // water body colour. exp(-d*2): ~0.86 opaque at 1 m, ~0.98 at 2 m, ~1 beyond.
    // Open ocean (no floor behind the surface) is always fully opaque.
    float waterBlend = hasFloorBehind ? (1.0 - exp(-verticalWaterDepth * 2.0)) : 1.0;
    vec3 waterColor = mix(refractedColor, waterBaseColor, waterBlend);

    // Combine reflection with water color via Fresnel
    vec3 finalColor = mix(waterColor, reflectionColor, fresnelFactor);

    // --- Ambient ocean contribution ---
    // Provides a minimum brightness so the water is never pitch black,
    // even without strong reflections or cubemap.  Simulates sky light
    // scattering through the upper water column.
    vec3 ambientOcean = shallowColor * 0.15;
    finalColor += ambientOcean;

    // --- Specular with noise modulation ---
    vec3 lightDir = safeNormalize(u_LightDirection.xyz, normalize(vec3(0.5, 1.0, 0.3)));
    // viewDir == -lightDir at grazing opposition sums to zero — fall back to
    // the surface normal (spec term is degenerate there anyway).
    vec3 halfVec = safeNormalize(viewDir + lightDir, normal);
    float specAngle = max(dot(normal, halfVec), 0.0);

    // Two specular lobes: tight sun disk + broader sparkle
    float noiseIntensity = u_VisualParams.w;
    float sparkleNoise = 1.0;
    if (noiseIntensity > 0.0)
    {
        // Use procedural noise if no noise texture is bound
        vec4 noiseSample = texture(u_NoiseMap, (v_WorldPos.xz + u_RenderOrigin.xz) * 0.5 + u_NormalMapScroll.xy * 0.3);
        bool hasNoiseMap = (noiseSample.r + noiseSample.g + noiseSample.b) > 0.001;
        // Procedural sparkle noise at 3 different frequencies. Evaluated
        // UNCONDITIONALLY and selected afterwards, rather than inside the
        // `hasNoiseMap` else-branch where it used to sit: `hasNoiseMap` comes
        // from a texture sample, so it is not quad-uniform, and smoothstepAA's
        // fwidth() is a derivative — undefined in non-uniform control flow. The
        // cost is a few noise evaluations on the rarely-taken texture path;
        // `noiseIntensity > 0.0` above is a uniform so that branch is fine.
        float n1p = valueNoise((v_WorldPos.xz + u_RenderOrigin.xz) * 3.0 + u_NormalMapScroll.xy * 5.0);
        float n2p = valueNoise((v_WorldPos.xz + u_RenderOrigin.xz) * 7.0 - u_NormalMapScroll.zw * 3.0);
        float n3p = valueNoise((v_WorldPos.xz + u_RenderOrigin.xz) * 13.0 + time * 0.5);
        // smoothstepAA: at 3/7/13 cycles per metre this product is deep below
        // Nyquist by mid-distance, and a hard threshold on it was the brightest
        // of the three flat-patch sources (#943).
        float proceduralSparkle = smoothstepAA(0.02, 0.15, n1p * n2p * n3p); // Threshold for sparkle dots

        float noiseVal = hasNoiseMap ? noiseSample.r : proceduralSparkle;
        sparkleNoise = mix(1.0, noiseVal, noiseIntensity);
    }

    float specIntensity = u_VisualParams.y;
    // specAngle^16 and ^256 as squaring chains (specAngle in [0,1]) — avoids two
    // pow() exp2/log2 pairs per water pixel; ^256 reuses the ^16 partial.
    float sa2 = specAngle * specAngle;
    float sa4 = sa2 * sa2;
    float sa8 = sa4 * sa4;
    float sa16 = sa8 * sa8;     // specAngle^16
    float sa256 = sa16 * sa16;  // ^32
    sa256 = sa256 * sa256;      // ^64
    sa256 = sa256 * sa256;      // ^128
    sa256 = sa256 * sa256;      // ^256
    // Tight sun specular (always present)
    float sunSpec = sa256 * specIntensity * 2.0;
    // Broader sparkle (noise-modulated) — wide lobe gives wet glint across the surface
    float sparkleSpec = sa16 * specIntensity * 0.35 * sparkleNoise;
    finalColor += vec3(sunSpec + sparkleSpec);

    // --- Subsurface Scattering (SSS) ---
    float sssIntensity = u_FoamParams2.z;
    if (sssIntensity > 0.0)
    {
        float sssFactor = pow(clamp(dot(viewDir, -lightDir), 0.0, 1.0), 4.0);
        sssFactor *= clamp(v_WaveHeight * 0.5 + 0.5, 0.0, 1.0);
        // SSS only on thin crests facing the light
        float sssThickness = clamp(1.0 - abs(dot(gerstnerNormal, vec3(0.0, 1.0, 0.0))), 0.0, 1.0);
        sssFactor *= (0.5 + 0.5 * sssThickness);
        finalColor += u_SSSColor.rgb * sssFactor * sssIntensity;
    }

    // --- Foam ---
    float foamHeightStart = u_FoamParams.x;
    float foamFadeDistance = u_FoamParams.y;
    float foamTiling = u_FoamParams.z;
    float foamBrightness = u_FoamParams.w;
    float foamAngleExponent = u_FoamParams2.x;
    float shorelineFoamPower = u_FoamParams2.y;

    // Height-based foam (wave crests)
    float heightFoam = smoothstep(foamHeightStart, foamHeightStart + foamFadeDistance, v_WaveHeight);

    // Angle-based foam (steep wave faces)
    float steepness = 1.0 - max(dot(gerstnerNormal, vec3(0.0, 1.0, 0.0)), 0.0);
    float angleFoam = pow(steepness, foamAngleExponent);

    // Shoreline foam — uses the same view-INDEPENDENT water-column depth as the
    // opacity above (computed once in the depth section), so foam only appears
    // where the water is genuinely shallow (true shorelines / against pillars)
    // and never flickers with camera angle over open water. See §7.2.
    const float shorelineDepthRange = 1.5; // metres of water depth over which shore foam fades out
    float shorelineFoam = hasFloorBehind
        ? pow(1.0 - clamp(verticalWaterDepth / shorelineDepthRange, 0.0, 1.0), shorelineFoamPower)
        : 0.0; // open ocean (no floor behind) → no shoreline foam

    // --- Large-scale spatial noise gate ---
    // Prevents foam from appearing on EVERY wave crest.
    // Very low-frequency noise so only sparse, random patches of
    // crests get foam — eliminates the grid/checkerboard pattern.
    float foamGateNoise = fbmNoise((v_WorldPos.xz + u_RenderOrigin.xz) * 0.03 + vec2(5.3, 11.7));
    // The gate's upper edge is driven by FoamCoverage (u_SSSColor.w) instead of
    // being hardcoded (#943). Coverage is the fraction of the noise range that
    // opens the gate: the 0.12 default gives exactly the old (0.62, 0.88) edges,
    // so existing scenes are unchanged, and raising it lets a storm actually
    // break. The 0.26 ramp width is kept so the clustering stays soft rather
    // than turning into a hard mask as coverage rises.
    float foamGateHi = 1.0 - clamp(u_SSSColor.w, 0.0, 1.0);
    float foamGate = smoothstepAA(foamGateHi - 0.26, foamGateHi, foamGateNoise);
    heightFoam *= foamGate;
    angleFoam  *= foamGate;

    float foam = max(max(heightFoam, angleFoam), shorelineFoam);

    // FFT ocean: Jacobian-based foam where the choppy surface folds (§2.1). The
    // displacement texture's alpha is saturate(1 - J): 0 on smooth water, →1 in
    // the pinched, breaking crests. This is far more physically plausible than
    // the height/angle thresholds above, so fold it in as a strong contributor.
    if (u_FFTParams.x > 0.5)
    {
        float fftFoam = textureLod(u_FFTDisplacement, (v_WorldPos.xz + u_RenderOrigin.xz) * u_FFTParams.y, 0.0).a;
        foam = max(foam, fftFoam);
    }

    // Distance fade: at grazing angles the foam patches compress toward the
    // horizon into a continuous white wash that dominates the frame. Fade foam
    // out fairly aggressively with distance so only nearby waves (where foam
    // detail is actually resolvable) keep it; far water reads as smooth tinted
    // ocean. See §7.2.
    float foamCamDist = length(u_CameraPosition - v_WorldPos);
    foam *= 1.0 - smoothstep(12.0, 45.0, foamCamDist);

    // Modulate foam with texture OR procedural noise
    vec2 foamUV = (v_WorldPos.xz + u_RenderOrigin.xz) * foamTiling + u_NormalMapScroll.xy * 0.2;
    vec4 foamSample = texture(u_FoamTexture, foamUV);
    bool hasFoamTexture = (foamSample.r + foamSample.g + foamSample.b) > 0.001;

    // Procedural foam pattern: multi-octave smooth noise, two scales blended to
    // break up regularity. Evaluated UNCONDITIONALLY and selected below for the
    // same reason as the sparkle noise above — `hasFoamTexture` is derived from
    // a texture sample and so is not quad-uniform, and smoothstepAA() takes a
    // derivative, which is undefined in non-uniform control flow.
    float foamNoise1 = fbmNoise(foamUV * 1.5);
    float foamNoise2 = fbmNoise(foamUV * 3.7 + vec2(17.3, 31.7));
    float foamNoise = foamNoise1 * 0.6 + foamNoise2 * 0.4;
    // Smooth, wide ramp instead of hard threshold — avoids speckle. Widened by
    // its own footprint: foamUV is per-metre, so this noise is undersampled well
    // before the foam distance fade ends (#943).
    float foamPattern = smoothstepAA(0.25, 0.65, foamNoise);

    foam *= hasFoamTexture ? foamSample.r : foamPattern;

    // --- Boat / actor wake foam (issue #967) --------------------------------
    // Sampled ON TOP of everything above, never instead of it: the crest,
    // shoreline and Jacobian terms describe what the SEA is doing, this one
    // describes what something has DONE to it.
    //
    // Folded in AFTER the crest-foam distance fade on purpose. That fade exists
    // because procedural whitecaps compress toward the horizon into a white
    // wash (#943); a wake is a low-frequency, world-anchored, filtered signal
    // with no such failure mode, and inheriting a 12->45 m fade would delete
    // the trail behind the boat under any chase camera — i.e. delete the
    // feature. It gets its own, much longer fade from u_WakeFieldParams2.xy.
    //
    // Everything here is evaluated unconditionally and combined with max(): the
    // field sample is a plain textureLod (no derivative), so it is safe in this
    // position, and there is no branch on a texture-derived condition of the
    // kind docs/agent-rules/water-shading-nyquist.md §2 warns about.
    vec2 wakeWorldXZ = v_WorldPos.xz + u_RenderOrigin.xz;
    float wakeFoam = sampleWaterDisturbance(wakeWorldXZ, u_WaterDisturbance,
                                            u_WakeFieldParams, u_WakeFieldParams2);
    // Break the field up with the SAME foam pattern the rest of the foam uses,
    // so the wake reads as churned water rather than a painted stripe. Kept
    // partly transparent to the pattern (0.55 floor) because a wake's core is
    // genuinely continuous where a whitecap's is not.
    wakeFoam *= mix(0.55, 1.0, hasFoamTexture ? foamSample.r : foamPattern);
    wakeFoam *= u_WakeFieldParams.w;
    wakeFoam *= 1.0 - smoothstep(u_WakeFieldParams2.x, u_WakeFieldParams2.y, foamCamDist);
    foam = max(foam, clamp(wakeFoam, 0.0, 1.0));

    finalColor = mix(finalColor, vec3(foamBrightness), clamp(foam, 0.0, 1.0));

    // --- Final alpha ---
    // Deep ocean water is fully opaque. Only edges/shoreline might
    // soften via depthFade, but we keep a near-opaque floor.
    float transparency = 1.0;

    o_Color = vec4(finalColor, transparency);
    o_EntityID = u_EntityID;
    o_ViewNormal = octEncode(normalize(mat3(u_View) * normal));

    // Camera + wave-reprojection velocity. v_PrevWorldPos is the Gerstner
    // displacement re-evaluated at prev time (packed into u_NormalMapSpeed.z)
    // through u_PrevModel, so on-surface wave motion is captured correctly.
    vec4 clipCurr = u_ViewProjection     * vec4(v_WorldPos,     1.0);
    vec4 clipPrev = u_PrevViewProjection * vec4(v_PrevWorldPos, 1.0);
    vec2 ndcCurr = clipCurr.xy / clipCurr.w;
    vec2 ndcPrev = clipPrev.xy / clipPrev.w;
    o_Velocity = (ndcCurr - ndcPrev) * 0.5;
}
