#ifdef OLO_VSM_FAMILY
#include "VirtualShadowFamilyVertex.glsl"
#endif
#ifdef OLO_VULKAN
// #691 (ADR 0011 §5): V8 foliage two-stream pull — stream 0 = the
// 32-byte {vec3 position, vec3 normal, vec2 uv} geometry vertex on the
// engine-wide binding 57 (the card quad, or the layer's authored plant mesh),
// stream 1 = FoliageRenderer's 48-byte per-instance VB {PositionScale,
// RotationHeight, ColorAlpha} on the reserved stream-1 binding 63, indexed by
// gl_InstanceIndex. Pulled locals under the attribute names in main() keep the
// body shared (FoliageInstanceVertexStage.glsl carries the canonical comment).
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

// Camera UBO (binding 0) — contains light VP during shadow pass
layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

// Model UBO (binding 3)
// Foliage uploads ONE shared InstanceData entry for all N pulled instances
// (see FoliageRenderer::Render) — per-instance data rides the 48-byte
// instance stream instead. OLO_INSTANCE_SINGLE keeps the include from
// indexing that one entry by gl_InstanceIndex (GL: garbage read, Vulkan:
// device-losing page fault at high instance counts).
#define OLO_INSTANCE_SINGLE 1
// This shader's consuming stage never reads v_InstanceIndex — declare no
// varying (a written-but-unconsumed output is a per-pipeline Vulkan
// validation interface warning).
#define OLO_INSTANCE_NO_FORWARD 1
#include "InstanceBlock_Vertex.glsl"

// Foliage UBO (binding 12)
#include "FoliageParams.glsl"

#include "FoliageInstanceGeometry.glsl"
#include "FoliageWind.glsl"

layout(location = 0) out vec2 v_TexCoord;
layout(location = 1) out float v_AlphaCutoff;
layout(location = 2) out float v_MeshCoverage;
// (this plant's own draw, its density fade) — issue #1237, the twin of the
// beauty stage's v_InstanceSeed / v_Fade pair. The caster set has to thin with
// the drawn set or a thinned-out plant keeps casting a shadow with nothing
// above it.
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
    bool isAuthoredMesh = u_MeshParams.x > 0.5;

    // LOD transition + density (issue #1237), from the same pivot, the same
    // hash and the same UBO parameters the beauty stage uses — the caster and
    // the drawn plant are the same plant at the same size or the shadow is of
    // something that is not there.
    vec3 lodPivot = (instances[0].Transform * vec4(a_PositionScale.xyz, 1.0)).xyz;
    vec3 lodPivotPrev = (instances[0].PrevTransform * vec4(a_PositionScale.xyz, 1.0)).xyz;
    float lodDist = distance(lodPivot, u_MeshViewPos.xyz);
    float lodPrevDist = distance(lodPivotPrev, u_PrevMeshViewPos.xyz);
    float instanceSeed = foliageLodInstanceHash(a_PositionScale.xyz);
    float densityAlpha = foliageDensityAlphaAt(u_LodTransition0, u_LodTransition1, instanceSeed, lodDist);
    scale *= foliageDensityScaleAt(u_LodTransition0, u_LodTransition1, lodDist);
    v_LodSeedFade = vec2(instanceSeed, densityAlpha * a_RotationHeight.z);

    // A mesh layer's far card faces the MAIN view and is scaled like the mesh,
    // exactly as the beauty stage places it (#1533), so it casts the shape the
    // lit frame draws.
    bool meshLayerCard = foliageIsMeshLayerCard(isAuthoredMesh, u_MeshParams);
    float facing = meshLayerCard
                       ? foliageCardFacingYaw(lodPivot, u_MeshViewPos.xyz, instances[0].Transform, rotation)
                       : rotation;
    vec3 rotatedPos = foliageInstanceRotation(facing) *
                      foliageInstanceLocalPos(a_Position, scale, height, isAuthoredMesh || meshLayerCard);

    rotatedPos = foliageDeform(rotatedPos, a_Position, a_PositionScale.xyz, a_RotationHeight.w,
                               instances[0].Transform, instances[0].PrevTransform).Current;

    vec3 instancePos = a_PositionScale.xyz;
    vec3 worldPos = (instances[0].Transform * vec4(instancePos + rotatedPos, 1.0)).xyz;

    // Same per-instance hand-over as the beauty pass, measured from the render
    // origin rather than the camera for exactly the reason this stage exists:
    // u_CameraPosition is the LIGHT's here, and a hand-over keyed on it would
    // shadow the card where the lit frame drew the mesh.
    v_MeshCoverage = foliageMeshCoverageLod(lodDist, lodPrevDist, u_MeshParams.y, u_MeshParams.z,
                                            instanceSeed, foliageLodSpread(u_LodTransition1),
                                            foliageLodHysteresis(u_LodTransition1));

    v_TexCoord = a_TexCoord;
    v_AlphaCutoff = a_ColorAlpha.a;

    gl_Position = u_ViewProjection * vec4(worldPos, 1.0);
#ifdef OLO_VSM_FAMILY
    gl_Position = vsmFamilyPosition(gl_Position);
#endif

    // Collapse whole instances this draw does not own — see the same guard in
    // FoliageInstanceVertexStage.glsl.
    if ((isAuthoredMesh ? (v_MeshCoverage <= 0.0) : (v_MeshCoverage >= 1.0)) || densityAlpha <= 0.0)
    {
        gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
    }
}
