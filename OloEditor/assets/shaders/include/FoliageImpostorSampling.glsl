// FoliageImpostorSampling.glsl — the ONE atlas-sampling body of the octahedral
// impostor card (issue #433), shared by Foliage_Impostor.glsl (forward) and
// Foliage_Impostor_GBuffer.glsl (deferred, #1225). Included by the FRAGMENT
// stage after it has declared: the CameraMatrices and FoliageParams UBO blocks
// and the varyings v_CardWorld, v_PivotWorld, v_AlphaCutoff, v_Rotation,
// v_Radius, v_MeshCoverage. Declares the two atlas samplers itself.
//
// SampleImpostorCard() also owns the DISCARD RULE, on purpose. The two paths
// used to disagree: forward discarded on `coverage < cutoff` and past
// ViewDistance, while the deferred copy collapsed the distance fade into
// `coverage * distFade < 0.3` — so the same layer drew a thinner canopy in
// Deferred and the tree line retreated ~7 m earlier (review finding on #1225).
// The forward rule is the visible one (foliage blends are OFF, so the alpha the
// forward card writes was never seen) and it is the rule both paths get.
//
// The atlas albedo already carries the layer tint (ImpostorBaker applies
// BaseColor at bake time) — never re-multiply by a per-instance tint here or
// the card reads far too dark.
//
// Sibling includes resolve inside include/.
#ifndef FOLIAGE_IMPOSTOR_SAMPLING_GLSL
#define FOLIAGE_IMPOSTOR_SAMPLING_GLSL

#include "BindlessHeap.glsl"
#ifdef OLO_BINDLESS
#define u_AlbedoAtlas OLO_HEAP_TEX_2D(0)  // rgb=albedo, a=coverage — TEX_DIFFUSE
#define u_NormalDepthAtlas OLO_HEAP_TEX_2D(10)  // rgb=obj normal, a=depth — TEX_USER_0
#else
layout(binding = 0) uniform sampler2D u_AlbedoAtlas;      // rgb=albedo, a=coverage
layout(binding = 10) uniform sampler2D u_NormalDepthAtlas; // rgb=obj normal, a=depth
#endif

#include "OctahedralImpostor.glsl"
#include "FoliageInstanceGeometry.glsl"

// Rotate a vector about +Y by angle.
vec3 rotateY(vec3 v, float angle)
{
    float c = cos(angle);
    float s = sin(angle);
    return vec3(v.x * c + v.z * s, v.y, -v.x * s + v.z * c);
}

struct ImpostorSample
{
    vec3 Albedo;       // atlas albedo, tint already baked in
    float Coverage;    // 3-frame blended coverage (already passed the alpha test)
    vec3 LocalNormal;  // object-space, normalized; rotate by v_Rotation for world
    float Dist;        // pivot -> camera distance
    float DistFade;    // 1 at FadeStart, 0 at ViewDistance (> 0 here, or we discarded)
};

// Samples the card for this fragment and applies the shared discard rule.
ImpostorSample SampleImpostorCard()
{
    float framesPerAxis = max(u_ImpostorParams0.x, 2.0);
    bool hemi = u_ImpostorParams0.y > 0.5;
    // WORLD-space card half-size — the virtual-plane offset vectors below are in
    // world units, so they must be divided by the world radius (object radius *
    // instance scale), NOT the object-space atlas radius.
    float radius = max(v_Radius, 1e-4);

    // Distance-driven detail ramp: near [< start] the card behaves like a cheap
    // stable single-frame billboard (no parallax, no cross-frame blend); across
    // [start, start+band] it cross-fades continuously to the full parallax
    // octahedral impostor — no pop, and both authoring knobs stay meaningful.
    float dist = distance(v_PivotWorld, u_CameraPosition);
    float lod = (u_ImpostorParams1.x > 0.5)
                    ? smoothstep(u_ImpostorParams0.z, u_ImpostorParams0.z + max(u_ImpostorParams0.w, 1e-3), dist)
                    : 1.0;
    float parallaxScale = u_ImpostorParams1.z * lod;

    // View direction in mesh-local space (undo the instance Y rotation).
    vec3 viewWorld = normalize(u_CameraPosition - v_PivotWorld);
    vec3 viewLocal = rotateY(viewWorld, -v_Rotation);

    vec2 grid = OctaDirToGrid(viewLocal, framesPerAxis, hemi);
    vec2 gridFloor = min(floor(grid), vec2(framesPerAxis - 1.0));
    vec2 f = grid - gridFloor;

    // 3-tile barycentric blend (quadBlendWeights); the two off-frames ramp in
    // with lod so the near look collapses to the single dominant frame.
    float w0 = min(1.0 - f.x, 1.0 - f.y);
    float w1 = abs(f.x - f.y) * lod;
    float w2 = min(f.x, f.y) * lod;
    float wsum = max(w0 + w1 + w2, 1e-4);
    w0 /= wsum;
    w1 /= wsum;
    w2 /= wsum;
    vec2 diag = (f.x > f.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
    vec2 maxFrame = vec2(framesPerAxis - 1.0);
    vec2 frame0 = gridFloor;
    vec2 frame1 = clamp(gridFloor + diag, vec2(0.0), maxFrame);
    vec2 frame2 = clamp(gridFloor + vec2(1.0, 1.0), vec2(0.0), maxFrame);

    vec2 frames[3] = vec2[3](frame0, frame1, frame2);
    float weights[3] = float[3](w0, w1, w2);

    float invN = 1.0 / framesPerAxis;
    vec3 pivotToCam = u_CameraPosition - v_PivotWorld;
    vec3 vertexToCam = u_CameraPosition - v_CardWorld;

    vec3 accAlbedo = vec3(0.0);
    vec3 accNormal = vec3(0.0);
    float accCoverage = 0.0;

    for (int i = 0; i < 3; ++i)
    {
        float weight = weights[i];
        if (weight <= 0.0)
            continue;

        // The direction this frame was captured from, back in world space.
        vec3 dirLocal = OctaFrameToDir(frames[i], framesPerAxis, hemi);
        vec3 dirWorld = rotateY(dirLocal, v_Rotation);

        // Virtual-plane reprojection: project the card ray onto this frame's
        // capture plane so each frame samples its own geometrically-correct UV
        // (otherwise the 3-frame blend ghosts). See issue #433 research.
        vec3 planeN = dirWorld;
        vec3 planeUp = (abs(planeN.y) > 0.999) ? vec3(0.0, 0.0, -1.0) : vec3(0.0, 1.0, 0.0);
        vec3 planeX = normalize(cross(planeUp, planeN));
        vec3 planeY = normalize(cross(planeN, planeX));

        // Intersect the camera->card-vertex ray with this frame's capture plane
        // (through the pivot, normal planeN): X = camera - offLen*vertexToCam, and
        // the in-plane offset from the pivot is pivotToCam - offLen*vertexToCam.
        float denom = dot(planeN, vertexToCam);
        if (abs(denom) < 1e-5)
            continue;
        float offLen = dot(planeN, pivotToCam) / denom;
        vec3 offVec = pivotToCam - vertexToCam * offLen;
        vec2 uvFrame = vec2(dot(planeX, offVec), dot(planeY, offVec)) / (2.0 * radius) + 0.5;

        // Single-step depth parallax along the in-plane view direction.
        vec2 tileUV = (frames[i] + clamp(uvFrame, 0.0, 1.0)) * invN;
        float depth = texture(u_NormalDepthAtlas, tileUV).a;
        vec2 viewTan = vec2(dot(planeX, viewWorld), dot(planeY, viewWorld));
        uvFrame += viewTan * (0.5 - depth) * parallaxScale;

        vec2 finalUV = (frames[i] + clamp(uvFrame, 0.0, 1.0)) * invN;
        vec4 alb = texture(u_AlbedoAtlas, finalUV);
        vec4 nd = texture(u_NormalDepthAtlas, finalUV);

        accAlbedo += alb.rgb * weight;
        accCoverage += alb.a * weight;
        accNormal += (nd.rgb * 2.0 - 1.0) * weight;
    }

    // The atlas albedo already has the layer tint baked in (ImpostorBaker applies
    // BaseColor at bake time) — do NOT re-multiply by v_Color here or the tint
    // is applied twice and the card reads far too dark.
    vec3 albedo = accAlbedo;
    float coverage = accCoverage;

    // Authored-mesh hand-over (issue #1233). Part of the SHARED discard rule
    // for the same reason the rest of it is: the impostor is the far side of a
    // partition whose near side is the plant's real geometry, and a forward
    // copy and a deferred copy of that test would drift exactly as the coverage
    // test once did. `false` = this draw is the card side.
    if (!foliageLodKeep(false, v_MeshCoverage, gl_FragCoord.xy))
        discard;

    // Alpha test against the baked coverage.
    if (coverage < v_AlphaCutoff)
        discard;

    // Distance fade (matches the flat-billboard path).
    float distFade = 1.0 - smoothstep(u_FadeStart, u_ViewDistance, dist);
    if (distFade <= 0.0)
        discard;

    ImpostorSample s;
    s.Albedo = albedo;
    s.Coverage = coverage;
    s.LocalNormal = normalize(accNormal);
    s.Dist = dist;
    s.DistFade = distFade;
    return s;
}

#endif // FOLIAGE_IMPOSTOR_SAMPLING_GLSL
