// =============================================================================
// WaterDisturbanceCommon.glsl — the GLSL half of the water-disturbance encoding
// contract (issue #967).
//
// GLSL twin of OloEngine/src/OloEngine/Renderer/Water/WaterDisturbanceField.h.
// That header is the source of truth; read its comment block before changing
// anything here. The rules it states and this file must honour:
//
//   * the field is anchored at the WORLD ORIGIN, so it never slides with mesh
//     vertices, tessellation level or the water surface's own transform;
//   * storage is TOROIDAL, so `%` (which truncates toward zero on both sides)
//     is never used bare — see waterDisturbanceWrapIndex below;
//   * sampling is uv = absoluteWorldXZ * invFieldExtent with REPEAT wrap, which
//     is exact with NO half-texel correction term.
//
// BOTH halves of the field live here on purpose — the writer
// (compute/WaterDisturbance_Update.comp) and the reader (Water.glsl) include
// this same file, so there is exactly ONE GLSL text for the C++ header to be
// checked against rather than two that can drift apart unnoticed.
// =============================================================================

#ifndef WATER_DISTURBANCE_COMMON_GLSL
#define WATER_DISTURBANCE_COMMON_GLSL

// Euclidean modulo. Mirrors OloEngine::Math::WrapIndex.
//
// A bare `%` on a lattice coordinate maps a NEGATIVE coordinate to a NEGATIVE
// index, which does not error — it mirrors the field about the world origin.
// See docs/agent-rules/ddgi-probe-cascades-and-sparsity.md §2.
int waterDisturbanceWrapIndex(int value, int modulus)
{
    int m = max(modulus, 1);
    int r = value % m;
    return (r < 0) ? (r + m) : r;
}

// The unique lattice coordinate congruent to `storage` (mod resolution) that
// lies inside [latticeMin, latticeMin + resolution).
// Mirrors WaterDisturbance::LatticeForStorage.
ivec2 waterDisturbanceLatticeForStorage(ivec2 storage, ivec2 latticeMin, int resolution)
{
    return ivec2(latticeMin.x + waterDisturbanceWrapIndex(storage.x - latticeMin.x, resolution),
                 latticeMin.y + waterDisturbanceWrapIndex(storage.y - latticeMin.y, resolution));
}

// True when `lattice` lies inside the window with lower corner `latticeMin`.
// Mirrors WaterDisturbance::WindowContains.
bool waterDisturbanceWindowContains(ivec2 lattice, ivec2 latticeMin, int resolution)
{
    return all(greaterThanEqual(lattice, latticeMin)) &&
           all(lessThan(lattice, latticeMin + ivec2(resolution)));
}

// Falloff of one capsule splat (p0 -> p1, radius, softness) at world XZ.
// Mirrors WaterDisturbance::SplatWeight — including the capsule, which is what
// stops a fast hull leaving a dotted trail across a dropped frame.
float waterDisturbanceSplatWeight(vec2 worldXZ, vec2 p0, vec2 p1, float radius, float softness)
{
    float safeRadius = max(radius, 1e-4);
    vec2 seg = p1 - p0;
    float segLenSq = dot(seg, seg);
    float t = (segLenSq > 1e-12) ? clamp(dot(worldXZ - p0, seg) / segLenSq, 0.0, 1.0) : 0.0;
    vec2 d = worldXZ - (p0 + seg * t);
    float normalized = length(d) / safeRadius;
    if (normalized >= 1.0)
        return 0.0;
    return pow(1.0 - normalized, max(softness, 1e-4));
}

// UV for a REPEAT-wrapped sample of the field at ABSOLUTE world XZ.
// Mirrors WaterDisturbance::FieldUV.
//
// `absoluteWorldXZ` must be absolute, i.e. `v_WorldPos.xz + u_RenderOrigin.xz`
// under camera-relative rendering (issue #429). Passing the camera-relative
// position instead makes the whole field drift with the render origin, which
// looks like the wake sliding under the boat — and only once the camera has
// travelled far enough for the origin to rebase, so it survives a short test.
vec2 waterDisturbanceFieldUV(vec2 absoluteWorldXZ, float invFieldExtent)
{
    return absoluteWorldXZ * invFieldExtent;
}

// Fade toward the window boundary. Mirrors WaterDisturbance::EdgeFade.
//
// Two jobs, both load-bearing: it hides the torus seam (where the neighbouring
// stored texel belongs to the opposite edge of the window), and it stops the
// field ending in a hard square across open ocean.
float waterDisturbanceEdgeFade(vec2 absoluteWorldXZ, vec2 windowCentreXZ,
                               float invFieldExtent, float edgeFadeStart)
{
    vec2 d = abs(absoluteWorldXZ - windowCentreXZ) * invFieldExtent;
    float m = max(d.x, d.y); // 0 at the centre, 0.5 at the boundary
    return 1.0 - clamp((m - edgeFadeStart) / max(0.5 - edgeFadeStart, 1e-4), 0.0, 1.0);
}

// Sample the disturbance field, edge fade included, at absolute world XZ.
//
// `params`  = (windowCentreX, windowCentreZ, invFieldExtent, intensity)
// `params2` = (fadeNearMetres, fadeFarMetres, edgeFadeStart, unused)
//
// `intensity <= 0` is the disabled state — there is no separate enable flag, so
// a frame that never ran the compute cannot leave a stale field showing.
//
// Sampled with an explicit LOD 0. The field is a low-frequency, world-anchored
// signal at 0.5 m/texel that is fully mipped by nothing (it is stored without
// mips), and an implicit-LOD fetch inside the caller's branch would take a
// derivative in non-quad-uniform control flow — undefined, and exactly the
// defect docs/agent-rules/water-shading-nyquist.md §2 was written about.
float sampleWaterDisturbance(vec2 absoluteWorldXZ, sampler2D field,
                             vec4 params, vec4 params2)
{
    if (params.w <= 0.0)
        return 0.0;

    float raw = textureLod(field, waterDisturbanceFieldUV(absoluteWorldXZ, params.z), 0.0).r;
    float fade = waterDisturbanceEdgeFade(absoluteWorldXZ, params.xy, params.z, params2.z);
    return clamp(raw, 0.0, 1.0) * fade;
}

#endif // WATER_DISTURBANCE_COMMON_GLSL
