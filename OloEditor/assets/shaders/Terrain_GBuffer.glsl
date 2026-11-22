// OLO_NORMAL_MAP_TBN_EXEMPT: terrain is not an imported-mesh material. Its normals come from a
// splat/triplanar blend of a sampler2DArray over a heightfield whose tangent frame is ANALYTIC
// (derived from the height gradient / the triplanar axis), not from screen-space UV derivatives.
// PBRCommon's derivative TBN neither applies nor is shared with it. See RenderPathDrift.
// =============================================================================
// Terrain_GBuffer.glsl - Deferred G-Buffer variant of Terrain_PBR.glsl.
//
// VS → TCS → TES → FS pipeline identical to Terrain_PBR up through material
// sampling. Instead of evaluating lighting + shadows + IBL in the fragment,
// this variant writes the blended splatmap / triplanar material into the
// 4-RT G-Buffer with `emissive.a = 0.0` (lit flag) so
// `DeferredLightingPass` performs full PBR evaluation.
//
// **Snow handling:** snow_weight-modulated material blend (albedo →
// snow albedo, roughness → snow roughness, normal → perturbed snow normal).
// The subsurface-scatter pass used by the forward path is not available in
// deferred; snow is treated as a diffuse overlay for now.
//
// **Velocity:** terrain is assumed static (zero velocity written). A future
// dynamic-terrain pass could add per-entity `PrevTransform` + previous-frame
// heightmap sampling here.
// =============================================================================

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5, amendment (76)): vertex pull from the engine-wide
// binding 57. Draw site is the terrain patch VBO (TerrainChunk.cpp /
// TerrainVertex.h — 32 B: vec3 Position @0, vec2 TexCoord @12, vec3 Normal
// @20), so the stride is 8 floats but the FIELD ORDER differs from the
// engine `Vertex` (uv before normal). Only the vertex stage pulls; the
// tess control/eval stages read the vertex stage's outputs unchanged. The
// GL attribute branch below is untouched.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;
layout(location = 2) in vec3 a_Normal;
#endif

// GPU-driven LOD (issue #714). Inert when u_TerrainGpuDriven == 0, which is
// every chunk-geometry draw including the shadow casters.
#include "include/TerrainGpuDrivenVertex.glsl"

layout(location = 0) out vec3 v_Position;
layout(location = 1) out vec2 v_TexCoord;
layout(location = 2) out vec3 v_Normal;

void main()
{
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 8;
    vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
    vec2 a_TexCoord = vec2(b_Vertices.v[vertBase + 3], b_Vertices.v[vertBase + 4]);
    vec3 a_Normal = vec3(b_Vertices.v[vertBase + 5], b_Vertices.v[vertBase + 6], b_Vertices.v[vertBase + 7]);
#endif
    // Copies, because the GL branch's a_* are `in` variables.
    vec3 position = a_Position;
    vec2 texCoord = a_TexCoord;
    vec3 normal = a_Normal;
    oloTerrainApplyGpuDrivenNode(position, texCoord, normal);

    v_Position = position;
    v_TexCoord = texCoord;
    v_Normal = normal;
}

#type tess_control
#version 460 core

layout(vertices = 3) out;

layout(location = 0) in vec3 v_Position[];
layout(location = 1) in vec2 v_TexCoord[];
layout(location = 2) in vec3 v_Normal[];

layout(location = 0) out vec3 tc_Position[];
layout(location = 1) out vec2 tc_TexCoord[];
layout(location = 2) out vec3 tc_Normal[];

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    // Camera-relative (issue #429): full tail so u_RenderOrigin lands at its
    // std140 offset (272). Deferred terrain adds it back to reconstruct absolute
    // world position for the world-anchored triplanar/snow/brush patterns.
    mat4 u_PrevViewProjection;
    vec3 u_RenderOrigin;
    float _padding1;
};

#include "include/TerrainParamsBlock.glsl"

float calcTessLevel(vec3 p0, vec3 p1)
{
    vec3 mid = (p0 + p1) * 0.5;
    float dist = distance(mid, u_CameraPosition);
    float edgeLen = distance(p0, p1);
    // abs(): u_Projection[1][1] is NEGATIVE on Vulkan (the projection-seam y
    // flip, #691) — see Terrain_PBR.glsl's calcTessLevel.
    float projScale = abs(u_Projection[1][1]) * 0.5;
    float screenLen = (edgeLen * projScale) / max(dist, 0.001);
    return clamp(screenLen / 8.0, 1.0, 64.0);
}

void main()
{
    tc_Position[gl_InvocationID] = v_Position[gl_InvocationID];
    tc_TexCoord[gl_InvocationID] = v_TexCoord[gl_InvocationID];
    tc_Normal[gl_InvocationID]   = v_Normal[gl_InvocationID];

    if (gl_InvocationID == 0)
    {
        if (u_TessFactors2.w > 0.5)
        {
            gl_TessLevelInner[0] = u_TessFactors.x;
            gl_TessLevelOuter[0] = u_TessFactors.y;
            gl_TessLevelOuter[1] = u_TessFactors.z;
            gl_TessLevelOuter[2] = u_TessFactors.w;
        }
        else
        {
            float e0 = calcTessLevel(v_Position[1], v_Position[2]);
            float e1 = calcTessLevel(v_Position[2], v_Position[0]);
            float e2 = calcTessLevel(v_Position[0], v_Position[1]);

            gl_TessLevelInner[0] = (e0 + e1 + e2) / 3.0;
            gl_TessLevelOuter[0] = e0;
            gl_TessLevelOuter[1] = e1;
            gl_TessLevelOuter[2] = e2;
        }
    }
}

#type tess_evaluation
#version 460 core

layout(triangles, equal_spacing, ccw) in;

layout(location = 0) in vec3 tc_Position[];
layout(location = 1) in vec2 tc_TexCoord[];
layout(location = 2) in vec3 tc_Normal[];

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    // Camera-relative (issue #429): full tail so u_RenderOrigin lands at its
    // std140 offset (272). Deferred terrain adds it back to reconstruct absolute
    // world position for the world-anchored triplanar/snow/brush patterns.
    mat4 u_PrevViewProjection;
    vec3 u_RenderOrigin;
    float _padding1;
};

#include "include/InstanceBlock_Single.glsl"

#include "include/TerrainParamsBlock.glsl"

#include "include/BindlessHeap.glsl"
#ifdef OLO_BINDLESS
#define u_TerrainHeightmap OLO_HEAP_TEX_2D(23)  // TEX_TERRAIN_HEIGHTMAP
#define u_SnowDepthMap OLO_HEAP_TEX_2D(30)  // TEX_SNOW_DEPTH
#else
layout(binding = 23) uniform sampler2D u_TerrainHeightmap;
layout(binding = 30) uniform sampler2D u_SnowDepthMap;
#endif

#include "include/TerrainHeightSampling.glsl"
#include "include/TerrainDisplacedHeight.glsl"

layout(std140, binding = 16) uniform SnowAccumulationParams {
    mat4 u_ClipmapViewProj[3];
    vec4 u_ClipmapCenterAndExtent[3];
    vec4 u_AccumulationParams;
    vec4 u_DisplacementParams;
};

// Brush preview UBO (editor-only overlay). Read here and handed to the fragment
// stage below, as Terrain_PBR does (#1565).
layout(std140, binding = 11) uniform BrushPreview {
    vec4 u_BrushPosAndRadius;
    vec4 u_BrushParams;
};

layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord;

// The fragment stage's per-frame snow-clipmap and brush constants as flat
// varyings, the same four at the same locations as Terrain_PBR (#1565 — see
// the note there): one transport for both terrain colour programs, which
// share include/TerrainSnowWeight.glsl.
layout(location = 3) flat out vec4 v_SnowClip;          // xy = ring-0 centre (world XZ), z = ring-0 extent, w = max depth
layout(location = 4) flat out float v_SnowAccumulation; // u_DisplacementParams.z: > 0.5 when the clipmap is live
layout(location = 5) flat out vec4 v_BrushPosAndRadius;
layout(location = 6) flat out vec4 v_BrushParams;
// The instance's normal matrix, for the fragment stage's per-pixel normal
// (include/TerrainPixelNormal.glsl, issue #1721): that normal is computed in
// the tile's own frame and has to be turned into the world's. Flat for the
// reason the four above are: one value per draw, and no new block in a stage
// that has none to spare.
layout(location = 7) flat out vec3 v_NormalMatrixX;
layout(location = 8) flat out vec3 v_NormalMatrixY;
layout(location = 9) flat out vec3 v_NormalMatrixZ;

vec3 interpolate3(vec3 a, vec3 b, vec3 c)
{
    return gl_TessCoord.x * a + gl_TessCoord.y * b + gl_TessCoord.z * c;
}

vec2 interpolate2(vec2 a, vec2 b, vec2 c)
{
    return gl_TessCoord.x * a + gl_TessCoord.y * b + gl_TessCoord.z * c;
}

void main()
{
    vec3 pos = interpolate3(tc_Position[0], tc_Position[1], tc_Position[2]);
    vec2 uv  = interpolate2(tc_TexCoord[0], tc_TexCoord[1], tc_TexCoord[2]);
    vec3 nrm = normalize(interpolate3(tc_Normal[0], tc_Normal[1], tc_Normal[2]));

    float heightScale = u_WorldSizeAndHeightScale.z;
    float heightMip = oloTerrainHeightMip(tc_TexCoord[0], tc_TexCoord[1], tc_TexCoord[2]);
    float sampledHeight = oloTerrainFilteredHeight(uv, heightMip) * heightScale;
    pos.y = sampledHeight;
    float snowDisplacement = 0.0;

    if (u_DisplacementParams.z > 0.5)
    {
        vec2 clipCenter = u_ClipmapCenterAndExtent[0].xy;
        float clipExtent = u_ClipmapCenterAndExtent[0].z;
        vec3 worldP = (instances[0].Transform * vec4(pos, 1.0)).xyz + u_RenderOrigin; // camera-relative (issue #429)
        vec2 snowUV = (worldP.xz - clipCenter) / clipExtent + 0.5;
        if (snowUV.x >= 0.0 && snowUV.x <= 1.0 && snowUV.y >= 0.0 && snowUV.y <= 1.0)
        {
            float snowDepth = texture(u_SnowDepthMap, snowUV).r;
            snowDisplacement = snowDepth * u_DisplacementParams.x;
        }
    }

    float texelSize = u_TerrainParams.x * exp2(heightMip);
    float worldSizeX = u_WorldSizeAndHeightScale.x;
    float worldSizeZ = u_WorldSizeAndHeightScale.y;

    float hL = oloTerrainFilteredHeight(uv + vec2(-texelSize, 0.0), heightMip) * heightScale;
    float hR = oloTerrainFilteredHeight(uv + vec2( texelSize, 0.0), heightMip) * heightScale;
    float hD = oloTerrainFilteredHeight(uv + vec2(0.0, -texelSize), heightMip) * heightScale;
    float hU = oloTerrainFilteredHeight(uv + vec2(0.0,  texelSize), heightMip) * heightScale;

    float dX = (hR - hL) / (2.0 * texelSize * worldSizeX);
    float dZ = (hU - hD) / (2.0 * texelSize * worldSizeZ);
    nrm = normalize(vec3(-dX, 1.0, -dZ));

    float morphFactor = u_TessFactors2.y;
    float meshHeight = interpolate3(tc_Position[0], tc_Position[1], tc_Position[2]).y;
    vec3 meshNormal = normalize(interpolate3(tc_Normal[0], tc_Normal[1], tc_Normal[2]));
    pos.y = oloTerrainDisplacedHeight(sampledHeight, snowDisplacement, meshHeight, morphFactor);
    nrm = normalize(mix(nrm, meshNormal, morphFactor));

    vec4 worldPos = instances[0].Transform * vec4(pos, 1.0);
    v_WorldPos = worldPos.xyz;
    v_Normal = mat3(instances[0].Normal) * nrm;
    v_NormalMatrixX = instances[0].Normal[0].xyz;
    v_NormalMatrixY = instances[0].Normal[1].xyz;
    v_NormalMatrixZ = instances[0].Normal[2].xyz;
    v_TexCoord = uv;
    v_SnowClip = vec4(u_ClipmapCenterAndExtent[0].xyz, u_AccumulationParams.y);
    v_SnowAccumulation = u_DisplacementParams.z;
    v_BrushPosAndRadius = u_BrushPosAndRadius;
    v_BrushParams = u_BrushParams;

    gl_Position = u_ViewProjection * worldPos;
}

#type fragment
#version 460 core

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    // Camera-relative (issue #429): full tail so u_RenderOrigin lands at its
    // std140 offset (272). Deferred terrain adds it back to reconstruct absolute
    // world position for the world-anchored triplanar/snow/brush patterns.
    mat4 u_PrevViewProjection;
    vec3 u_RenderOrigin;
    float _padding1;
};

// Terrain is single-instance; match the tess_eval include so v_InstanceIndex
// isn't declared as fragment input without a producer (link error).
#include "include/InstanceBlock_Single.glsl"

#include "include/TerrainParamsBlock.glsl"

// The brush preview (binding 11) and snow accumulation (binding 16) values
// arrive as the flat varyings below (locations 3-6), not as blocks (#1565).

#include "include/BindlessHeap.glsl"
#ifdef OLO_BINDLESS
#define u_TerrainSplatmap0 OLO_HEAP_TEX_2D(24)  // TEX_TERRAIN_SPLATMAP
#define u_TerrainAlbedoArray OLO_HEAP_TEX_2D_ARRAY(25)  // TEX_TERRAIN_ALBEDO_ARRAY
#define u_TerrainNormalArray OLO_HEAP_TEX_2D_ARRAY(26)  // TEX_TERRAIN_NORMAL_ARRAY
#define u_TerrainARMArray OLO_HEAP_TEX_2D_ARRAY(27)  // TEX_TERRAIN_ARM_ARRAY
#define u_TerrainSplatmap1 OLO_HEAP_TEX_2D(28)  // TEX_TERRAIN_SPLATMAP_1
#define u_SnowDepthMapFS OLO_HEAP_TEX_2D(30)  // TEX_SNOW_DEPTH
#define u_TerrainMacroMap OLO_HEAP_TEX_2D(0)  // TEX_DIFFUSE, see below
#define u_TerrainHeightmap OLO_HEAP_TEX_2D(23)  // TEX_TERRAIN_HEIGHTMAP, for the per-pixel normal
#else
layout(binding = 23) uniform sampler2D u_TerrainHeightmap;     // for the per-pixel normal
layout(binding = 24) uniform sampler2D u_TerrainSplatmap0;
layout(binding = 25) uniform sampler2DArray u_TerrainAlbedoArray;
layout(binding = 26) uniform sampler2DArray u_TerrainNormalArray;
layout(binding = 27) uniform sampler2DArray u_TerrainARMArray;
layout(binding = 28) uniform sampler2D u_TerrainSplatmap1;
layout(binding = 30) uniform sampler2D u_SnowDepthMapFS;
layout(binding = 0) uniform sampler2D u_TerrainMacroMap;
#endif
// The macro map (issue #1721) sits on TEX_DIFFUSE: terrain is not drawn
// through the material system, so no material texture can occupy that slot
// under it — the same reasoning that puts the foliage leaf-thickness map on
// TEX_METALLIC — and it costs no new engine texture slot.

// Snow as a material layer (issue #1451) and the terrain's cover — the SAME
// functions Terrain_PBR calls, so Forward and Deferred carry the same snow.
// SnowLayer.glsl brings the Snow UBO (binding 13).
#include "include/SnowLayer.glsl"
#include "include/TerrainSnowWeight.glsl"

layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord;
layout(location = 3) flat in vec4 v_SnowClip;
layout(location = 4) flat in float v_SnowAccumulation;
layout(location = 5) flat in vec4 v_BrushPosAndRadius;
layout(location = 6) flat in vec4 v_BrushParams;
layout(location = 7) flat in vec3 v_NormalMatrixX;
layout(location = 8) flat in vec3 v_NormalMatrixY;
layout(location = 9) flat in vec3 v_NormalMatrixZ;

layout(location = 0) out vec4 o_GBufferAlbedo;
layout(location = 1) out vec4 o_GBufferNormal;
layout(location = 2) out vec4 o_GBufferEmissive;
layout(location = 3) out vec4 o_GBufferVelocity;
layout(location = 4) out int  o_GBufferEntityID;
// Baked lightmap irradiance target (G-Buffer RT5, issue #865). This shader
// draws no lightmapped receiver, but an MRT output it never writes is
// UNDEFINED in that attachment, and RT5's .a is a coverage flag — undefined
// there reads as "this pixel has baked GI" and the deferred ambient ladder
// shades it from whatever the target happened to hold. Writing vec4(0) is the
// explicit "no baked GI here" every non-lightmapped G-Buffer writer owes the
// lighting pass.
layout(location = 5) out vec4 o_GBufferBakedGI;

vec2 octEncodeGB(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0,
                                        n.y >= 0.0 ? 1.0 : -1.0);
    return n.xy;
}

// =============================================================================
// SPLATMAP HELPERS — the blend math is SHARED with Terrain_PBR.glsl and with
// compute/TerrainVTTileBake.comp via include/TerrainLayerBlend.glsl (issue
// #715). It used to be a hand-duplicated copy of the forward path's, which the
// old header here said out loud; the virtual-texture bake made it a third copy,
// which is one too many to keep in step by hand.
// =============================================================================

// The SAMPLING is shared with Terrain_PBR.glsl too, in
// include/TerrainLayerSampling.glsl (issue #1721).
#include "include/TerrainLayerBlend.glsl"
#include "include/TerrainVirtualTexture.glsl"
#include "include/TerrainLayerSampling.glsl"
#include "include/TerrainPixelNormal.glsl"

void main()
{
    // Camera-relative (issue #429): absolute world position for the world-
    // anchored brush/snow patterns below (lighting/velocity keep v_WorldPos).
    vec3 worldPosAbs = v_WorldPos + u_RenderOrigin;
    int layerCount = int(u_TerrainParams.z);
    float triplanarSharpness = u_TerrainParams.w;
    // Where the layers are sampled and their screen-space gradients, taken here
    // before any per-pixel branch (include/TerrainLayerSampling.glsl says why).
    OloTerrainLayerCoords layerCoords = oloTerrainLayerCoords(v_TexCoord, worldPosAbs.y);
    // The geometric normal, from the height map at this pixel's own scale
    // rather than the vertex stage's (include/TerrainPixelNormal.glsl).
    vec3 N = normalize(mat3(v_NormalMatrixX, v_NormalMatrixY, v_NormalMatrixZ) *
                       oloTerrainPixelNormalLocal(v_TexCoord, layerCoords.TopDx, layerCoords.TopDy));

    bool useSplatmap = (layerCount > 0);

    vec3 albedo;
    float metallic;
    float roughness;
    float ao;
    vec3 normalMap = vec3(0.0, 0.0, 1.0);

    // Virtual texturing (issue #715). One indirection lookup + one cache sample
    // replaces the whole loop below, so shading cost stops scaling with
    // layerCount. `Enabled` is a UBO value, so the branch is uniform across the
    // draw — which is what makes the dFdx inside oloVTSectorMip legal here.
    //
    // Adaptive (slice 3): identical shape to Terrain_PBR.glsl — the pixel's
    // sector maps terrain UV into that sector's image inside the virtual
    // atlas, and an unready sector falls back to the splat path per pixel
    // while still writing feedback.
    OloVTParams vtParams = oloVTUnpackParams(u_TerrainVTParams0, u_TerrainVTParams1, u_TerrainVTParams2,
                                             u_TerrainVTParams3);
    bool vtResolved = false;

    if (useSplatmap && vtParams.Enabled > 0.5)
    {
        int vtSectorIdx = oloVTSectorIndex(vtParams, v_TexCoord);
        vtResolved = oloVTResolveSurface(vtParams, u_TerrainVTSectors[2 * vtSectorIdx],
                                         u_TerrainVTSectors[2 * vtSectorIdx + 1], v_TexCoord, gl_FragCoord.xy,
                                         albedo, ao, normalMap, roughness, metallic);
    }

    if (!vtResolved && useSplatmap)
    {
        // The splat blend. Both positions are camera-relative, so their
        // distance is the view distance the two-scale sampling fades on.
        oloTerrainBlendLayers(layerCoords, N, distance(u_CameraPosition, v_WorldPos), layerCount, triplanarSharpness,
                              albedo, ao, normalMap, roughness, metallic);
    }
    else if (!vtResolved)
    {
        float slope = 1.0 - N.y;
        vec3 grassColor = vec3(0.15, 0.35, 0.08);
        vec3 rockColor  = vec3(0.45, 0.4, 0.35);
        albedo = mix(grassColor, rockColor, smoothstep(0.3, 0.7, slope));
        metallic = 0.0;
        roughness = mix(0.85, 0.95, slope);
        ao = 1.0;
    }

    // Apply TBN-space normal when splatmap is active. The frame is the one the
    // layer blend expressed normalMap in.
    if (useSplatmap)
    {
        N = normalize(oloTerrainTangentFrame(N) * normalMap);
    }

    // The macro map (issue #1721): after the layer blend or the virtual-texture
    // resolve, before the brush overlay, snow and weather — the same place
    // Terrain_PBR applies it.
    oloTerrainApplyMacroMap(layerCoords, albedo, roughness);

    // Brush preview overlay (editor).
    if (v_BrushParams.x > 0.5)
    {
        vec3 brushCenter = v_BrushPosAndRadius.xyz;
        float brushRadius = v_BrushPosAndRadius.w;
        float falloff = v_BrushParams.y;

        float dist = length(worldPosAbs.xz - brushCenter.xz);
        float normalizedDist = dist / max(brushRadius, 0.001);

        if (normalizedDist < 1.0)
        {
            float innerRadius = falloff;
            float weight = normalizedDist < innerRadius ? 1.0 :
                0.5 + 0.5 * cos(3.14159265 * (normalizedDist - innerRadius) / (1.0 - innerRadius));

            vec3 brushColor = v_BrushParams.z < 0.5 ? vec3(0.0, 0.8, 1.0) : vec3(0.2, 1.0, 0.3);

            float edgeDist = abs(normalizedDist - 1.0);
            float ring = 1.0 - smoothstep(0.0, 0.03, edgeDist);

            float fill = weight * 0.15;
            albedo = mix(albedo, brushColor, fill + ring * 0.6);
        }
    }

    // THE SNOW LAYER (issue #1451) — include/SnowLayer.glsl, the same calls
    // Terrain_PBR makes. The blended material and the snow-FILLED normal go
    // into the G-Buffer, the weight into RT3.a (the material profile), and
    // the lighting pass rebuilds the shading normal and adds the sparkle
    // from those two — it used to be dropped here, so Deferred terrain had
    // no blur and no sparkle, and a noise of its own.
    float snowWeight = oloTerrainSnowWeight(worldPosAbs, v_Normal, v_SnowClip, v_SnowAccumulation > 0.5);
    vec3 terrainEmissive = vec3(0.0);
    oloSnowLayerBlendMaterial(snowWeight, albedo, metallic, roughness, ao, terrainEmissive);
    N = oloSnowLayerFilledNormal(N, snowWeight);

    o_GBufferAlbedo   = vec4(albedo, metallic);
    o_GBufferNormal   = vec4(octEncodeGB(N), roughness, ao);
    // emissive.a = 0.0 → lit. Terrain does not emit light of its own.
    o_GBufferEmissive = vec4(0.0, 0.0, 0.0, 0.0);
    // Static terrain → zero screen-space velocity.
    // .a: the material profile (#1256) is the snow weight (issue #1451).
    o_GBufferVelocity = vec4(0.0, 0.0, 1.0, snowWeight);
    o_GBufferEntityID = instances[0].EntityID;
    o_GBufferBakedGI = vec4(0.0); // no baked lightmap on this surface (issue #865)
}
