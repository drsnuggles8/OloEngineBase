// GBufferNormalEncode.glsl — the G-Buffer's octahedral world-normal encoder.
// Every G-Buffer writer must store normals THIS way and every reader decodes
// the inverse (GBufferRaySurface.glsl, SpatialDenoise.glsl, the SSGI/SSR/
// contact-shadow and ray-traced passes all say "matches octEncodeGB() in
// PBR_GBuffer.glsl"). Nine writers carry a hand-copied definition today; this
// include exists so a new writer does not add a tenth. Byte-identical to the
// PBR_GBuffer.glsl body. (Migrating the existing copies is a separate,
// mechanical change — keep this one definition authoritative when you do.)
#ifndef GBUFFER_NORMAL_ENCODE_GLSL
#define GBUFFER_NORMAL_ENCODE_GLSL

vec2 octEncodeGB(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0,
                                        n.y >= 0.0 ? 1.0 : -1.0);
    return n.xy;
}

#endif // GBUFFER_NORMAL_ENCODE_GLSL
