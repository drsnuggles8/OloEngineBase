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
layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    // Previous-frame VP for scene FB RT3 velocity. Wind displacement is
    // time-varying and not reprojected; camera + per-object motion only.
    mat4 u_PrevViewProjection;
    vec3 u_RenderOrigin; // camera-relative render origin (issue #429)
    float _padding1;
};

// Model UBO (binding 3)
// Foliage uploads ONE shared InstanceData entry for all N pulled instances
// (see FoliageRenderer::Render) — per-instance data rides the 48-byte
// instance stream instead. OLO_INSTANCE_SINGLE keeps the include from
// indexing that one entry by gl_InstanceIndex (GL: garbage read, Vulkan:
// device-losing page fault at high instance counts).
#define OLO_INSTANCE_SINGLE 1
#include "InstanceBlock_Vertex.glsl"

// Foliage UBO (binding 12)
layout(std140, binding = 12) uniform FoliageParams
{
    float u_Time;
    float u_WindStrength;
    float u_WindSpeed;
    float u_ViewDistance;
    float u_FadeStart;
    float u_AlphaCutoff;
    float u_PrevTime;       // Previous-frame time for wind velocity reprojection
    float _foliagePad1;
    vec3  u_FoliageBaseColor;
    float _foliagePad2;
    vec4 _foliageImpostorParams0; // consumed by the impostor card only
    vec4 _foliageImpostorParams1; // consumed by the impostor card only
    // x = this draw is the authored mesh (1) or the flat card (0);
    // yz = the layer's mesh-to-card hand-over band (issue #1233).
    vec4 u_MeshParams;
    // xyz = the view position the hand-over is measured from, in the same
    // render-relative space as the instance pivots. NOT u_CameraPosition: the
    // shadow pass's camera is the light. See ShaderBindingLayout::FoliageUBO.
    vec4 u_MeshViewPos;
    // Leaf material (issue #1234) — see ShaderBindingLayout::FoliageUBO. The
    // block is declared identically in every stage of every foliage program:
    // std140 blocks must match across the stages of one program, so a lane
    // appended to one declaration and not the others is a LINK failure, not a
    // wrong pixel.
    vec4 u_LeafSurface;   // x=roughness y=normalStrength z=thicknessScale w=mapFlags
    vec4 u_LeafTransmit;  // rgb=tint*strength w=strength (0 == not a leaf material)
    vec4 u_LeafLobe;      // x=distortion y=power z=wrap w=environment scale
    vec4 u_LeafIds;       // x = leaf-profile slot for the deferred lighting pass
};

// Wind field (optional — provides direction-aware wind when enabled)
#include "WindSampling.glsl"
#include "FoliageInstanceGeometry.glsl"

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

    mat3 rotY = foliageInstanceRotation(rotation);
    vec3 rotatedPos = rotY * foliageInstanceLocalPos(a_Position, scale, height, isAuthoredMesh);

    // The mesh's scaling is uniform, so it leaves normals unchanged; the card's
    // is not, but its only normal is +Y — the scaling axis itself — which a
    // non-uniform scale also leaves alone. So one rotation serves both, and the
    // card's normal comes out as the vec3(0, 1, 0) this stage used to hard-code.
    vec3 rotatedNormal = rotY * a_Normal;

    // Wind animation — direction-aware when WindSystem is enabled,
    // otherwise falls back to legacy sine-wave model.
    // a_Position.y is 0 at the base and 1 at the tip for BOTH shapes: the card
    // is a unit quad and plant meshes are authored base-at-origin unit-height
    // (the convention the impostor bake already assumes; FoliageRenderer warns
    // when a mesh breaks it).
    float windInfluence = a_Position.y;

    // Compute both current and previous rotated tip positions so the fragment
    // stage can emit a per-fragment motion vector that captures the wind sway
    // itself (not just camera/rigid motion).
    vec3 rotatedPosPrev = rotatedPos;

    if (windEnabled())
    {
        // Sample wind field at blade root world position
        // Camera-relative (issue #429): u_Model is render-relative, so add the
        // render origin back — the wind field is anchored in absolute world.
        vec3 bladeWorldPos = (u_Model * vec4(a_PositionScale.xyz, 1.0)).xyz + u_RenderOrigin;
        vec3 bladeWorldPosPrev = (u_PrevModel * vec4(a_PositionScale.xyz, 1.0)).xyz + u_RenderOrigin;
        vec3 windVel = analyticalWind(bladeWorldPos); // Fast analytical path for vertex shader
        vec3 windVelPrev = analyticalWindAtTime(bladeWorldPosPrev, windPrevTime());
        // Displace blade tip along wind direction, scaled by per-layer strength
        rotatedPos.xyz     += windVel     * u_WindStrength * windInfluence * 0.1;
        rotatedPosPrev.xyz += windVelPrev * u_WindStrength * windInfluence * 0.1;
    }
    else
    {
        // Legacy sine-wave wind
        rotatedPos     += foliageLegacyWindOffset(a_PositionScale.xz, u_Time,     u_WindSpeed, u_WindStrength, windInfluence);
        rotatedPosPrev += foliageLegacyWindOffset(a_PositionScale.xz, u_PrevTime, u_WindSpeed, u_WindStrength, windInfluence);
    }

    // World position
    vec3 instancePos = a_PositionScale.xyz;
    vec3 worldPos     = (u_Model     * vec4(instancePos + rotatedPos,     1.0)).xyz;
    vec3 worldPosPrev = (u_PrevModel * vec4(instancePos + rotatedPosPrev, 1.0)).xyz;

    // The mesh/card hand-over is decided PER INSTANCE, from the plant's pivot,
    // not per fragment from its surface: a per-fragment distance puts the trunk
    // of one pine on the mesh side and its canopy on the card side, and the
    // plant tears in half across the band.
    vec3 pivotRenderRel = (u_Model * vec4(instancePos, 1.0)).xyz;
    v_MeshCoverage = foliageMeshCoverage(distance(pivotRenderRel, u_MeshViewPos.xyz),
                                         u_MeshParams.y, u_MeshParams.z);

    v_WorldPos = worldPos;
    v_PrevWorldPos = worldPosPrev;
    v_Normal = normalize(mat3(u_Normal) * rotatedNormal);
    v_TexCoord = a_TexCoord;
    v_Color = a_ColorAlpha.rgb;
    v_AlphaCutoff = a_ColorAlpha.a;
    v_Fade = fade;

    gl_Position = u_ViewProjection * vec4(worldPos, 1.0);

    // Whole instances this draw does not own cost nothing past here. An
    // authored tree is thousands of vertices per instance, so collapsing it
    // behind the near plane rather than letting the fragment stage discard it
    // is the difference between the mesh path costing at its hand-over distance
    // and costing at the layer's full view distance.
    if (isAuthoredMesh ? (v_MeshCoverage <= 0.0) : (v_MeshCoverage >= 1.0))
    {
        gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
    }
}
