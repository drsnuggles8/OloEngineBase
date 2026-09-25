// FoliageInstanceVertexStage.glsl — the ONE vertex stage of the foliage
// instance draw, shared by:
//   * Foliage_Instance.glsl         — forward: lights the plant, writes SceneColor
//   * Foliage_Instance_GBuffer.glsl — deferred: writes the G-Buffer MRT
// Included whole after the includer's own `#type vertex` / `#version` lines,
// exactly like FoliageImpostorVertexStage.glsl next to it. The two files used
// to carry a copy each; the copies had already drifted in comments, and the
// #953 placement bugs were exactly one foliage vertex stage drifting from its
// sibling.
//
// Foliage_Depth.glsl keeps a vertex stage of its own because it runs under the
// SHADOW camera UBO and with no wind field bound — but every placement
// decision it makes comes from FoliageInstanceGeometry.glsl, the same
// functions called here, so it cannot put a plant somewhere this stage does
// not (issue #1233, fourth criterion).
//
// It draws BOTH shapes a layer can have: the flat card it has always drawn,
// and the layer's authored plant mesh up close. One stage rather than a card
// variant and a mesh variant per pass.
//
// Geometry stream (binding 57 / attribute locations 0-2) is the engine's
// 32-byte Vertex {vec3 position, vec3 normal, vec2 texcoord} for both shapes.
// The card supplies a +Y normal, which is the constant this stage used to
// hard-code, so the card renders as it always did.
//
// The includer decides ONE thing before including: whether the consuming
// fragment reads the instance index (u_EntityID) —
//   forward  : `#define OLO_INSTANCE_NO_FORWARD 1` (nothing reads it; a
//              written-but-unconsumed output is a Vulkan validation warning)
//   deferred : no define (the fragment includes InstanceBlock.glsl for u_EntityID)
//
// Varying contract every consumer must declare:
//   location 0 vec3  v_WorldPos       location 4 float v_AlphaCutoff
//   location 1 vec3  v_Normal         location 5 float v_Fade
//   location 2 vec2  v_TexCoord       location 6 vec3  v_PrevWorldPos
//   location 3 vec3  v_Color          location 7 float v_MeshCoverage

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5): V8 foliage two-stream pull. Stream 0 is the 32-byte
// {vec3 position, vec3 normal, vec2 uv} geometry vertex on the engine-wide
// binding 57 — the card quad, or the layer's authored plant mesh; stream 1 is
// FoliageRenderer's 48-byte per-instance VB {PositionScale, RotationHeight,
// ColorAlpha} riding binding 63 (the reserved stream-1 pull binding — bone
// influences are just its first tenant), indexed by gl_InstanceIndex. Pulled
// locals under the attribute names in main() keep the body shared; the GL
// attribute branch below is untouched.
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
// Per-vertex attributes (card quad or authored mesh — same layout)
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;

// Per-instance attributes
layout(location = 3) in vec4 a_PositionScale;  // xyz = terrain-local pos, w = scale
layout(location = 4) in vec4 a_RotationHeight; // x = Y rotation (rad), y = height, z = fade, w = unused
layout(location = 5) in vec4 a_ColorAlpha;     // rgb = tint, a = alpha cutoff
#endif

// Camera UBO (binding 0)
// The shared camera block (include/CameraCommon.glsl), identical in every
// stage of every program that includes this — GL links a program only if
// its stages agree on the block — and carrying the forward screen-space AO
// lane (issue #1452).
#include "CameraCommon.glsl"

// Model UBO (binding 3)
// Foliage uploads ONE shared InstanceData entry for all N pulled instances
// (see FoliageRenderer::Render) — per-instance data rides the 48-byte
// instance stream instead. OLO_INSTANCE_SINGLE keeps the include from
// indexing that one entry by gl_InstanceIndex (GL: garbage read, Vulkan:
// device-losing page fault at high instance counts).
#define OLO_INSTANCE_SINGLE 1
#include "InstanceBlock_Vertex.glsl"

// Foliage UBO (binding 12)
#include "FoliageParams.glsl"

// Wind field (optional — provides direction-aware wind when enabled)
#include "FoliageInstanceGeometry.glsl"
#include "FoliageWind.glsl"

// Outputs
layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord;
layout(location = 3) out vec3 v_Color;
layout(location = 4) out float v_AlphaCutoff;
layout(location = 5) out float v_Fade;
// Previous-frame world position (wind + model reprojection) for RT3 velocity.
layout(location = 6) out vec3 v_PrevWorldPos;
// This plant's authored-mesh share, decided per INSTANCE in this stage so the
// fragment cannot re-derive it and disagree (issue #1233).
layout(location = 7) out float v_MeshCoverage;
// This plant's own draw in [0, 1) — foliageLodInstanceHash of its
// terrain-local pivot (issue #1237). Carried rather than re-hashed in the
// fragment stage for the same reason v_MeshCoverage is: it decorrelates the
// hand-over dither PER PLANT, and a fragment that hashed its own interpolated
// position would get a different number for every pixel, which is a
// per-fragment coin flip rather than a partition.
layout(location = 8) out float v_InstanceSeed;

void main()
{
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 8;
    vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
    vec3 a_Normal = vec3(b_Vertices.v[vertBase + 3], b_Vertices.v[vertBase + 4], b_Vertices.v[vertBase + 5]);
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
    float fade = a_RotationHeight.z;
    bool isAuthoredMesh = u_MeshParams.x > 0.5;

    // ── LOD transition + density (issue #1237) ──────────────────────────────
    //
    // Everything here is decided from the plant's PIVOT, before the geometry
    // is placed, because the density compensation multiplies `scale` — and a
    // per-vertex distance would grow one end of a pine more than the other.
    // The pivot is also what the mesh/card hand-over has always used, for the
    // reason spelled out below it.
    vec3 lodInstancePos = a_PositionScale.xyz;
    vec3 lodPivot = (u_Model * vec4(lodInstancePos, 1.0)).xyz;
    vec3 lodPivotPrev = (u_PrevModel * vec4(lodInstancePos, 1.0)).xyz;
    float lodDist = distance(lodPivot, u_MeshViewPos.xyz);
    float lodPrevDist = distance(lodPivotPrev, u_PrevMeshViewPos.xyz);
    float instanceSeed = foliageLodInstanceHash(lodInstancePos);

    // The thinning fade rides the EXISTING per-instance fade lane, so every
    // consumer downstream (v_Fade in both fragment programs, the alpha the
    // G-Buffer resolves) picks it up with no new plumbing — and a layer with
    // the feature off multiplies by exactly 1.
    float densityAlpha = foliageDensityAlphaAt(u_LodTransition0, u_LodTransition1, instanceSeed, lodDist);
    fade *= densityAlpha;
    // Coverage preservation: the survivors grow by the factor that keeps the
    // layer covering what it covered unthinned. UNIFORM in `scale`, which is
    // the same lane the card's anisotropy and the mesh's uniform scaling both
    // read, so a grown plant is the same plant — never a stretched one.
    float lodScale = foliageDensityScaleAt(u_LodTransition0, u_LodTransition1, lodDist);
    // The PREVIOUS frame's compensation, from the previous eye. Carried so the
    // growth reaches the motion vector: a plant that is being grown is moving,
    // and a velocity computed at this frame's size for both endpoints reports
    // zero for that component, which is a history TAA would reproject wrongly.
    // The per-frame delta is small — the compensation ramps over the whole
    // density band — so this is a correctness fix rather than a visible one,
    // and it is cheaper than the alternative of zeroing the velocity, which
    // throws away the camera and wind motion that ARE valid.
    float lodScalePrev = foliageDensityScaleAt(u_LodTransition0, u_LodTransition1, lodPrevDist);
    scale *= lodScale;

    mat3 rotY = foliageInstanceRotation(rotation);
    vec3 rotatedPos = rotY * foliageInstanceLocalPos(a_Position, scale, height, isAuthoredMesh);

    // The mesh's scaling is uniform, so it leaves normals unchanged; the card's
    // is not, but its only normal is +Y — the scaling axis itself — which a
    // non-uniform scale also leaves alone. So one rotation serves both, and the
    // card's normal comes out as the vec3(0, 1, 0) this stage used to hard-code.
    vec3 rotatedNormal = rotY * a_Normal;

    FoliageDeformation deformation = foliageDeform(rotatedPos, a_Position, a_PositionScale.xyz, a_RotationHeight.w);
    vec3 rotatedPosPrev = deformation.Previous;
    rotatedPos = deformation.Current;
    // Re-place the PREVIOUS frame's vertex at the previous frame's size. The
    // deformation is evaluated once, at the current size, because it is a
    // function of the plant's pivot rather than of its vertices; the size
    // difference is added afterwards as the offset it is. A no-op whenever the
    // density LOD is off, where both compensations are exactly 1.
    if (lodScalePrev != lodScale)
    {
        float baseScale = a_PositionScale.w;
        rotatedPosPrev += rotY * (foliageInstanceLocalPos(a_Position, baseScale * lodScalePrev, height,
                                                          isAuthoredMesh) -
                                  foliageInstanceLocalPos(a_Position, scale, height, isAuthoredMesh));
    }
    vec3 displacement = deformation.Current - rotY * foliageInstanceLocalPos(a_Position, scale, height, isAuthoredMesh);
    // Transport the normal whenever the plant is actually deformed. Interaction
    // bending (issue #1238) reaches layers that never opted into hierarchical
    // wind, and shading a flattened blade with its upright normal is the same
    // defect the cofactor transport was added for.
    if (dot(u_WindWeights.xyz, vec3(1.0)) > 0.0 ||
        (u_InteractionParams.x >= 0.5 && u_InteractionParams.y > 0.0))
    {
        vec3 size = isAuthoredMesh ? vec3(height * scale) : vec3(scale, height * scale, scale);
        mat3 shapeJacobian = rotY * mat3(vec3(size.x, 0.0, 0.0), vec3(0.0, size.y, 0.0), vec3(0.0, 0.0, size.z));
        rotatedNormal = foliageWindNormal(a_Normal, a_Position, a_PositionScale.xyz, a_RotationHeight.w,
                                          shapeJacobian, displacement);
    }

    // World position
    vec3 instancePos = a_PositionScale.xyz;
    vec3 worldPos     = (u_Model * vec4(instancePos + rotatedPos,     1.0)).xyz;
    vec3 worldPosPrev = (u_PrevModel * vec4(instancePos + rotatedPosPrev, 1.0)).xyz;

    // The mesh/card hand-over is decided PER INSTANCE, from the plant's pivot,
    // not per fragment from its surface: a per-fragment distance puts the trunk
    // of one pine on the mesh side and its canopy on the card side, and the
    // plant tears in half across the band.
    v_MeshCoverage = foliageMeshCoverageLod(lodDist, lodPrevDist, u_MeshParams.y, u_MeshParams.z,
                                            instanceSeed, foliageLodSpread(u_LodTransition1),
                                            foliageLodHysteresis(u_LodTransition1));
    v_InstanceSeed = instanceSeed;

    v_WorldPos = worldPos;
    v_PrevWorldPos = worldPosPrev;
    v_Normal = normalize(mat3(u_Normal) * rotatedNormal);
    v_TexCoord = a_TexCoord;
    v_Color = u_WindWeights.w > 0.5 ? vec3(clamp(length(displacement) / max(abs(u_WindStrength) * 2.5, 1e-5), 0.0, 1.0), 0.0, 1.0) : a_ColorAlpha.rgb;
    v_AlphaCutoff = a_ColorAlpha.a;
    v_Fade = fade;

    gl_Position = u_ViewProjection * vec4(worldPos, 1.0);

    // Whole instances this draw does not own cost nothing past here. An
    // authored tree is thousands of vertices per instance, so collapsing it
    // behind the near plane rather than letting the fragment stage discard it
    // is the difference between the mesh path costing at its hand-over distance
    // and costing at the layer's full view distance.
    //
    // A plant the density LOD has thinned all the way out is collapsed by the
    // same idiom (issue #1237). The GPU cull refuses to append such a row at
    // all, so on the indirect path this branch is unreachable; it is what
    // makes the UNCOMPACTED path — a layer the cull could not run for, which
    // FoliageRenderer reports rather than hides — cost the same as the culled
    // one instead of shading thousands of fully transparent plants.
    if ((isAuthoredMesh ? (v_MeshCoverage <= 0.0) : (v_MeshCoverage >= 1.0)) || densityAlpha <= 0.0)
    {
        gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
    }
}
