#ifndef FORWARD_SHADING_NORMAL_GLSL
#define FORWARD_SHADING_NORMAL_GLSL

// =============================================================================
// ForwardShadingNormal.glsl — the forward PBR surface normal, ONE definition
// shared by the colour pass and the depth-normal prepass (issue #1452).
//
// The prepass writes the view normal the screen-space AO passes read, before
// the colour pass runs; the colour pass then writes the same attachment again.
// If the two computed the normal differently, AO would be evaluated for one
// surface and applied to another — the mismatch shows at every normal-mapped
// edge. So both call the functions below, with the same inputs, in the same
// order, and scene attachment 2 is written with the same value twice.
//
// REQUIREMENTS, declared by the including stage before this file:
//   * include/PBRCommon.glsl (OLO_MAT_NORMAL, OLO_SKIN_MAT_NORMAL,
//     OLO_MATERIAL_KIND_SKIN);
//   * the PBRMaterialProperties block (u_UseNormalMap, u_NormalScale,
//     u_MaterialKind, u_SkinDetailStrength);
//   * u_NormalMap, as a sampler or as the bindless / heap accessor macro. It
//     is referenced by name, never passed: the Vulkan heap reader cannot pass
//     a sampler through a function argument (PBRCommon.glsl's OLO_MAT_* note).
// =============================================================================

// The normal-mapped shading normal: the vertex normal, then the authored normal
// map, with the skin pore band (issue #1243) when a skin material asks for it.
// Taken before the ocular surface tilts it (see the colour pass for why the
// variance filter must see this normal and not the tilted one).
vec3 oloForwardMappedNormal(vec3 vertexNormal, vec2 uv, vec3 worldPos)
{
    vec3 N = normalize(vertexNormal);
    if (u_UseNormalMap == 1)
    {
        // Branched on the strength, not merely on the kind: the second tap is a
        // real texture fetch, and a skin material whose author left the detail
        // fields at their neutral default must cost what it cost before.
        if (u_MaterialKind == OLO_MATERIAL_KIND_SKIN && u_SkinDetailStrength != 0.0)
            N = OLO_SKIN_MAT_NORMAL(u_NormalMap, uv, worldPos, vertexNormal, u_NormalScale, u_SkinDetailStrength);
        else
            N = OLO_MAT_NORMAL(u_NormalMap, uv, worldPos, vertexNormal, u_NormalScale);
    }
    return N;
}

// Octahedral encode: unit normal -> RG16F [-1,1]^2.
vec2 oloOctEncodeViewNormal(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return n.xy;
}

// What scene attachment 2 stores for a world-space normal: the octahedral view
// normal SSAO, GTAO and the sphere proxies read.
vec2 oloForwardViewNormalOutput(mat4 view, vec3 worldNormal)
{
    return oloOctEncodeViewNormal(normalize(mat3(view) * worldNormal));
}

#endif // FORWARD_SHADING_NORMAL_GLSL
