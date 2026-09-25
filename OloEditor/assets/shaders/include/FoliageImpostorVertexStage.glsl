// FoliageImpostorVertexStage.glsl — the ONE vertex stage of the octahedral
// impostor card (issue #433), shared by:
//   * Foliage_Impostor.glsl         — forward: relights the card, writes SceneColor
//   * Foliage_Impostor_GBuffer.glsl — deferred: writes the G-Buffer MRT (#1225)
// Included whole after the includer's own `#type vertex` / `#version` lines.
// The two paths must place the card IDENTICALLY or it jumps in world space at
// the Forward/Deferred seam; keeping the whole stage here makes drift
// impossible instead of merely reviewable (the #953 placement fixes were
// exactly one vertex stage drifting from its sibling).
//
// The includer decides ONE thing before including: whether the consuming
// fragment reads the instance index (u_EntityID) —
//   forward  : `#define OLO_INSTANCE_NO_FORWARD 1` (nothing reads it; a
//              written-but-unconsumed output is a Vulkan validation warning)
//   deferred : no define (the fragment includes InstanceBlock.glsl for u_EntityID)
//
// Varying contract every consumer must declare:
//   location 0  vec3  v_CardWorld       location 5  float v_Rotation
//   location 1  vec3  v_PivotWorld      location 6  vec3  v_PrevCardWorld
//   location 2  float v_MeshCoverage    location 7  float v_Radius
//   location 4  float v_AlphaCutoff
// (location 3 was an unread tint; retired — the atlas albedo has the tint
// baked in, and re-applying it was the double-tint bug the sampling include's
// comment warns about. Location 2 was an unread uv and now carries the
// authored-mesh hand-over share, issue #1233.)
//
// Sibling includes resolve inside include/.

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5): V8 foliage two-stream pull — stream 0 = the
// 32-byte {vec3 position, vec3 normal, vec2 uv} geometry vertex on the
// engine-wide binding 57, stream 1 = FoliageRenderer's
// 48-byte per-instance VB {PositionScale, RotationHeight, ColorAlpha} on the
// reserved stream-1 binding 63, indexed by gl_InstanceIndex. Pulled locals
// under the attribute names in main() keep the body shared (Foliage_Instance
// carries the canonical comment).
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;
layout(std430, binding = 63) readonly buffer OloBonePull
{
    float v[];
} b_Instances;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;   // unread here — the card builds its own basis
layout(location = 2) in vec2 a_TexCoord;

layout(location = 3) in vec4 a_PositionScale;  // xyz = terrain-local pos, w = scale
layout(location = 4) in vec4 a_RotationHeight; // x = Y rotation (rad), y = height, z = fade, w = unused
layout(location = 5) in vec4 a_ColorAlpha;     // rgb = tint, a = alpha cutoff
#endif

// The shared camera block (include/CameraCommon.glsl), identical in every
// stage of every program that includes this — GL links a program only if
// its stages agree on the block — and carrying the forward screen-space AO
// lane (issue #1452).
#include "CameraCommon.glsl"

// Foliage uploads ONE shared InstanceData entry for all N pulled instances
// (see FoliageRenderer::Render) — per-instance data rides the 48-byte
// instance stream instead. OLO_INSTANCE_SINGLE keeps the include from
// indexing that one entry by gl_InstanceIndex (GL: garbage read, Vulkan:
// device-losing page fault at high instance counts).
#define OLO_INSTANCE_SINGLE 1
#include "InstanceBlock_Vertex.glsl"

#include "FoliageParams.glsl"

#include "FoliageInstanceGeometry.glsl"
#include "FoliageWind.glsl"

layout(location = 0) out vec3 v_CardWorld;  // this fragment's card world position
layout(location = 1) out vec3 v_PivotWorld; // card centre (world)
layout(location = 4) out float v_AlphaCutoff;
layout(location = 5) out float v_Rotation;  // instance Y rotation
#ifndef OLO_FOLIAGE_SHADOW
layout(location = 6) out vec3 v_PrevCardWorld;
layout(location = 8) out float v_WindDisplacement;
#endif
layout(location = 7) out float v_Radius;    // WORLD-space card radius (object radius * scale)
// This plant's authored-mesh share (issue #1233). The impostor is the FAR side
// of the hand-over, so it keeps the pixels the near mesh does not.
layout(location = 2) out float v_MeshCoverage;
// (this plant's own draw, its density fade) — issue #1237. One varying rather
// than two because location 3 is the only free slot in this stage's set, and
// the two are always read together: the seed decorrelates the hand-over and
// thinning dither, the fade is the per-instance thinning alpha the card must
// dissolve by. The flat-card path carries the fade on the instance lane
// instead; the impostor stage never read that lane, so it is carried here.
layout(location = 3) out vec2 v_LodSeedFade;

void main()
{
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 8;
    vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
    vec2 a_TexCoord = vec2(b_Vertices.v[vertBase + 6], b_Vertices.v[vertBase + 7]);
    int instBase = gl_InstanceIndex * 12;
    vec4 a_PositionScale = vec4(b_Instances.v[instBase + 0], b_Instances.v[instBase + 1],
                                b_Instances.v[instBase + 2], b_Instances.v[instBase + 3]);
    vec4 a_RotationHeight = vec4(b_Instances.v[instBase + 4], b_Instances.v[instBase + 5],
                                 b_Instances.v[instBase + 6], b_Instances.v[instBase + 7]);
    vec4 a_ColorAlpha = vec4(b_Instances.v[instBase + 8], b_Instances.v[instBase + 9],
                             b_Instances.v[instBase + 10], b_Instances.v[instBase + 11]);
#endif
    OLO_INSTANCE_FORWARD();

    float scale = a_PositionScale.w;
    float rotation = a_RotationHeight.x;
    float height = a_RotationHeight.y;

    // World-space card radius. u_ImpostorParams1.y is the OBJECT-space radius the
    // bake framed the mesh with, and the meshes are authored unit-height
    // (pine.obj spans y in [0,1], radius 0.560) — so the per-instance world
    // height has to come back in here, exactly as the near path applies it
    // (`localPos.y *= height * scale` in Foliage_Instance.glsl). This used to
    // read only `scale`, drawing a 9-16 m pine as a ~1.5 m card: an ~8x shrink
    // the moment an instance crossed ImpostorStartDistance (issue #953).
    //
    // UNIFORM, not anisotropic. Matching the near quad's aspect exactly would
    // mean stretching the card 1:16, which renders the baked pine as a needle —
    // and the near quad is a deliberately different thing anyway (a tufted
    // billboard; the impostor is what "gives the tree line a 3D silhouette from
    // any azimuth at range", issue #433). Scaling uniformly puts the drawn tree
    // at exactly `height * scale` tall, so nothing pops vertically across the
    // transition, and leaves it its own proportions.
    // ── LOD transition + density (issue #1237) ──────────────────────────────
    //
    // Evaluated here, BEFORE the card is sized, and folded into `scale` rather
    // than into `radius`: `radius` and the card's vertical anchor below are
    // both built from `height * scale`, so compensating only the radius would
    // grow the card while leaving its centre where a smaller plant's was — the
    // tree sinking into the ground as the layer thins. One multiply on the lane
    // they share is what keeps a grown plant standing on its own base.
    //
    // `instWorld` is the pivot and is computed below from u_Model alone, so it
    // is hoisted here; nothing above this point depends on it.
    vec3 instWorldPivot = (u_Model * vec4(a_PositionScale.xyz, 1.0)).xyz;
    vec3 instWorldPrev = (u_PrevModel * vec4(a_PositionScale.xyz, 1.0)).xyz;
    float lodDist = distance(instWorldPivot, u_MeshViewPos.xyz);
    float lodPrevDist = distance(instWorldPrev, u_PrevMeshViewPos.xyz);
    float instanceSeed = foliageLodInstanceHash(a_PositionScale.xyz);
    float densityAlpha = foliageDensityAlphaAt(u_LodTransition0, u_LodTransition1, instanceSeed, lodDist);
    float lodScale = foliageDensityScaleAt(u_LodTransition0, u_LodTransition1, lodDist);
    // The previous frame's compensation, so the growth reaches this card's
    // motion vector too — see the twin note in FoliageInstanceVertexStage.glsl.
    float lodScalePrev = foliageDensityScaleAt(u_LodTransition0, u_LodTransition1, lodPrevDist);
    scale *= lodScale;
    // The impostor stage never read the instance fade lane, so the thinning
    // alpha is carried explicitly alongside the seed the dither needs. The
    // layer's own fade rides with it so the card honours both.
    v_LodSeedFade = vec2(instanceSeed, densityAlpha * a_RotationHeight.z);

    float radius = u_ImpostorParams1.y * height * scale;

    // Instance pivot. Foliage's per-instance positions are TERRAIN-LOCAL (x/z in
    // [0, WorldSize], y the raw sampled height), so they only become world
    // positions after the owning terrain's transform — which DrawFoliageLayer
    // uploads as the single u_Model entry, already made render-relative by
    // UploadModelInstance. Foliage_Instance.glsl has always multiplied through
    // it; this stage did not, and subtracted the render origin directly instead
    // on the belief that a_PositionScale was absolute world (issue #953). It is
    // not: no island sits at the origin, so every impostor card rendered at its
    // island's LOCAL coordinates — all six islands' pines piled into one heap
    // over open water near (0,0,0), hanging above anything the terrain can
    // reach, which from the boat reads as a swarm of dark specks in the sky.
    //
    // The OLO_INSTANCE_SINGLE define above is what makes u_Model safe here: it
    // pins the read to instances[0] rather than indexing by gl_InstanceIndex,
    // which is the out-of-bounds hazard the old comment was really about (issue
    // #433). The two got conflated, and the transform was dropped with them.
    vec3 instWorld = instWorldPivot;

    // Authored-mesh hand-over (issue #1233), decided per INSTANCE from the
    // render-relative pivot exactly as the flat card and the shadow pass decide
    // it. Without this a layer that has BOTH an authored mesh and an impostor
    // draws the pine and a card of that pine on top of each other up close.
    //
    // Decorrelated per plant and hysteretic since #1237, from the same hash and
    // the same parameters those stages use — the impostor is one rung of the
    // same ladder, and a stage that sized its own band would leave a stretch
    // where both rungs are opaque.
    v_MeshCoverage = foliageMeshCoverageLod(lodDist, lodPrevDist, u_MeshParams.y, u_MeshParams.z,
                                            instanceSeed, foliageLodSpread(u_LodTransition1),
                                            foliageLodHysteresis(u_LodTransition1));
    // Anchor the card on the MESH CENTRE, because that is what the bake framed:
    // ImpostorBaker centres each tile on the source mesh's bounding-box centre
    // and spans +-u_ImpostorParams1.y around it, so the card's centre has to
    // land on that same point in world space or the tree floats inside its own
    // card. The meshes are authored base-at-origin (pine.obj spans y in [0,1])
    // and the near path already relies on that (`localPos.y *= height * scale`
    // in Foliage_Instance.glsl), so the centre is half the drawn height up.
    //
    // This offset by `radius` instead, which was indistinguishable while radius
    // was the UNIT-mesh radius (~0.5 m). The moment radius became
    // R0 * height * scale (6-12 m for Drift's pines) the same line lifted the
    // card centre — and the tree drawn around it — metres into the air: trees in
    // the sky. The card's BOTTOM stayed on the terrain, so the card extent
    // looked right and only its contents floated, which is what made this read
    // as a placement bug rather than an anchoring one.
    vec3 cardCenter = instWorld + vec3(0.0, 0.5 * height * scale, 0.0);

    // Far field retains coherent whole-card trunk/branch motion. Fine leaf
    // flutter is baked away; the near mesh remains its detailed consumer.
    float influence = dot(u_WindWeights.xyz, vec3(1.0)) > 0.0 ? 0.5 : 0.15;
    FoliageDeformation sway = foliageDeform(vec3(0.0), vec3(0.0, influence, 0.0), a_PositionScale.xyz, a_RotationHeight.w);
    vec3 cardCenterCur = cardCenter + mat3(u_Model) * sway.Current;
    vec3 prevInstWorld = (u_PrevModel * vec4(a_PositionScale.xyz, 1.0)).xyz;
    vec3 cardCenterPrev = prevInstWorld +
                          vec3(0.0, 0.5 * height * scale * (lodScale > 0.0 ? lodScalePrev / lodScale : 1.0), 0.0) +
                          mat3(u_PrevModel) * sway.Previous;

    // Camera-facing basis. u_CameraPosition is treated in the same space as the
    // render-relative pivot (renderOrigin ~ 0 for authored scenes) — matches the
    // existing foliage distance/fade convention.
    #ifdef OLO_FOLIAGE_SHADOW
    vec3 toCam = u_MeshViewPos.xyz - cardCenterCur;
#else
    vec3 toCam = u_CameraPosition - cardCenterCur;
#endif
    vec2 offset = (a_TexCoord - 0.5) * (2.0 * radius);
    vec3 cardWorld = foliageImpostorPoint(cardCenterCur, cardCenterCur + toCam, offset);
    // Carry the previous main eye explicitly. VP alone cannot recover an
    // orthographic eye; a current-eye fallback loses camera-facing history.
    // At the PREVIOUS frame's size: both the card's half-extent and the pivot
    // offset that rides on it scale with the compensation, so a card that is
    // growing reports the motion it actually has.
    float scaleRatioPrev = lodScale > 0.0 ? (lodScalePrev / lodScale) : 1.0;
    vec3 cardWorldPrev = foliageImpostorPoint(cardCenterPrev, u_PrevMeshViewPos.xyz, offset * scaleRatioPrev);

    v_CardWorld = cardWorld;
    #ifndef OLO_FOLIAGE_SHADOW
    v_PrevCardWorld = cardWorldPrev;
    v_WindDisplacement = clamp(length(sway.Current) / max(abs(u_WindStrength) * 2.5, 1e-5), 0.0, 1.0);
#endif
    v_PivotWorld = cardCenterCur;
    v_AlphaCutoff = a_ColorAlpha.a;
    v_Rotation = rotation;
    v_Radius = radius; // world-space half-size, needed by the fragment's virtual-plane UV

    gl_Position = u_ViewProjection * vec4(cardWorld, 1.0);

    // A plant the density LOD has thinned all the way out costs nothing past
    // here (issue #1237) — the same collapse idiom the flat-card and mesh
    // stage uses, and for the same reason: on the UNCOMPACTED path there is no
    // cull to have dropped the row, and a fully transparent card still pays
    // the atlas fetches and the parallax march.
    if (densityAlpha <= 0.0)
    {
        gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
    }
}
