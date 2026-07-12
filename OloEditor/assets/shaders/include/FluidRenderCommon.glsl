// FluidRenderCommon.glsl — the FluidRenderUBO parameter block shared by every
// screen-space fluid rendering stage (issue #630, pillar B): the splat
// vertex/fragment shaders (via FluidSplatCommon.glsl), the bilateral depth
// smooth (FluidSmooth.comp) and the composite (FluidComposite.glsl).
//
// GLSL twin of UBOStructures::FluidRenderUBO
// (OloEngine/src/OloEngine/Renderer/ShaderBindingLayout.h, std140 binding 48,
// 96 bytes, static_assert-pinned on the C++ side). Declared here ONCE so all
// fluid render stages agree on the layout — keep field-for-field in sync.

#ifndef FLUID_RENDER_COMMON_GLSL
#define FLUID_RENDER_COMMON_GLSL

layout(std140, binding = 48) uniform FluidRenderUBO
{
    vec4 TintRadius;       // xyz = surface tint, w = particle world radius
    vec4 AbsorptionParams; // xyz = Beer-Lambert absorption color, w = absorption scale
    vec4 FoamParams;       // x = foam speed threshold, y = foam intensity, z/w = unused
    vec4 SmoothParams;     // x = blur radius (px), y = depth falloff (view metres), z = camera near, w = camera far
    vec4 ScreenParams;     // xy = viewport size (px), zw = texel size
    uvec4 Counts;          // x = particle upper bound, y = entity id (as uint), z = env-map-present flag, w = unused
} u_FluidRender;

#endif // FLUID_RENDER_COMMON_GLSL
