#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    // @brief Per-fluid draw submission for the screen-space fluid rendering
    // passes (issue #630, pillar B).
    //
    // One instance is submitted per GPU-fluid entity per frame (Renderer3D →
    // FluidIntermediatesPass::SetFrameDraws). The SSBO ids are RAW GL buffer
    // ids owned by the fluid solver (GPUFluidSolver) — the render passes bind
    // them by id at the fixed ShaderBindingLayout::SSBO_FLUID_* binding points
    // and never take ownership.
    //
    // POD by design: trivially copyable, no Ref<> members, safe to move
    // through the per-frame draw list. Deliberately NOT named *Component —
    // OloHeaderTool sweeps every `struct *Component` into the ECS codegen.
    struct FluidRenderData
    {
        u32 PositionsSSBOId = 0;    // GL buffer id, vec4 world positions (w = kill flag)
        u32 VelocitiesSSBOId = 0;   // GL buffer id, vec4 velocities
        u32 CountersSSBOId = 0;     // GL buffer id, GPUFluidCounters (live count in [0])
        u32 ParticleUpperBound = 0; // CPU-known conservative count (instance count for draws)
        f32 ParticleRadius = 0.1f;
        glm::vec3 Tint{ 0.35f, 0.65f, 0.85f };
        glm::vec3 AbsorptionColor{ 0.45f, 0.06f, 0.01f };
        f32 AbsorptionScale = 1.0f;
        f32 FoamSpeedThreshold = 3.0f;
        i32 EntityID = -1;
    };
} // namespace OloEngine
