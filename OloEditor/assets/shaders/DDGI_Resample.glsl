// =============================================================================
// DDGI_Resample.glsl — capture grid -> octahedral hit-point cache (issue #632)
//
// Runs once per captured probe with the viewport set to the probe's hit tile
// (HitCacheTexels^2, no border) on the hit-atlas MRT FBO. For every hit texel
// it derives the fixed octahedral direction, selects the cube face + face UV
// with the SAME per-face basis math the rasterizer used (lookAt with the
// ReflectionProbeBaker face tables — see kFaceTargets/kFaceUps in
// DDGIProbeUpdatePass.cpp), reads the face cell of the 3x2 capture grid, and
// writes the cache texel:
//   RT0 (RGBA8)   — HitAlbedo: rgb albedo, a = DDGI_HIT_* flag
//   RT1 (RGBA16F) — HitGeo: rg oct normal, b hit distance (< 0 = sky),
//                   a = DDGI_HIT_* flag (canonical flag home, exact in fp16)
// Sky (capture RT1.b < 0): distance -1, flag DDGI_HIT_SKY.
// =============================================================================

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

#include "include/DDGICommon.glsl"

layout(std140, binding = 7) uniform DDGIPassData
{
    mat4 u_DDGIModel;
    mat4 u_DDGINormalMatrix;
    vec4 u_DDGIBaseColor;
    vec4 u_DDGIProbePosition; // w = probe linear index
};

layout(binding = 0) uniform sampler2D u_CaptureAlbedo; // capture RT0 (albedo + flag)
layout(binding = 1) uniform sampler2D u_CaptureGeo;    // capture RT1 (oct normal + distance)

layout(location = 0) in vec2 v_TexCoord;

layout(location = 0) out vec4 o_HitAlbedo;
layout(location = 1) out vec4 o_HitGeo;

// Per-face bases matching lookAt(probe, probe + f, up) with the capture face
// tables: s = normalize(cross(f, up)), u = cross(s, f). A direction d in the
// face's frustum projects to ndc = (dot(s,d), dot(u,d)) / dot(f,d) under the
// 90-degree square projection, i.e. faceUV = ndc * 0.5 + 0.5 — guaranteed
// consistent with the rasterized cells, no cubemap-convention guesswork.
const vec3 kFaceF[6] = vec3[6](
    vec3(1.0, 0.0, 0.0), vec3(-1.0, 0.0, 0.0),
    vec3(0.0, 1.0, 0.0), vec3(0.0, -1.0, 0.0),
    vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, -1.0));
const vec3 kFaceS[6] = vec3[6](
    vec3(0.0, 0.0, -1.0), vec3(0.0, 0.0, 1.0),
    vec3(1.0, 0.0, 0.0), vec3(1.0, 0.0, 0.0),
    vec3(1.0, 0.0, 0.0), vec3(-1.0, 0.0, 0.0));
const vec3 kFaceU[6] = vec3[6](
    vec3(0.0, -1.0, 0.0), vec3(0.0, -1.0, 0.0),
    vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, -1.0),
    vec3(0.0, -1.0, 0.0), vec3(0.0, -1.0, 0.0));

void main()
{
    int t = u_DDGIHitCacheTexels;
    int probeIdx = int(u_DDGIProbePosition.w + 0.5);
    ivec2 tileOrigin = ddgiProbeTileCoord(probeIdx) * t;
    ivec2 local = ivec2(gl_FragCoord.xy) - tileOrigin;

    vec3 dir = ddgiTexelDirection(local, t);

    // Dominant-axis cube face selection (matches atlasCubeFace's face order).
    vec3 a = abs(dir);
    int face;
    if (a.x >= a.y && a.x >= a.z)
    {
        face = dir.x > 0.0 ? 0 : 1;
    }
    else if (a.y >= a.z)
    {
        face = dir.y > 0.0 ? 2 : 3;
    }
    else
    {
        face = dir.z > 0.0 ? 4 : 5;
    }

    float forward = dot(kFaceF[face], dir);
    vec2 uv = vec2(dot(kFaceS[face], dir), dot(kFaceU[face], dir)) / forward * 0.5 + 0.5;

    int faceRes = 2 * t;
    ivec2 cellOrigin = ivec2((face % 3) * faceRes, (face / 3) * faceRes);
    ivec2 faceTexel = clamp(ivec2(uv * float(faceRes)), ivec2(0), ivec2(faceRes - 1));
    ivec2 gridTexel = cellOrigin + faceTexel;

    vec4 capAlbedo = texelFetch(u_CaptureAlbedo, gridTexel, 0);
    vec4 capGeo = texelFetch(u_CaptureGeo, gridTexel, 0);

    if (capGeo.b < 0.0)
    {
        // Sky miss — nothing rasterized along this direction.
        o_HitAlbedo = vec4(0.0, 0.0, 0.0, DDGI_HIT_SKY);
        o_HitGeo = vec4(0.0, 0.0, -1.0, DDGI_HIT_SKY);
        return;
    }

    // Canonicalize the RGBA8-quantized capture flag back to the exact
    // DDGI_HIT_* values (0.5 is not representable in UNORM8).
    float flag = capAlbedo.a > 0.75 ? DDGI_HIT_FRONTFACE : DDGI_HIT_BACKFACE;

    o_HitAlbedo = vec4(capAlbedo.rgb, flag);
    o_HitGeo = vec4(capGeo.rg, capGeo.b, flag);
}
