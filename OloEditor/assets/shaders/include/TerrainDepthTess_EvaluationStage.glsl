layout(triangles, equal_spacing, ccw) in;

layout(location = 0) in vec3 tc_Position[];
layout(location = 1) in vec2 tc_TexCoord[];

// Camera UBO (binding 0) — holds light VP during shadow pass
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    // Camera-relative (issue #429): full tail so u_RenderOrigin is at offset 272.
    mat4 u_PrevViewProjection;
    vec3 u_RenderOrigin;
    float _padding1;
};

// Model UBO (binding 3)
#include "InstanceBlock_Single.glsl"

// Terrain UBO (binding 10)
#include "TerrainParamsBlock.glsl"

#include "BindlessHeap.glsl"

// Heap-bindless conversion (issue #691, bucket 1). Sampled in the
// VERTEX stage for displacement, which is fine: the heap SSBO (45) and the
// offset table (56) are program-wide, not fragment-only.
//
// Terrain_GBuffer.glsl and Terrain_PBR.glsl bind the same TEX_TERRAIN_HEIGHTMAP
// slot but declare the name in their OWN files, so this #define cannot reach
// them — and Terrain_GBuffer must stay slot-based anyway, being a G-Buffer
// producer the bindless route would misroute.
#ifdef OLO_BINDLESS
#define u_TerrainHeightmap OLO_HEAP_TEX_2D(23)
// CONVERTED FOR THE SAME REASON, and it was missed the first time. Its bind
// (CommandDispatch's TEX_SNOW_DEPTH BindTrackedTextureUnit) already goes
// through HeapBinding::BindTextureOrOffset, which for a bindless-variant
// program records an offset and issues NO bind. Leaving the declaration
// slot-based left this sampler unbound under OLO_RHI_BINDLESS=1, so snow
// deformation silently read zero — §5c: the unit of conversion is a C++ bind
// AND its declaration, together. Terrain_GBuffer/Terrain_PBR declare the name
// in their own files and stay slot-based, so they still get a real bind.
#define u_SnowDepthMap OLO_HEAP_TEX_2D(30)
#else
layout(binding = 23) uniform sampler2D u_TerrainHeightmap;
layout(binding = 30) uniform sampler2D u_SnowDepthMap;
#endif

#include "TerrainHeightSampling.glsl"
#include "TerrainDisplacedHeight.glsl"

// Snow Accumulation UBO (binding 16)
layout(std140, binding = 16) uniform SnowAccumulationParams {
    mat4 u_ClipmapViewProj[3];
    vec4 u_ClipmapCenterAndExtent[3];
    vec4 u_AccumulationParams;
    vec4 u_DisplacementParams;
};

#ifdef OLO_VSM_FAMILY
#include "VirtualShadowFamilyVertex.glsl"
#endif
void main()
{
    vec3 pos = gl_TessCoord.x * tc_Position[0]
             + gl_TessCoord.y * tc_Position[1]
             + gl_TessCoord.z * tc_Position[2];
    vec2 uv  = gl_TessCoord.x * tc_TexCoord[0]
             + gl_TessCoord.y * tc_TexCoord[1]
             + gl_TessCoord.z * tc_TexCoord[2];

    // Displace Y from heightmap
    float heightScale = u_WorldSizeAndHeightScale.z;
    float heightMip = oloTerrainHeightMip(tc_TexCoord[0], tc_TexCoord[1], tc_TexCoord[2]);
    float sampledHeight = oloTerrainFilteredHeight(uv, heightMip) * heightScale;
    pos.y = sampledHeight;
    float snowDisplacement = 0.0;

    // Snow accumulation displacement (must match Terrain_PBR.glsl)
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

    // Morph blend
    float morphFactor = u_TessFactors2.y;
    float meshHeight = gl_TessCoord.x * tc_Position[0].y
                     + gl_TessCoord.y * tc_Position[1].y
                     + gl_TessCoord.z * tc_Position[2].y;
    pos.y = oloTerrainDisplacedHeight(sampledHeight, snowDisplacement, meshHeight, morphFactor);

    gl_Position = u_ViewProjection * instances[0].Transform * vec4(pos, 1.0);
#ifdef OLO_VSM_FAMILY
    gl_Position = vsmFamilyPosition(gl_Position);
#endif
}
