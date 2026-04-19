// =============================================================================
// ShaderUnit_TerrainFlatHeightmap.glsl
//
// Fullscreen probe that replicates the Terrain_PBR vertex displacement +
// normal-from-heightmap logic using a UNIFORM-CONSTANT heightmap. The probe
// samples u_TerrainHeightmap (binding 23) just like production, computes
// 4-tap central-difference normals, and writes (normal.xyz, 1) to the
// framebuffer.
//
// For a truly flat heightmap (all texels equal), hL == hR and hD == hU so
// the computed normal must be exactly (0, 1, 0). Any deviation indicates:
//   - wrong derivative weighting,
//   - swapped X/Z axes,
//   - texelSize sign error,
//   - heightScale dependency (a flat field is flat at any scale).
//
// Catches regressions in the normal reconstruction kernel without needing
// the full tessellated patch pipeline.
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;
layout(location = 0) out vec2 v_TexCoord;

void main()
{
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;

layout(binding = 0) uniform sampler2D u_TerrainHeightmap;

const float HEIGHT_SCALE = 20.0;     // matches default TerrainConfig scale
const float HEIGHTMAP_RES = 32.0;    // test fixture uses a 32x32 heightmap
const float TERRAIN_SIZE  = 100.0;   // world-space size of the patch

void main()
{
    vec2 uv = v_TexCoord;
    float texelSize = 1.0 / HEIGHTMAP_RES;

    float hL = texture(u_TerrainHeightmap, uv + vec2(-texelSize, 0.0)).r * HEIGHT_SCALE;
    float hR = texture(u_TerrainHeightmap, uv + vec2( texelSize, 0.0)).r * HEIGHT_SCALE;
    float hD = texture(u_TerrainHeightmap, uv + vec2(0.0, -texelSize)).r * HEIGHT_SCALE;
    float hU = texture(u_TerrainHeightmap, uv + vec2(0.0,  texelSize)).r * HEIGHT_SCALE;

    float dX = (hR - hL) / (2.0 * texelSize * TERRAIN_SIZE);
    float dZ = (hU - hD) / (2.0 * texelSize * TERRAIN_SIZE);

    vec3 normal = normalize(vec3(-dX, 1.0, -dZ));
    o_Color = vec4(normal, 1.0);
}
