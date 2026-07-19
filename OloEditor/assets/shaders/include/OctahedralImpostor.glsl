// =============================================================================
// OctahedralImpostor.glsl — octahedral atlas mapping, GPU mirror of the CPU
// library OloEngine/src/OloEngine/Renderer/Impostor/OctahedralImpostor.h.
// Object space, +Y up (foliage/tree trunk grows +Y). See issue #433.
// =============================================================================
#ifndef OCTAHEDRAL_IMPOSTOR_GLSL
#define OCTAHEDRAL_IMPOSTOR_GLSL

// Sign that never returns 0 (sign(0)==0 would zero a folded coordinate for an
// axis-aligned view direction). Matches SignNotZero in the CPU library.
float signNotZero(float v)
{
    return (v >= 0.0) ? 1.0 : -1.0;
}

// dir (unit, object space) -> octahedron square coord in [-1,1]^2 (full sphere).
vec2 OctaEncodeSphere(vec3 dir)
{
    float sum = abs(dir.x) + abs(dir.y) + abs(dir.z); // L1
    vec3 oct = (sum != 0.0) ? dir / sum : vec3(0.0, 1.0, 0.0);
    if (oct.y < 0.0)
    {
        vec2 a = abs(vec2(oct.x, oct.z));
        oct.x = signNotZero(oct.x) * (1.0 - a.y);
        oct.z = signNotZero(oct.z) * (1.0 - a.x);
    }
    return vec2(oct.x, oct.z);
}

vec3 OctaDecodeSphere(vec2 coord)
{
    vec3 p = vec3(coord.x, 0.0, coord.y);
    vec2 a = abs(vec2(p.x, p.z));
    p.y = 1.0 - a.x - a.y;
    if (p.y < 0.0)
    {
        float sx = (p.x >= 0.0) ? 1.0 : -1.0;
        float sz = (p.z >= 0.0) ? 1.0 : -1.0;
        p.x = sx * (1.0 - a.y);
        p.z = sz * (1.0 - a.x);
    }
    return normalize(p);
}

// Hemi-octahedron (upper hemisphere, y >= 0).
vec2 OctaEncodeHemi(vec3 dir)
{
    dir.y = max(dir.y, 0.001);
    dir = normalize(dir);
    vec3 octant = sign(dir);
    float sum = dot(dir, octant);
    vec3 oct = (sum != 0.0) ? dir / sum : vec3(0.0, 1.0, 0.0);
    return vec2(oct.x + oct.z, oct.z - oct.x);
}

vec3 OctaDecodeHemi(vec2 coord)
{
    vec3 p = vec3(0.5 * (coord.x - coord.y), 0.0, 0.5 * (coord.x + coord.y));
    vec2 a = abs(vec2(p.x, p.z));
    p.y = 1.0 - a.x - a.y;
    return normalize(p);
}

vec2 OctaEncode(vec3 dir, bool hemi)
{
    return hemi ? OctaEncodeHemi(dir) : OctaEncodeSphere(dir);
}

vec3 OctaDecode(vec2 coord, bool hemi)
{
    return hemi ? OctaDecodeHemi(coord) : OctaDecodeSphere(coord);
}

// View direction -> continuous grid coords in [0, N-1].
vec2 OctaDirToGrid(vec3 dir, float framesPerAxis, bool hemi)
{
    vec2 oct = OctaEncode(dir, hemi);
    vec2 uv01 = clamp(oct * 0.5 + 0.5, 0.0, 1.0);
    return uv01 * (framesPerAxis - 1.0);
}

// Integer frame -> the direction it was captured from.
vec3 OctaFrameToDir(vec2 frame, float framesPerAxis, bool hemi)
{
    vec2 uv01 = frame / (framesPerAxis - 1.0);
    vec2 coord = uv01 * 2.0 - 1.0;
    return OctaDecode(coord, hemi);
}

#endif // OCTAHEDRAL_IMPOSTOR_GLSL
