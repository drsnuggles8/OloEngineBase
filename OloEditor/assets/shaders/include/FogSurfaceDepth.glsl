// =============================================================================
// The depth the fog is computed to (issue #1721).
//
// Water is drawn blended and writes no depth, so the scene depth under a water
// pixel is the seabed's, not the surface's. Fog computed to that depth is the
// fog of a point that is not what the pixel shows: where the seabed is close
// under the surface the sea came out lightly fogged, and where it is far, or
// absent, heavily. Round a terrain tile that reads as a straight-edged patch of
// differently coloured water in the shape of the tile.
//
// WaterRenderPass captures the surface's own depth for the underwater fog
// (TEX_UNDERWATER_WATER_DEPTH; 1.0 where there is no water). The fog takes the
// nearer of the two, which is the surface wherever the camera sees one.
// FogRenderPass binds the scene depth to both when no water was drawn, so the
// min is then the scene depth unchanged.
//
// Shared by PostProcess_Fog.glsl and PostProcess_FogUpsample.glsl: the
// upsample's edge weights must judge the depths the fog was computed at.
// Include after the declarations of u_DepthTexture and u_WaterSurfaceDepth.
// =============================================================================

#ifndef OLO_FOG_SURFACE_DEPTH_GLSL
#define OLO_FOG_SURFACE_DEPTH_GLSL

float oloFogSurfaceDepth(vec2 uv)
{
    return min(texture(u_DepthTexture, uv).r, texture(u_WaterSurfaceDepth, uv).r);
}

#endif // OLO_FOG_SURFACE_DEPTH_GLSL
