#ifndef TERRAIN_PARAMS_BLOCK_GLSL
#define TERRAIN_PARAMS_BLOCK_GLSL

// =============================================================================
// The terrain UBO (binding 10) — declared HERE and nowhere else.
//
// This block used to be hand-copied into thirteen places: every stage of
// Terrain_PBR / Terrain_GBuffer / Terrain_Depth, the four voxel shaders, and
// include/TerrainGpuDrivenVertex.glsl. All thirteen were byte-identical, which
// is exactly the state that stops being true the moment someone adds a member.
//
// It stopped being merely untidy with issue #715: `ShaderUBOSizeConsistency.
// CrossStageUBOLayoutAgreesWithinShader` pins every stage of ONE shader to the
// same block layout (glLinkProgram rejects a disagreement outright), so
// appending the VT members to only the stage that reads them would have been a
// link failure, and appending them to twelve of thirteen copies would have been
// a link failure somewhere less obvious. One declaration removes the choice.
//
// C++ twin: ShaderBindingLayout::UBOStructures::TerrainUBO. The GLSL block may
// be SHORTER than that struct (std140 lets the buffer carry trailing bytes the
// shader never reads) but never longer.
//
// No samplers, no images, no OLO_BINDLESS token: including this file does not
// move an includer onto the bindless route.
// =============================================================================

layout(std140, binding = 10) uniform TerrainParams {
    vec4 u_WorldSizeAndHeightScale;
    vec4 u_TerrainParams;
    int u_HeightmapResolution;
    int u_TerrainGpuDriven;
    int u_TerrainGpuGridRes;
    int _terrainPad2;
    vec4 u_TessFactors;
    vec4 u_TessFactors2;
    vec4 u_LayerTilingScales0;
    vec4 u_LayerTilingScales1;
    vec4 u_LayerBlendSharpness0;
    vec4 u_LayerBlendSharpness1;
    // Virtual texturing (issue #715). Unpacked by oloVTUnpackParams() in
    // include/TerrainVirtualTexture.glsl; every shader gets the members whether
    // or not it has a VT branch, because they all have to agree.
    vec4 u_TerrainVTParams0; // x = pagesWide, y = pageTexels, z = borderTexels, w = tileTexels
    vec4 u_TerrainVTParams1; // x = cacheTexels, y = maxMip, zw = feedback dimensions
    vec4 u_TerrainVTParams2; // x = enabled, y = feedback frame slot, z = downscale, w = log2(downscale)
};

#endif // TERRAIN_PARAMS_BLOCK_GLSL
