#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5): on the Vulkan backend vertex data is PULLED — binding 57
// is the engine-wide vertex-pull binding; the root struct carries this buffer's
// device address, so the SAME 20-byte {vec3 position, vec2 uv} stream the
// attribute path consumes is read by index instead.
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
// RayTracedShadow.glsl — draw A of RayTracedShadowPass. Issue #1056.
//
// One shadow ray per pixel per ray-traced light, traced with GL_EXT_ray_query
// against the #978 TLAS, writing a screen-space visibility MASK: one channel
// per light, 1 = lit, 0 = occluded.
//
// VULKAN ONLY, and loaded only behind RenderCommand::SupportsRayTracing() —
// GL_EXT_ray_query has no OpenGL representation, exactly like the probe shader
// and the mesh-shader stages. The pass falls back to the raster shadow tier
// everywhere else, loudly and counted (ShadowTechnique.h).
//
// WHY THIS IS A FRAGMENT SHADER AND NOT A COMPUTE DISPATCH. Ray queries are
// legal in every stage, and the output is a screen-resolution colour target
// that two more fullscreen draws in the same node consume — the exact shape
// SSGIRenderPass and CloudscapeRenderPass already use. A compute variant would
// need a storage image, an image-namespace binding and a hand-placed barrier
// between the three stages, all to produce the same texture. The trade is that
// this pass cannot be converted to async compute later; when it wants to be,
// the shader body moves unchanged because nothing here is a fragment-only
// construct (no derivatives, no discard).
//
// ZERO NEW BUFFER BINDINGS. The TLAS arrives as a DEVICE ADDRESS in the one
// UBO — accelerationStructureEXT(uvec2) converts an address straight to an
// acceleration-structure handle, so it needs no descriptor at all. That UBO is
// UBO_RAY_TRACING (65), the block RayTracingProbe.comp also declares: the
// buffer-binding namespace has been full since #978 took the last free number,
// and the probe is a diagnostic that never runs inside a frame, so the two
// blocks can never be live at once. Each pass rebinds its own UBO before its
// draws, which is the established idiom here (see SSGIRenderPass's comment on
// binding 40).
//
// THE WITHIN-SHADER RULE HOLDS: 65 is also TEX_VSM_PHYSICAL, and this shader
// declares no VSM sampler and must not start to.
//
// MASKED OCCLUDERS SHADOW AS SOLID, DELIBERATELY. A ray query gets no any-hit
// shader for free, and outside OLO_BINDLESS a shader reaches a material texture
// through a PER-DRAW slot binding that a single fullscreen dispatch has no
// scope to supply — see include/RayTracingAlphaTest.glsl's "who supplies the
// texture" note. Running its POLICY half against a fully-opaque sample (which
// is what the probe does) confirms every candidate, so this shader states that
// outcome directly with gl_RayFlagsOpaqueEXT instead of paying for a
// traversal loop that always says yes. The consequence is real and visible: an
// alpha-cutout leaf casts the shadow of its quad. It is counted as
// ShadowTechniqueStats::MaskedOccludersShadowedAsSolid rather than left as a
// comment, and it is unblocked by the shader-visible sampler heap, #805.
// =============================================================================

#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

layout(location = 0) out vec4 o_Visibility;
// Per channel, the distance to the blocker that stopped the last ray of that
// channel; 0 when nothing blocked. It is fed to the temporal resolve's
// OLO_SURFACE_TEST_HIT_DISTANCE test, which is the lane SurfaceHistoryRecord
// reserved "for a future ray hit" — the signal that separates two samples
// agreeing on depth and normal but disagreeing about WHICH occluder they
// found, which is exactly a shadow's silhouette moving across a flat wall.
//
// It is A blocker distance, not THE nearest one: gl_RayFlagsTerminateOnFirstHit
// commits whichever intersection traversal reaches first. That is the right
// trade for a visibility ray, and the value is used only as an identity signal
// under a 10% relative threshold, never to size a penumbra.
layout(location = 1) out vec4 o_HitDistance;

layout(location = 0) in vec2 v_TexCoord;

#include "include/BindlessHeap.glsl"

#ifdef OLO_BINDLESS
#define u_DepthTexture OLO_HEAP_TEX_2D(19)  // TEX_POSTPROCESS_DEPTH
#define u_GBufferNormal OLO_HEAP_TEX_2D(44) // TEX_GBUFFER_NORMAL
#else
layout(binding = 19) uniform sampler2D u_DepthTexture;  // scene depth (nonlinear, [0,1])
layout(binding = 44) uniform sampler2D u_GBufferNormal; // RT1: rg = oct world normal, z = roughness, w = ao
#endif

#include "include/SkyDepth.glsl"

// Opt into the shared blue-noise tile at TEX_BLUE_NOISE (17). The standing
// rule for that header is that no shader enabling the global sampler may also
// declare uniform or storage block 17; this one declares block 65 and nothing
// else, and StochasticSamplerTest.NoShaderCollidesWithTheBlueNoiseSlot greps
// the tree rather than trusting the comment.
#define OLO_BLUE_NOISE_GLOBAL_SAMPLER
#include "include/StochasticCommon.glsl"

// UBO_RAY_TRACING (65). Shared verbatim by the resolve and filter draws so one
// upload feeds all three; mirrored on the CPU by
// UBOStructures::RayTracingShadowUBO.
layout(std140, binding = 65) uniform RayTracingShadowParams
{
    mat4 u_InvView;
    mat4 u_InvProjection;
    mat4 u_View;
    // One row per mask channel. xyz = the world-space direction TOWARD the
    // light for a directional light, or the light's world POSITION for a
    // punctual one; w = the light type (0 = none, 1 = directional, 2 = punctual).
    vec4 u_LightVectors[4];
    // Per channel: x = tan(angular radius) for a directional light or the
    // emitter RADIUS in metres for a punctual one, y = the light's range in
    // metres (0 = unbounded), zw reserved.
    vec4 u_LightShapes[4];
    uvec4 u_TlasAddressAndCounts; // xy = TLAS device address, z = active channel count, w = frame index
    vec4 u_RayParams;    // x = raysPerPixel, y = maxRayDistance (0 = unbounded), z = normalBias, w = reserved
    vec4 u_ScreenParams; // x = width, y = height, z = 1/width, w = 1/height
    vec4 u_TemporalParams; // x = feedback, y = hasVelocity, z = historyUsable, w = clipGamma
    vec4 u_FilterParams;   // x = spatialRadiusPixels, y = spatialEnabled, zw reserved
};

// Every ray starts this far along its own direction, on top of the normal
// offset. The normal offset alone cannot fix a ray that leaves a surface at a
// grazing angle — the offset is perpendicular to the error, not along it — and
// this is the standard second term. Metres.
const float RT_SHADOW_RAY_TMIN = 0.005;

// Hard cap on the per-pixel sample loop. Mirrors
// RayTracedShadowSettings::RaysPerPixel's clamp; a uniform-driven loop needs a
// constant bound or the compiler cannot unroll it and a bad upload can hang the
// GPU.
const int RT_SHADOW_MAX_RAYS = 8;

// Octahedral decode — matches octEncodeGB() in PBR_GBuffer.glsl.
vec3 OctDecode(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

vec3 ViewPosFromDepth(vec2 uv, float depth)
{
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 view = u_InvProjection * ndc;
    return view.xyz / view.w;
}

// OrthonormalBasis comes from include/MathCommon.glsl through
// StochasticCommon.glsl — the same basis OloCosineHemisphere builds its
// samples in, so the cone sampled below and every other stochastic direction in
// the engine agree about what "around this axis" means.

// Sample a direction uniformly inside the cone of half-angle atan(tanRadius)
// around `axis`. THIS is where the penumbra comes from, and why it is a
// geometric fact rather than a filter width: the ray leaves within the light's
// solid angle, so a distant occluder intercepts a wider spread of the cone's
// footprint than a near one, and the shadow softens with occluder distance on
// its own.
vec3 SampleCone(vec3 axis, float tanRadius, vec2 u)
{
    if (tanRadius <= 0.0)
        return axis;

    // Disc sample in the plane perpendicular to the axis, at unit distance —
    // the small-angle parameterisation, which is exact enough for the angular
    // radii a light source actually has (the sun is 0.265 degrees) and avoids a
    // trigonometric inverse per ray.
    float radius = tanRadius * sqrt(u.x);
    float theta = 6.28318530718 * u.y;
    vec3 t;
    vec3 b;
    OrthonormalBasis(axis, t, b);
    return normalize(axis + t * (radius * cos(theta)) + b * (radius * sin(theta)));
}

void main()
{
    // Every channel defaults to LIT, not to occluded. A channel no light was
    // assigned, a sky pixel, and a frame where the trace could not run all
    // read 1 — so the worst a stale or absent mask can do is fail to darken,
    // never darken something that should be lit. The house rule is that a path
    // which cannot do its job says so loudly; the counters do the saying, and
    // the default here makes sure the failure is not also a black frame.
    o_Visibility = vec4(1.0);
    o_HitDistance = vec4(0.0);

    float depth = texture(u_DepthTexture, v_TexCoord).r;
    if (oloDepthIsSky(depth))
        return;

    uint channelCount = min(u_TlasAddressAndCounts.z, 4u);
    if (channelCount == 0u)
        return;

    vec3 viewPos = ViewPosFromDepth(v_TexCoord, depth);
    vec3 worldPos = (u_InvView * vec4(viewPos, 1.0)).xyz;

    vec4 packedNormal = texture(u_GBufferNormal, v_TexCoord);
    vec3 N = OctDecode(packedNormal.xy);

    int rayCount = clamp(int(u_RayParams.x + 0.5), 1, RT_SHADOW_MAX_RAYS);
    float maxDistance = u_RayParams.y > 0.0 ? u_RayParams.y : 1.0e30;
    vec3 origin = worldPos + N * u_RayParams.z;

    ivec2 pixel = ivec2(gl_FragCoord.xy);
    uint frameIndex = u_TlasAddressAndCounts.w;

    vec4 visibility = vec4(1.0);
    vec4 hitDistance = vec4(0.0);
    for (uint channel = 0u; channel < 4u; ++channel)
    {
        if (channel >= channelCount)
            break;

        float lightType = u_LightVectors[channel].w;
        if (lightType < 0.5)
            continue;

        // Directional: the vector IS the toward-light direction and the ray is
        // unbounded (subject to maxDistance). Punctual: the vector is a
        // position, so the direction and the ray's tMax both come from the
        // offset to it — a ray that runs past the light would report the wall
        // BEHIND the light as an occluder.
        vec3 toLight;
        float tMax;
        float shapeTan;
        if (lightType < 1.5)
        {
            toLight = normalize(u_LightVectors[channel].xyz);
            tMax = maxDistance;
            shapeTan = u_LightShapes[channel].x;
        }
        else
        {
            vec3 offset = u_LightVectors[channel].xyz - origin;
            float distance = length(offset);
            if (distance <= 1.0e-4)
                continue;
            // Beyond the light's own falloff range it contributes nothing, so
            // its visibility term is irrelevant and the ray is pure cost. Left
            // at 1.0 rather than 0.0: the lighting shader's attenuation is what
            // zeroes the contribution, and a 0 here would also darken any other
            // term that ever reads this channel.
            float range = u_LightShapes[channel].y;
            if (range > 0.0 && distance > range)
                continue;
            toLight = offset / distance;
            // Stop just short of the emitter so its own geometry, if it has
            // any, is not counted as its own occluder.
            tMax = min(distance * 0.999, maxDistance);
            // A sphere of radius r at distance d subtends tan(theta) ~ r / d.
            shapeTan = distance > 1.0e-4 ? u_LightShapes[channel].x / distance : 0.0;
        }

        // A surface facing away from the light is in FORM shadow, which the
        // lighting pass's own N.L already handles. Tracing it would spend a ray
        // to rediscover that, and would start the ray inside the surface.
        if (dot(N, toLight) <= 0.0)
        {
            visibility[channel] = 0.0;
            hitDistance[channel] = 0.0;
            continue;
        }

        float occluded = 0.0;
        for (int s = 0; s < RT_SHADOW_MAX_RAYS; ++s)
        {
            if (s >= rayCount)
                break;

            // Blue-noise-seeded stratified pair. Decorrelating per channel as
            // well as per sample matters: without the channel in the dimension,
            // two lights would jitter their cones identically and their
            // penumbrae would share the same noise, which the temporal resolve
            // then cannot average away.
            vec2 u = OloSampleStratified2D(pixel, frameIndex, uint(s), uint(rayCount), channel * 2u);
            vec3 direction = SampleCone(toLight, shapeTan, u);

            rayQueryEXT rayQuery;
            // gl_RayFlagsOpaqueEXT forces every candidate opaque — see the
            // masked-occluder note in the header. With TerminateOnFirstHit the
            // hardware commits the first intersection and stops, which is the
            // cheapest possible visibility ray and needs no traversal loop.
            rayQueryInitializeEXT(rayQuery, accelerationStructureEXT(u_TlasAddressAndCounts.xy),
                                  gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT,
                                  0xFFu, origin, RT_SHADOW_RAY_TMIN, direction, tMax);
            rayQueryProceedEXT(rayQuery);
            if (rayQueryGetIntersectionTypeEXT(rayQuery, true) != gl_RayQueryCommittedIntersectionNoneEXT)
            {
                occluded += 1.0;
                hitDistance[channel] = rayQueryGetIntersectionTEXT(rayQuery, true);
            }
        }

        visibility[channel] = 1.0 - occluded / float(rayCount);
    }

    o_Visibility = visibility;
    o_HitDistance = hitDistance;
}
