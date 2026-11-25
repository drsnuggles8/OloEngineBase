#version 460 core

layout(local_size_x = 1) in;
#include "../include/HybridReflectionHit.glsl"

layout(std430, binding = 32) writeonly buffer Outputs
{
    vec4 u_Footprint[5];
};

vec4 Pack(mat2 footprint)
{
    return vec4(footprint[0], footprint[1]);
}

void main()
{
    const vec3 e1 = vec3(2.0, 0.0, 0.0);
    const vec3 e2 = vec3(0.0, 1.0, 0.0);
    const vec2 du = vec2(4.0, 0.0);
    const vec2 dv = vec2(0.0, 1.0);
    const vec3 normalIncidence = vec3(0.0, 0.0, -1.0);
    const vec3 grazing = vec3(1.0, 0.0, 0.0);
    u_Footprint[0] = Pack(oloReflectHitFootprint(e1, e2, du, dv, normalIncidence, 0.25));
    u_Footprint[1] = Pack(oloReflectHitFootprint(-e1, e2, du, dv, normalIncidence, 0.25));
    u_Footprint[2] = Pack(oloReflectHitFootprint(e1, e2, du, dv, grazing, 0.25));
    u_Footprint[3] = Pack(oloReflectHitFootprint(e1, 2.0 * e1, du, dv, normalIncidence, 0.25));
    u_Footprint[4] = Pack(oloReflectHitFootprint(e1, e2, du, dv, normalIncidence, uintBitsToFloat(0x7fc00000u)));
}
