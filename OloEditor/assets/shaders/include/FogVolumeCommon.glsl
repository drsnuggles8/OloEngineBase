// =============================================================================
// FogVolumeCommon.glsl — Local fog volume evaluation functions
//
// Provides:
//   - UBO declaration for fog volume data (binding 20)
//   - Shape-specific signed distance functions (SDF)
//   - Point containment with smooth boundary falloff
//   - Multi-volume evaluation with blending
//
// Usage:
//   #include "include/FogVolumeCommon.glsl"
//   FogVolumeResult fogResult = evaluateFogVolumesAtPoint(worldPos);
// =============================================================================

#ifndef FOG_VOLUME_COMMON_GLSL
#define FOG_VOLUME_COMMON_GLSL

// Shape type constants (match FogVolumeShape enum on CPU side)
#define FOG_VOLUME_SHAPE_BOX       0
#define FOG_VOLUME_SHAPE_SPHERE    1
#define FOG_VOLUME_SHAPE_CYLINDER  2
// OpenVDB-imported density grid (#724) — handled separately from the SDF
// shapes below (evaluateFogVolumesAtPointVDB), since it needs a sampler3D
// the plain evaluateFogVolumesAtPoint() callers don't declare.
#define FOG_VOLUME_SHAPE_TEXTURE3D 3

#define MAX_FOG_VOLUMES 16

struct FogVolumeDataGPU
{
    mat4 WorldToLocal;
    vec4 ColorAndDensity;   // rgb = color, a = density
    vec4 ShapeAndFalloff;   // x = shape, y = falloffDist, z = blendWeight, w = pad
    vec4 Extents;           // xyz = half-extents/radius, w = pad
};

// Local fog volumes UBO (std140, binding 20)
layout(std140, binding = 20) uniform FogVolumesData
{
    FogVolumeDataGPU u_FogVolumes[MAX_FOG_VOLUMES];
    ivec4 u_FogVolumeCount; // x = active count
};

struct FogVolumeResult
{
    float density;
    vec3  color;
};

// ---------------------------------------------------------------------------
// Signed distance functions in local space (negative = inside)
// ---------------------------------------------------------------------------

// Box SDF: extents = half-size per axis
float sdfBox(vec3 localPos, vec3 extents)
{
    vec3 d = abs(localPos) - extents;
    return length(max(d, 0.0)) + min(max(d.x, max(d.y, d.z)), 0.0);
}

// Sphere SDF: uses extents.x as radius
float sdfSphere(vec3 localPos, vec3 extents)
{
    return length(localPos) - extents.x;
}

// Cylinder SDF: radius = extents.x, half-height = extents.y, aligned along Y
float sdfCylinder(vec3 localPos, vec3 extents)
{
    float radius = extents.x;
    float halfH = extents.y;
    vec2 d = vec2(length(localPos.xz) - radius, abs(localPos.y) - halfH);
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
}

// ---------------------------------------------------------------------------
// Evaluate SDF for a given shape type
// ---------------------------------------------------------------------------
float evaluateVolumeSDF(vec3 localPos, int shape, vec3 extents)
{
    if (shape == FOG_VOLUME_SHAPE_SPHERE)
        return sdfSphere(localPos, extents);
    else if (shape == FOG_VOLUME_SHAPE_CYLINDER)
        return sdfCylinder(localPos, extents);
    else // BOX (default)
        return sdfBox(localPos, extents);
}

// ---------------------------------------------------------------------------
// Evaluate all active fog volumes at a world-space point.
// Returns blended density and color from all contributing volumes.
// ---------------------------------------------------------------------------
FogVolumeResult evaluateFogVolumesAtPoint(vec3 worldPos)
{
    FogVolumeResult result;
    result.density = 0.0;
    result.color = vec3(0.0);

    int count = u_FogVolumeCount.x;
    if (count <= 0)
        return result;

    float totalWeight = 0.0;

    for (int i = 0; i < count && i < MAX_FOG_VOLUMES; ++i)
    {
        int shape = int(u_FogVolumes[i].ShapeAndFalloff.x + 0.5);
        // This overload has no sampler3D to read (see evaluateFogVolumesAtPointVDB
        // above for the one that does) — a Texture3D-shaped volume contributes
        // nothing here rather than falling into evaluateVolumeSDF's BOX default
        // and rendering as a uniform-density box, which would silently draw the
        // wrong shape. Every caller of THIS overload (the analytical fog
        // fallback, VolumetricShadow_Generate.comp) only ever sees Texture3D
        // volumes when froxel volumetric fog is off — use
        // evaluateFogVolumesAtPointVDB for a caller that must see them.
        if (shape == FOG_VOLUME_SHAPE_TEXTURE3D)
            continue;

        // Transform world position to volume local space
        vec3 localPos = (u_FogVolumes[i].WorldToLocal * vec4(worldPos, 1.0)).xyz;

        vec3 extents = u_FogVolumes[i].Extents.xyz;
        float falloff = u_FogVolumes[i].ShapeAndFalloff.y;
        float blendWeight = u_FogVolumes[i].ShapeAndFalloff.z;
        float density = u_FogVolumes[i].ColorAndDensity.a;
        vec3 color = u_FogVolumes[i].ColorAndDensity.rgb;

        // Compute signed distance (negative = inside)
        float sdf = evaluateVolumeSDF(localPos, shape, extents);

        // Smooth falloff at boundary: 1.0 at center, 0.0 beyond falloff distance
        float influence = 1.0 - smoothstep(-falloff, 0.0, sdf);

        if (influence < 0.001)
            continue;

        float volumeDensity = density * influence * blendWeight;

        // Weighted accumulation for color blending
        result.density += volumeDensity;
        totalWeight += volumeDensity;
        result.color += color * volumeDensity;
    }

    // Normalize color by total weight
    if (totalWeight > 0.001)
        result.color /= totalWeight;

    return result;
}

// ---------------------------------------------------------------------------
// Same evaluation as evaluateFogVolumesAtPoint(), plus FOG_VOLUME_SHAPE_
// TEXTURE3D support (#724) — a separate function (rather than an optional
// argument, which GLSL has no syntax for) so callers that never bind a
// density volume — everything except FroxelFogScatter.comp — keep using the
// plain overload above without also having to declare `densityVolume`.
// `densityVolume` is bound at whatever unit the caller's shader chose (see
// VolumetricFogPass.cpp) — a 1x1x1 all-zero placeholder when no
// FogVolumeShape::Texture3D entity has a resolved volume this frame, so a
// Texture3D-shaped volume with no texture bound contributes zero density
// rather than needing a separate "is bound" branch here.
// ---------------------------------------------------------------------------
FogVolumeResult evaluateFogVolumesAtPointVDB(vec3 worldPos, sampler3D densityVolume)
{
    FogVolumeResult result;
    result.density = 0.0;
    result.color = vec3(0.0);

    int count = u_FogVolumeCount.x;
    if (count <= 0)
        return result;

    float totalWeight = 0.0;

    for (int i = 0; i < count && i < MAX_FOG_VOLUMES; ++i)
    {
        vec3 localPos = (u_FogVolumes[i].WorldToLocal * vec4(worldPos, 1.0)).xyz;

        int shape = int(u_FogVolumes[i].ShapeAndFalloff.x + 0.5);
        vec3 extents = u_FogVolumes[i].Extents.xyz;
        float blendWeight = u_FogVolumes[i].ShapeAndFalloff.z;
        float density = u_FogVolumes[i].ColorAndDensity.a;
        vec3 color = u_FogVolumes[i].ColorAndDensity.rgb;

        float volumeDensity;
        if (shape == FOG_VOLUME_SHAPE_TEXTURE3D)
        {
            // extents is the half-extent of the authored volume in local
            // space; map [-extents, extents] -> [0,1]^3 UVW. Hard-edged at
            // the box boundary (no falloff softening) — that boundary IS
            // the source .vdb's own active-region bounds, so softening it
            // would fade content the artist authored as solid.
            //
            // Floor the divisor: m_Extents carries no CPU-side clamp (unlike
            // Density/FalloffDistance/Priority — Box/Sphere/Cylinder's SDFs
            // all tolerate a zero extent harmlessly, this branch does not),
            // and a zero/near-zero axis here divides to NaN/Inf, which the
            // bounds check below does NOT catch (IEEE 754: every comparison
            // against NaN is false, so `any(lessThan(uvw, 0))` never fires)
            // — an unbounded, unclamped sample would then pollute this
            // volume's contribution, and via the froxel pass's temporal
            // history, following frames too.
            vec3 uvw = (localPos / max(extents, vec3(0.01))) * 0.5 + 0.5;
            if (any(lessThan(uvw, vec3(0.0))) || any(greaterThan(uvw, vec3(1.0))))
            {
                continue;
            }
            float sampled = texture(densityVolume, uvw).r;
            volumeDensity = density * sampled * blendWeight;
        }
        else
        {
            float falloff = u_FogVolumes[i].ShapeAndFalloff.y;
            float sdf = evaluateVolumeSDF(localPos, shape, extents);
            float influence = 1.0 - smoothstep(-falloff, 0.0, sdf);
            if (influence < 0.001)
            {
                continue;
            }
            volumeDensity = density * influence * blendWeight;
        }

        if (volumeDensity < 0.0001)
        {
            continue;
        }

        result.density += volumeDensity;
        totalWeight += volumeDensity;
        result.color += color * volumeDensity;
    }

    if (totalWeight > 0.001)
        result.color /= totalWeight;

    return result;
}

#endif // FOG_VOLUME_COMMON_GLSL
