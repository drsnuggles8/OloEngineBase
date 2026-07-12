// FluidSplatCommon.glsl — shared declarations for the fluid particle splat
// stages (FluidDepthSplat.glsl + FluidThickness.glsl, issue #630 pillar B):
// the solver-owned particle SSBOs, the FluidRenderUBO parameter block, and
// the live-instance helpers for the SSBO-indexed instanced quad draw.
//
// SSBO layouts mirror the GPU PBF solver's buffers (bindings pinned in
// ShaderBindingLayout.h: SSBO_FLUID_POSITIONS = 21, SSBO_FLUID_VELOCITIES =
// 22, SSBO_FLUID_COUNTERS = 28; block shapes match Fluid_Integrate.comp).
// The render passes bind the solver's raw buffer ids at these points.

#ifndef FLUID_SPLAT_COMMON_GLSL
#define FLUID_SPLAT_COMMON_GLSL

#include "FluidRenderCommon.glsl"

layout(std430, binding = 21) readonly buffer FluidPositionsBuffer
{
    vec4 positions[]; // xyz = absolute world position, w = kill flag (< 0.0 marked dead)
};

layout(std430, binding = 22) readonly buffer FluidVelocitiesBuffer
{
    vec4 velocities[]; // xyz = velocity (m/s), w = unused
};

layout(std430, binding = 28) readonly buffer FluidCountersBuffer
{
    uint Count;        // live particle count (compact-maintained)
    uint EmitCount;
    uint KillCount;
    uint ScratchCount;
} fluidCounters;

// True when this instance indexes a live particle. The draw is issued at the
// CPU-known conservative upper bound (readback discipline #551), so the
// GPU-side live count is the real gate; the kill flag guards mid-step deaths.
bool FluidSplatAlive(uint instanceIndex)
{
    uint liveCount = min(fluidCounters.Count, u_FluidRender.Counts.x);
    if (instanceIndex >= liveCount)
    {
        return false;
    }
    if (positions[instanceIndex].w < 0.0)
    {
        return false;
    }
    return true;
}

// Degenerate clip position for dead instances: z > w is clipped, and all four
// quad corners collapsing to one point rasterises zero area either way.
vec4 FluidSplatDegenerate()
{
    return vec4(0.0, 0.0, 2.0, 1.0);
}

#endif // FLUID_SPLAT_COMMON_GLSL
