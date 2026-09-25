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

// Screen-Space Global Illumination — one-bounce indirect *diffuse* (deferred path).
//
// For each opaque pixel, reconstruct its view-space position + normal from the
// G-Buffer, then cast a cosine-weighted hemisphere of short rays around the
// normal. Each ray linear-marches in view space against scene depth; on a hit
// (the ray passes just behind a visible surface, within a thickness tolerance)
// the lit scene colour at the hit UV is the incoming indirect radiance — so a
// saturated wall bleeds its colour onto a neutral floor.
//
// WHAT IT OWNS (issue #1336). The lighting pass has ALREADY added indirect
// diffuse to this pixel: the ambient ladder's rung (lightmap, probe volume or
// IBL), which assumed radiance arrives from every direction of the hemisphere.
// SSGI is a second estimate of that same quantity, better informed over the
// directions its rays resolve on screen and uninformed everywhere else. So it
// must REPLACE the ladder over the directions it resolved, never add to it:
// adding counts the sky (or the baked bounce) behind a wall AND the wall's
// bounced light for the same directions. With `c` the resolved fraction of the
// cosine-weighted hemisphere, the pixel's indirect diffuse becomes
//
//     (1 - c) * ladder  +  kD * albedo * mean(resolved radiance)
//
// and this pass hands the composite the difference from what the frame already
// carries: kD * albedo * sum(w_i * L_i) / N  -  c * ladder. It is SIGNED — a
// floor under a dark overhang loses the sky the ladder gave it — and a miss
// contributes exactly nothing, because the ladder still answers for it. The
// ladder is re-selected here by the SAME function the lighting pass shaded the
// pixel with (include/AmbientLadder.glsl), with the same AO applied, so the
// fraction removed is the fraction that was there.
// docs/agent-rules/lighting-signal-contract.md has the ownership table.
//
// OUTPUT (issue #902): this pass writes ONLY the stochastic term into the
// dedicated SSGISignal target — rgb = the signed indirect-diffuse delta, a = the
// positive view-space depth of the shading point (the temporal resolve's
// disocclusion test needs it and there is no depth history buffer). It no
// longer composites: PostProcess_SSGIResolve.glsl accumulates the signal and
// PostProcess_SSGIComposite.glsl adds it to the upstream colour afterwards.
// Compositing here would make the output un-accumulable, which is exactly the
// structural blocker #902 removed.
//
// Cosine-weighted importance sampling (Malley's method) means the Monte-Carlo
// estimator of the diffuse irradiance integral is simply the mean of the
// per-ray radiance, and the resolved fraction is the mean of the per-ray
// confidence.
//
// The hemisphere directions come from the shared blue-noise sampler (issue
// #706) rather than the interleaved-gradient hash this pass used to carry. Same
// ray count, same cost: what changes is that the residual error is now
// blue-noise-distributed across the screen instead of white, so the low
// spatial frequencies the eye is most sensitive to — and that no small filter
// can remove — carry far less of it. See include/StochasticCommon.glsl.
//
// The math here is mirrored on the CPU by ScreenSpaceGIMathTest, and the
// rendered frame is checked by SSGIVisualEvidenceTest.

layout(location = 0) out vec4 o_Color;
// The guide plane (issue #708), packed exactly like G-Buffer RT1:
//   rg = octahedral world normal, b = roughness, a = AO.
// Every later stage of the denoiser chain is guided by THIS, not by the
// full-resolution G-Buffer, because the chain runs at the trace band and a
// guide read at a different resolution than the signal it weights would reject
// taps by silhouettes the signal cannot see. It is also the source the graph
// extracts SSGISurfaceHistory from, so the temporal resolve's surface test
// compares like with like across frames.
layout(location = 1) out vec4 o_Guide;

layout(location = 0) in vec2 v_TexCoord;

#include "include/BindlessHeap.glsl"

// Heap-bindless conversion (issue #691, bucket 1). The BODY below is
// byte-identical between the two variants — only these declarations move, and
// each names the same TEX_* constant SSGIRenderPass binds with.
#ifdef OLO_BINDLESS
#define u_SceneColor OLO_HEAP_TEX_2D(0)
#define u_DepthTexture OLO_HEAP_TEX_2D(19)   // TEX_POSTPROCESS_DEPTH
#define u_GBufferNormal OLO_HEAP_TEX_2D(44)  // TEX_GBUFFER_NORMAL
#define u_GBufferAlbedo OLO_HEAP_TEX_2D(43)  // TEX_GBUFFER_ALBEDO
#define u_GBufferBakedGI OLO_HEAP_TEX_2D(69) // TEX_GBUFFER_BAKEDGI (#1336)
#define u_IrradianceMap OLO_HEAP_TEX_CUBE(10) // TEX_USER_0 (#1336)
#define u_ScreenSpaceAO OLO_HEAP_TEX_2D(20)  // TEX_SSAO (#1336)
#define u_GBufferEmissive OLO_HEAP_TEX_2D(45) // TEX_GBUFFER_EMISSIVE (#1336)
#else
layout(binding = 0) uniform sampler2D u_SceneColor;     // lit upstream HDR colour (indirect light source)
layout(binding = 19) uniform sampler2D u_DepthTexture;  // scene depth (nonlinear, [0,1])
layout(binding = 44) uniform sampler2D u_GBufferNormal; // RT1: rg = oct world normal, z = roughness, w = ao
layout(binding = 43) uniform sampler2D u_GBufferAlbedo; // RT0: rgb = albedo, a = metallic
// The ladder the trace replaces (issue #1336): RT5 is the lightmap sample the
// lighting pass handed the ladder, the irradiance cube its IBL rung, the AO
// buffer the visibility it multiplied the result by. Bound by SSGIRenderPass
// (white / placeholder when a source is absent; the UBO lanes gate the reads).
layout(binding = 69) uniform sampler2D u_GBufferBakedGI;
layout(binding = 10) uniform samplerCube u_IrradianceMap;
layout(binding = 20) uniform sampler2D u_ScreenSpaceAO;
// RT2's flag lane: an unlit pixel was given no ladder (issue #1336).
layout(binding = 45) uniform sampler2D u_GBufferEmissive;
#endif

// The shared blue-noise tile at TEX_BLUE_NOISE (17) plus the sample sequence
// that rotates by it. Must come AFTER BindlessHeap.glsl — the bindless branch
// of the tile declaration expands OLO_HEAP_TEX_2D.
#define OLO_BLUE_NOISE_GLOBAL_SAMPLER
#include "include/StochasticCommon.glsl"

layout(std140, binding = 40) uniform SSGIParams
{
    mat4 u_Projection;
    mat4 u_InvProjection;
    mat4 u_View;
    vec4 u_RayParams;      // x = MaxSteps, y = MaxDistance (view units), z = Thickness, w = Stride (view units)
    vec4 u_ShadeParams;    // x = Intensity, y = RayCount, z = EdgeFade (UV), w = unused
    vec4 u_ScreenParams;   // x = FULL-res width, y = height, z = 1/width, w = 1/height
    vec4 u_Flags;          // x = DebugView (0/1), y = FrameIndex, zw = pad
    vec4 u_TemporalParams; // #902; read by the resolve/composite draws, not here
    // #708 denoiser chain. TraceParams is the band the trace / pre-blur /
    // resolve / post-blur actually run at — half of ScreenParams when the
    // half-resolution trace is on, equal to it otherwise. Every stage that
    // steps by a texel MUST use TraceParams.zw, not ScreenParams.zw.
    vec4 u_TraceParams;    // x = trace width, y = trace height, z = 1/width, w = 1/height
    vec4 u_DenoiseParams;  // x = PreBlurRadius (px), y = PostBlurMinRadius, z = PostBlurMaxRadius, w = VarianceKnee
    vec4 u_DenoiseGuide;   // x = PlaneTolerance (relative), y = NormalPower, z = TargetHistoryLength, w = RayDistribution (0/1)
    vec4 u_LadderParams;   // #1336: x = EnableIBL, y = EnableLightProbes, z = IBLIntensity, w = unused
    vec4 u_ScreenAOParams; // #1336: x = live, y = strength, z/w = reconstruction projection (2,2) / (3,2)
    mat4 u_InverseRelativeView; // #1336: inverse of the render-relative view
};

// The ambient ladder and the AO upsample the lighting pass shaded this pixel
// with (issue #1336). LightProbeSampling brings the probe volume's own
// bindings (UBO 22 / 51, SSBO 8, the always-bound DDGI atlas slots).
#include "include/PBRCommon.glsl"
#include "include/LightProbeSampling.glsl"
#define OLO_AMBIENT_LADDER_EXPLICIT_CONTROLS
#include "include/AmbientLadder.glsl"
// The weather wetness DeferredLighting applies before its ladder (UBO 54 plus
// the always-bound cloud-shadow slot 62, as GBufferRaySurface.glsl uses it).
#include "include/AtmosphereShading.glsl"
#define OLO_SSAO_TAP_DEPTH(uv) texture(u_DepthTexture, (uv)).r
#include "include/ScreenSpaceAOSampling.glsl"

// The sky sentinel this pass writes into alpha now lives in the denoiser
// header, because every later stage of the chain has to recognise it — see
// OLO_DENOISE_SKY_VIEW_DEPTH there for why the value is 60000 and not a round
// number.
#include "include/SpatialDenoise.glsl"

const float SKY_DEPTH = 0.999999;
const float OLO_MAX_VIEW_DEPTH = OLO_DENOISE_SKY_VIEW_DEPTH;
const int HARD_MAX_STEPS = 64;  // loop-safety cap; must match kSSGIMaxSteps
const int HARD_MAX_RAYS = 32;   // loop-safety cap; must match kSSGIMaxRays

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

// (The orthonormal basis and the Malley mapping both moved into the shared
// header — OrthonormalBasis() in MathCommon.glsl and OloCosineHemisphere() in
// StochasticCommon.glsl. They were byte-identical to the local copies this
// pass carried; keeping one copy is what stops the sampler and the basis
// drifting apart the way the IBL bake shaders' radical inverses did (#262).)

// The full-resolution texel this (possibly half-resolution) fragment stands on.
// The shading point's depth, normal and albedo MUST come from one whole texel:
// a bilinear tap at a half-res fragment centre averages a 2x2 block, and across
// a silhouette that averages a foreground depth with a background one into a
// position that lies on neither surface — the ray then starts inside geometry
// and the pixel goes black. `texelFetch` cannot do that by construction.
//
// Everything the ray MARCH samples is still an ordinary filtered lookup at an
// arbitrary UV; only these three centre reads are snapped.
ivec2 FullResTexel(vec2 uv)
{
    ivec2 maxTexel = ivec2(max(u_ScreenParams.xy - 1.0, vec2(0.0)));
    return clamp(ivec2(uv * u_ScreenParams.xy), ivec2(0), maxTexel);
}

void main()
{
    ivec2 centerTexel = FullResTexel(v_TexCoord);
    float depth = texelFetch(u_DepthTexture, centerTexel, 0).r;

    // View-space position first, so the depth this pass hands the temporal
    // resolve in alpha is written on EVERY path — including the sky early-out.
    // A pixel that returned before filling alpha would compare against zero
    // next frame and read as a permanent disocclusion.
    vec3 P = ViewPosFromDepth(v_TexCoord, depth); // view-space position (z < 0)
    // Saturate rather than max(): at the far plane the inverse-projection
    // divide can hand back a non-finite w, and a single Inf or NaN in alpha
    // would come back next frame as a NaN confidence, a NaN resolve, and — once
    // bloom has spread it — a black block (the amplification chain in
    // docs/agent-rules/render-graph-transient-aliasing.md). The predicate is
    // FALSE for NaN and Inf alike, which is the point.
    //
    // The ceiling is 60000, not some round 1e6: alpha travels through an
    // RGBA16F signal target and an RGBA16F history copy, and half-float tops
    // out at 65504. A larger sentinel would be stored as +Inf — reintroducing
    // exactly the value this guard exists to keep out — so the clamp has to
    // land inside the format that carries it. 60000 is exactly representable
    // in half (1875 x 32) and is far beyond any real view distance.
    float rawViewDepth = -P.z;
    float viewDepth = (rawViewDepth > 0.0 && rawViewDepth < OLO_MAX_VIEW_DEPTH) ? rawViewDepth
                                                                                : OLO_MAX_VIEW_DEPTH;

    vec4 gN = texelFetch(u_GBufferNormal, centerTexel, 0);

    // The guide plane is written on EVERY path, sky included, for the same
    // reason alpha is: it becomes next frame's SSGISurfaceHistory, and a texel
    // the trace returned early from would hand the resolve an undefined normal
    // to run its surface test against.
    o_Guide = gN;

    if (depth >= SKY_DEPTH) // sky / background — receives no GI
    {
        o_Color = vec4(0.0, 0.0, 0.0, viewDepth);
        return;
    }

    // An UNLIT pixel (grid, light billboards) took no ambient in the lighting
    // pass, so there is no ladder here for the trace to replace (issue #1336).
    if (oloGBufferFlagsAreUnlit(oloDecodeGBufferFlags(texelFetch(u_GBufferEmissive, centerTexel, 0).a)))
    {
        o_Color = vec4(0.0, 0.0, 0.0, viewDepth);
        return;
    }

    vec4 albedoMetallic = texelFetch(u_GBufferAlbedo, centerTexel, 0);
    vec3 albedo = albedoMetallic.rgb;

    // World normal -> view space.
    vec3 Nworld = OctDecode(gN.xy);
    vec3 Nview = normalize(mat3(u_View) * Nworld);

    // ---- THE LADDER THIS PIXEL WAS SHADED WITH (issue #1336) --------------
    // Re-selected by the same function DeferredLightingShared called, from the
    // same inputs: RT5, the probe volume, the irradiance cube, the same rung
    // controls, and the same material x screen-space AO it was multiplied by.
    // `u_View` is the ABSOLUTE view the marcher projects with; the ladder needs
    // the RENDER-RELATIVE position the probe-volume bounds are uploaded in, the
    // one DeferredLighting gets by subtracting u_RenderOrigin. The CPU uploads
    // that inverse once per frame rather than inverting a mat4 per pixel.
    vec3 worldPos = (u_InverseRelativeView * vec4(P, 1.0)).xyz;
    vec3 Vworld = normalize(u_InverseRelativeView[3].xyz - worldPos);
    float metallic = albedoMetallic.a;
    // The surface DeferredLighting shaded: roughness floored as it is read,
    // then the weather wetness on albedo and roughness, in that order.
    float roughness = max(gN.z, MIN_ROUGHNESS);
    atmosphereApplyWetness(albedo, roughness, Nworld);
    float screenAO = 1.0;
    if (u_ScreenAOParams.x > 0.5)
        screenAO = oloScreenSpaceAOVisibility(
            oloSampleScreenSpaceAO(u_ScreenSpaceAO, v_TexCoord, u_ScreenAOParams.z, u_ScreenAOParams.w),
            u_ScreenAOParams.y);
    // RT5 only when the pass bound the real one (u_LadderParams.w): the white
    // placeholder would read as "lightmapped, E = 1".
    vec4 lightmapSample = (u_LadderParams.w > 0.5) ? texelFetch(u_GBufferBakedGI, centerTexel, 0) : vec4(0.0);
    OloAmbientDiffuseRung rung = oloSelectAmbientDiffuseRung(
        lightmapSample, worldPos, Nworld, Vworld, u_IrradianceMap,
        u_LadderParams.x > 0.5, u_LadderParams.y > 0.5, u_LadderParams.z);
    vec3 ladderDiffuse =
        oloAmbientDiffuseFromRung(rung, albedo, metallic, roughness, Nworld, Vworld) * (gN.w * screenAO);
    // The weight a resolved ray's radiance is reflected with: the SAME weight the
    // rung applies to its own irradiance — the (1 - F)(1 - metallic) body-lobe
    // split on every physical rung, so a metal takes no diffuse bounce (the old
    // `albedo * mean(L)` gave it in full), and plain albedo on the flat fill.
    // Weighting the two estimates differently would make the mixture change
    // the pixel even where the resolved radiance equals what the rung assumed.
    OloAmbientDiffuseRung unitRung = rung;
    unitRung.NormalizedIrradiance = vec3(1.0);
    unitRung.Scale = 1.0;
    vec3 diffuseWeight = oloAmbientDiffuseFromRung(unitRung, albedo, metallic, roughness, Nworld, Vworld);

    float maxSteps = u_RayParams.x;
    float maxDist = u_RayParams.y;
    float thickness = u_RayParams.z;
    float stride = u_RayParams.w;
    int rayCount = clamp(int(u_ShadeParams.y), 1, HARD_MAX_RAYS);
    float edge = u_ShadeParams.z;

    // Depth-proportional bias along the normal so the first march step does not
    // self-intersect the originating surface.
    vec3 vStart = P + Nview * (0.02 * -P.z);

    ivec2 pixel = ivec2(gl_FragCoord.xy);
    uint frameIndex = uint(max(u_Flags.y, 0.0));

    vec3 indirect = vec3(0.0);
    float resolved = 0.0; // sum of per-ray confidence: the directions SSGI answers for

    for (int r = 0; r < HARD_MAX_RAYS; ++r)
    {
        if (r >= rayCount)
            break;

        // Cosine-weighted hemisphere sample (Malley's method), issue #706.
        //
        // The radius stays stratified across rays exactly as before; what the
        // shared sampler adds is a blue-noise jitter WITHIN each stratum (the
        // old form used the same u1 at every pixel, making its dimension-0 error
        // a screen-wide constant) and a blue-noise-rotated azimuth in place of
        // the interleaved-gradient hash. See OloSampleStratified2D for why
        // rotating the radius instead measured WORSE than the noise it replaced.
        //
        // Ray distribution (issue #708, step 1) subdivides each of those strata
        // four ways and gives one quarter to each pixel of the 2x2 quad, so
        // adjacent pixels sample complementary directions and the pre-blur that
        // follows recovers close to 4x the ray count instead of re-averaging
        // near-identical hemispheres. Per-pixel stratification is unchanged —
        // see OloSampleQuadDistributed2D.
        vec2 u = u_DenoiseGuide.w > 0.5
                     ? OloSampleQuadDistributed2D(pixel, frameIndex, uint(r), uint(rayCount), 0u)
                     : OloSampleStratified2D(pixel, frameIndex, uint(r), uint(rayCount), 0u);
        vec3 dir = OloCosineHemisphere(u, Nview);

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
                float confidence = edgeFade * distFade;
                indirect += texture(u_SceneColor, uv).rgb * confidence;
                resolved += confidence;
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

    // The mixture's DIFFERENCE from the frame (issue #1336): the resolved
    // directions' radiance through the body lobe, minus the share of the
    // ladder's estimate those directions carried. A miss adds 0 to both sums,
    // so an unresolved hemisphere hands back exactly zero and the ladder keeps
    // it. Diffuse reflectance already folds the 1/pi normalisation, so a
    // resolved ray contributes kD * albedo * L.
    float invRays = 1.0 / float(rayCount);
    vec3 indirectDiffuse = diffuseWeight * (indirect * invRays) - (resolved * invRays) * ladderDiffuse;

    // A non-finite ladder (a probe atlas read gone wrong) must not enter the
    // accumulator, where one NaN spreads through the history.
    if (any(isnan(indirectDiffuse)) || any(isinf(indirectDiffuse)))
        indirectDiffuse = vec3(0.0);

    // Signal only — no base colour, no intensity, no debug branch. All three
    // belong to the composite draw (PostProcess_SSGIComposite.glsl); mixing any
    // of them in here would put non-stochastic energy into the buffer the
    // temporal resolve accumulates. Alpha is the view depth the resolve's
    // disocclusion test compares against next frame.
    o_Color = vec4(indirectDiffuse, viewDepth);
}
