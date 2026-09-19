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

#include "FoliageLodTransition.glsl"

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

// The same hand-over, DECORRELATED PER INSTANCE and hysteretic (issue #1237).
//
// The band keeps its authored WIDTH and slides bodily by this plant's own
// offset, so every plant still cross-fades over exactly the interval the
// author sized — only the interval's position differs, and its mean over the
// layer is the authored one. Sliding only one edge would quietly change the
// hand-over's duration per plant, which is a different feature.
//
// `receding` is (dist >= prevDist) from the CURRENT and PREVIOUS main eye. It
// moves both edges outward while the viewer retreats and inward while it
// approaches, so a plant holds the representation it already has. See the C++
// header for what that does and does not guarantee.
//
// EVERY caller must pass the same arguments for a given plant — the mesh draw,
// the card draw, the impostor draw and the shadow depth stage — or the two
// sides of the partition disagree and the plant either doubles or disappears
// across its band. That is why this takes the layer parameters rather than
// reading a UBO: the shadow stage does not have the same one bound.
float foliageMeshCoverageLod(float dist, float prevDist, float bandStart, float bandEnd,
                             float offset01, float spread, float hysteresis)
{
    if (bandEnd <= 0.0)
        return 0.0;
    // ONE offset for both edges — that is what makes the band slide rather
    // than stretch. A per-edge factor scales the band's WIDTH by (1 +- h) too,
    // so a plant's hand-over would take longer walking away than walking in.
    float shift = foliageLodHysteresisOffset(bandStart, dist >= prevDist, hysteresis);
    float start = foliageLodTransitionDistance(bandStart, offset01, spread, shift);
    float end = foliageLodTransitionDistance(bandEnd, offset01, spread, shift);
    return foliageMeshCoverage(dist, start, end);
}

// Per-pixel dither threshold.
//
// TWO IMPLEMENTATIONS, and the switch between them is the whole reason there
// are two. The first is the `fract(sin(dot(...)))` hash this started as
// (issue #1233), kept EXACTLY as it was: it is what every layer authored
// before #1237 partitions its hand-over with, and replacing it outright moved
// the committed FoliageWind golden by an SSIM of 0.011 for a layer that had
// opted into nothing. A transition-smoothing feature must not change the image
// of a scene that did not ask for it.
//
// The second is INTERLEAVED GRADIENT NOISE, decorrelated per plant, and it is
// what `LodStochasticCoverage` buys. Both are cheap; the difference is the
// DISTRIBUTION. A sine hash clusters — over any small pixel neighbourhood its
// values are far from uniform — so a coverage of 0.5 does not hand the mesh
// half the pixels of a plant that only covers forty of them, it hands it a
// lumpy two thirds, and across a hand-over that reads as the plant changing
// DENSITY as well as shape. Interleaved gradient noise is uniform over a 3x3
// neighbourhood by construction, which is the property the partition needs.
//
// `instanceSeed` is this plant's own draw (foliageLodInstanceHash). Without it
// two overlapping plants mid-hand-over at the same pixel make the SAME
// keep/discard decision, so their bands correlate and the thinning shows up as
// a moire between them rather than as noise. It must be a per-INSTANCE
// constant, never per-fragment: the mesh draw and the card draw have to agree
// pixel for pixel or neither covers it.
//
// Both quantise to whole pixels so the pattern does not shimmer within a pixel
// under MSAA sample positions.
float foliageLodDither(vec2 fragCoord, float instanceSeed, bool decorrelate)
{
    vec2 p = floor(fragCoord);
    if (!decorrelate)
        return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
    // The IGN constants (Jimenez 2014), with the instance's draw folded into
    // the phase the way a frame index normally is.
    return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y + instanceSeed));
}

// Does THIS draw own the pixel? The mesh takes the fraction `coverage` of
// pixels and the card takes the rest, so the two calls partition them exactly.
bool foliageLodKeep(bool isAuthoredMesh, float coverage, vec2 fragCoord, float instanceSeed, bool decorrelate)
{
    if (coverage <= 0.0)
        return !isAuthoredMesh; // past the band (or no mesh at all): card only
    if (coverage >= 1.0)
        return isAuthoredMesh; // inside the band's near end: mesh only

    float d = foliageLodDither(fragCoord, instanceSeed, decorrelate);
    return isAuthoredMesh ? (d < coverage) : (d >= coverage);
}

// The per-instance thinning fade (issue #1237) resolved to a keep/discard in a
// pass that has NO ALPHA to blend — the G-Buffer, and the shadow depth pass.
//
// Stochastic for the same reason the hand-over above is: a hard alpha cut-off
// would delete a thinning plant the instant its fade dropped below the
// threshold, which is the pop the density feature exists to remove, moved one
// step down the ladder. Dithering it means a plant at fade 0.4 keeps four
// tenths of its pixels and shrinks away instead of vanishing.
//
// Only ever reached with the decorrelated dither — it is called behind
// foliageStochasticCoverage, which is the same switch — and the seed is offset
// by a constant so a plant that is simultaneously mid-hand-over AND
// mid-thinning does not make the two decisions with the same number, which
// would couple them and delete exactly the pixels the other draw was counting
// on.
bool foliageDensityKeep(float densityAlpha, vec2 fragCoord, float instanceSeed)
{
    if (densityAlpha >= 1.0)
        return true;
    if (densityAlpha <= 0.0)
        return false;
    return foliageLodDither(fragCoord, fract(instanceSeed + 0.5), true) < densityAlpha;
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
