// FoliageInstanceGeometry.glsl — where a foliage instance's geometry goes, and
// which of a layer's two shapes owns a given pixel (issue #1233).
//
// Pure functions over their arguments: no uniform blocks, no varyings, no
// stage assumptions. That is what lets EVERY foliage vertex stage call them —
// the shared beauty/G-Buffer stage (FoliageInstanceVertexStage.glsl) and the
// shadow depth stage, which keeps its own compact stage because it runs under
// the shadow camera UBO and no wind field. Placement living here is what makes
// the fourth acceptance criterion structural: a shadow cast by a quad while
// the lit plant is a pine passes every CPU test and reads downstream as a
// completely different bug, and the only durable fix is that no stage gets to
// decide where the geometry goes.
//
// A layer with a loadable MeshPath emits TWO draws over the SAME instance
// stream: its real geometry up close and its flat card (or octahedral
// impostor) beyond. Both carry the IDENTICAL hand-over band, differing only in
// which side they are, and each keeps exactly the pixels the other discards.
// Two independently-written fades would leave a stretch where the pine and its
// card are both opaque — which in the G-Buffer (no blending, a hard alpha cut)
// reads as the tree fighting a billboard of itself.

#ifndef OLO_FOLIAGE_INSTANCE_GEOMETRY_GLSL
#define OLO_FOLIAGE_INSTANCE_GEOMETRY_GLSL

// Place a geometry vertex in instance-local space.
//
// The CARD is anisotropic: x/z by `scale`, y by `height * scale`. That is
// deliberate — it is a tufted billboard, and the per-layer Min/MaxHeight range
// is what gives a grass field its variation.
//
// The authored MESH is scaled UNIFORMLY by `height * scale`, which is exactly
// what FoliageImpostorVertexStage.glsl does to the impostor card
// (`radius = meshRadius * height * scale`). Matching it is the whole point: a
// mesh stretched to the card's 1:16 aspect renders a pine as a needle, and the
// near geometry and the far impostor would be different trees. Uniform puts
// the drawn plant at exactly `height * scale` tall in both, so nothing changes
// size across the hand-over.
vec3 foliageInstanceLocalPos(vec3 vertexPos, float scale, float height, bool isAuthoredMesh)
{
    vec3 cardLocal = vec3(vertexPos.x * scale, vertexPos.y * height * scale, vertexPos.z * scale);
    vec3 meshLocal = vertexPos * (height * scale);
    return isAuthoredMesh ? meshLocal : cardLocal;
}

// Per-instance Y rotation, as a matrix so a NORMAL goes through the same
// rotation as the position. Columns are the images of the basis vectors:
// x' = x*cos - z*sin, z' = x*sin + z*cos.
mat3 foliageInstanceRotation(float rotation)
{
    float cosR = cos(rotation);
    float sinR = sin(rotation);
    return mat3(vec3(cosR, 0.0, sinR),
                vec3(0.0, 1.0, 0.0),
                vec3(-sinR, 0.0, cosR));
}

// The legacy sine-wave wind offset, when no wind field is enabled. Shared so
// the shadow stage sways a plant exactly as the beauty stage does — a shadow
// that swings out of phase with its plant is the same class of desync.
vec3 foliageLegacyWindOffset(vec2 instanceXZ, float time, float windSpeed, float windStrength,
                             float windInfluence)
{
    float phase = (instanceXZ.x + instanceXZ.y) * 0.1 + time * windSpeed;
    float wind = sin(phase) * cos(phase * 0.7 + 1.3) * windStrength * windInfluence;
    return vec3(wind, 0.0, wind * 0.5);
}

// The authored mesh's share of this plant at `dist`. 1 up close, 0 past the
// band, and 0 everywhere for a layer with no mesh (bandEnd == 0), which is what
// makes the card path bit-identical to its pre-#1233 self.
//
// `dist` is measured from u_MeshViewPos, not u_CameraPosition: the shadow pass
// renders the same plants under a LIGHT's camera, so a hand-over keyed on the
// camera position would pick a different shape for the shadow than for the lit
// plant. u_MeshViewPos carries the main view's position, render-relative, into
// every pass.
//
// Nor the render origin, which was the first attempt: the origin is only the
// camera while camera-relative rendering is ON, and with it off it is world
// zero — which makes the hand-over a ring around the world origin rather than a
// radius around the viewer. It rendered as "the mesh appears in one corner of
// the terrain", and the near/far A/B is what caught it.
float foliageMeshCoverage(float dist, float bandStart, float bandEnd)
{
    if (bandEnd <= 0.0)
        return 0.0;
    // A zero-width or inverted band would make smoothstep undefined; widen it
    // to a millimetre so the hand-over degenerates to a hard switch instead.
    float end = max(bandEnd, bandStart + 1e-3);
    return 1.0 - smoothstep(bandStart, end, dist);
}

// Per-pixel dither threshold. A plain hash, not blue noise: the band is crossed
// over many frames and the pattern is never still, so the cheap one is not
// distinguishable here. Quantised to whole pixels so the pattern does not
// shimmer within a pixel under MSAA sample positions.
float foliageLodDither(vec2 fragCoord)
{
    vec2 p = floor(fragCoord);
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

// Does THIS draw own the pixel? The mesh takes the fraction `coverage` of
// pixels and the card takes the rest, so the two calls partition them exactly.
bool foliageLodKeep(bool isAuthoredMesh, float coverage, vec2 fragCoord)
{
    if (coverage <= 0.0)
        return !isAuthoredMesh; // past the band (or no mesh at all): card only
    if (coverage >= 1.0)
        return isAuthoredMesh; // inside the band's near end: mesh only

    return isAuthoredMesh ? (foliageLodDither(fragCoord) < coverage)
                          : (foliageLodDither(fragCoord) >= coverage);
}

#endif // OLO_FOLIAGE_INSTANCE_GEOMETRY_GLSL

// Camera-facing square, shared by current/previous impostor evaluations.
vec3 foliageImpostorPoint(vec3 centre, vec3 eye, vec2 offset)
{
    vec3 z = normalize(eye - centre);
    vec3 up = abs(z.y) > 0.999 ? vec3(0.0, 0.0, -1.0) : vec3(0.0, 1.0, 0.0);
    vec3 x = normalize(cross(up, z));
    return centre + x * offset.x + normalize(cross(z, x)) * offset.y;
}
