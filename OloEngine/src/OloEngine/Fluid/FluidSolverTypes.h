#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace OloEngine
{
    // =========================================================================
    // Shared types for the Position-Based Fluids solver (issue #630).
    //
    // The CPU and GPU solvers consume the same FluidSolverParams and body-proxy
    // representation so the Jolt coupling logic and every contract test can run
    // against either backend. GPU-mirrored structs must match the std430
    // layouts in OloEditor/assets/shaders/compute/Fluid_*.comp exactly (layout
    // pins live in OloEngine/tests/Fluid/FluidSolverTypesTest section of the
    // kernel contract test).
    //
    // NOTE: none of these are ECS components — do NOT suffix any struct here
    // with "Component" or OloHeaderTool's struct-*Component scan sweeps it into
    // the generated AllComponents/SaveGame/serializer lists and breaks the
    // build (CLAUDE.md "Common pitfalls").
    // =========================================================================

    /// Collision-proxy shape for two-way rigid-body coupling. Extracted from a
    /// Jolt body's shape at the physics kick seam; particles collide against
    /// the analytic SDF and the reaction impulse is fed back to the body.
    enum class FluidBodyProxyShape : u8
    {
        Sphere = 0,
        Box = 1,
        Capsule = 2,
    };

    /// One rigid body overlapping a fluid domain, snapshotted at the kick seam.
    /// vec4-packed so the identical bytes upload to SSBO_FLUID_BODY_PROXIES.
    /// 80 bytes, std430-aligned.
    struct FluidBodyProxy
    {
        glm::vec4 Position;        // xyz = center of mass (world), w = shape type as float (FluidBodyProxyShape)
        glm::vec4 Rotation;        // world-from-local quaternion (x, y, z, w)
        glm::vec4 HalfExtents;     // sphere: x = radius; box: xyz = half extents; capsule: x = radius, y = half height (cylinder part)
        glm::vec4 LinearVelocity;  // xyz = COM linear velocity, w = inverse mass (0 = static/kinematic)
        glm::vec4 AngularVelocity; // xyz = angular velocity (rad/s), w = unused

        static constexpr u32 GetSize()
        {
            return sizeof(FluidBodyProxy);
        }
    };

    static_assert(sizeof(FluidBodyProxy) == 80, "FluidBodyProxy must be 80 bytes for std430 upload");

    /// Reaction the solver accumulated on one body proxy over a step, in world
    /// space. The caller (FluidSystem) converts this into JoltBody::AddImpulse /
    /// AddAngularImpulse before the Jolt step integrates.
    struct FluidBodyFeedback
    {
        glm::vec3 Impulse{ 0.0f };        // kg·m/s
        glm::vec3 AngularImpulse{ 0.0f }; // kg·m²/s, about the COM
    };

    /// Solver tuning derived each tick from FluidSettings + FluidComponent.
    /// All values are validated/clamped by the caller (FluidSystem) before the
    /// solvers see them.
    struct FluidSolverParams
    {
        glm::vec3 BoundsMin{ -4.0f, 0.0f, -4.0f }; // world-space domain AABB
        glm::vec3 BoundsMax{ 4.0f, 8.0f, 4.0f };
        glm::vec3 Gravity{ 0.0f, -9.81f, 0.0f };

        f32 ParticleRadius = 0.1f;       // r: rendering/collision radius; rest spacing = 2r
        f32 SmoothingRadiusScale = 2.0f; // h = scale * (2r); ~30-40 neighbours at rest
        f32 RestDensity = 1000.0f;       // rho0, kg/m^3

        u32 SolverIterations = 4; // constraint-projection (Jacobi) iterations (under-relaxed — see kFluidJacobiRelaxation)
        f32 CfmEpsilon = 50.0f;   // lambda relaxation (Macklin & Müller eq. 11)
        f32 SCorrK = 0.01f;       // tensile-instability anti-clump strength (eq. 13; scaled for physical-mass units, see kFluidMaxDeltaPFraction)
        f32 SCorrN = 4.0f;        // anti-clump exponent
        f32 SCorrDeltaQ = 0.2f;   // |Δq| as a fraction of h

        f32 XsphViscosity = 0.1f;     // XSPH c (eq. 17)
        f32 VorticityEpsilon = 0.05f; // vorticity-confinement strength (eq. 16)

        f32 MaxSpeed = 40.0f;         // velocity clamp, m/s (stability)
        f32 CouplingStiffness = 1.0f; // scales the reaction impulse fed back to bodies

        /// Rest particle spacing d = 2r.
        [[nodiscard]] f32 Spacing() const
        {
            return 2.0f * ParticleRadius;
        }
        /// Smoothing radius h.
        [[nodiscard]] f32 SmoothingRadius() const
        {
            return SmoothingRadiusScale * Spacing();
        }
        /// Per-particle mass m = rho0 * d^3 (each particle owns one spacing-cube of fluid).
        [[nodiscard]] f32 ParticleMass() const
        {
            const f32 d = Spacing();
            return RestDensity * d * d * d;
        }
    };

    // =========================================================================
    // GPU mirror structs (std430)
    // =========================================================================

    /// Atomic counters in SSBO_FLUID_COUNTERS. 16 bytes.
    struct GPUFluidCounters
    {
        u32 Count;        // live particle count (atomicAdd'd by emit, rewritten after compact)
        u32 EmitCount;    // staged emissions this step (written by CPU)
        u32 KillCount;    // particles marked dead this step (atomicAdd'd by the kill pass)
        u32 ScratchCount; // compact pass's atomic append cursor; promoted to Count afterwards
    };

    static_assert(sizeof(GPUFluidCounters) == 16, "GPUFluidCounters must be 16 bytes");

    /// One staged emission in SSBO_FLUID_EMIT_STAGING. 32 bytes.
    struct GPUFluidEmitEntry
    {
        glm::vec4 Position; // xyz = world spawn position, w = unused
        glm::vec4 Velocity; // xyz = initial velocity, w = unused

        static constexpr u32 GetSize()
        {
            return sizeof(GPUFluidEmitEntry);
        }
    };

    static_assert(sizeof(GPUFluidEmitEntry) == 32, "GPUFluidEmitEntry must be 32 bytes");

    /// Fixed-point impulse accumulator per body proxy in
    /// SSBO_FLUID_BODY_IMPULSES: GLSL atomicAdd exists only for int/uint, so
    /// the GPU accumulates impulse/angular-impulse * kFluidImpulseFixedScale as
    /// i32 and the CPU divides on readback. 32 bytes per proxy.
    struct GPUFluidBodyImpulse
    {
        i32 ImpulseX, ImpulseY, ImpulseZ;
        i32 AngularX, AngularY, AngularZ;
        i32 Pad0, Pad1;
    };

    static_assert(sizeof(GPUFluidBodyImpulse) == 32, "GPUFluidBodyImpulse must be 32 bytes");

    /// Fixed-point scale for GPUFluidBodyImpulse (1/4096 kg·m/s resolution,
    /// ±524k range — far beyond any sane per-step fluid impulse).
    inline constexpr f32 kFluidImpulseFixedScale = 4096.0f;

    /// Hard cap on body proxies coupled per fluid domain per step. Bodies
    /// beyond this (dynamic-first, nearest-first at extraction) are ignored.
    inline constexpr u32 kFluidMaxBodyProxies = 64;

    /// Emit-staging capacity: max particles staged per domain per step (both
    /// backends; the GPU staging SSBO is sized to this).
    inline constexpr u32 kFluidMaxEmitPerStep = 4096;

    /// Workgroup size for every per-particle Fluid_*.comp dispatch — keep in
    /// sync with layout(local_size_x = 256) in the shaders by hand.
    inline constexpr u32 kFluidWorkgroupSize = 256;

    /// Jacobi under-relaxation applied to the summed position correction.
    /// Overlapping density constraints (each particle sits in ~30 neighbours'
    /// constraints) over-correct when satisfied simultaneously, which drives a
    /// persistent collective 'breathing' oscillation XSPH cannot damp (it is
    /// neighbour-relative; the breathing mode is in-phase). 0.4 is the usual
    /// PBD/Flex-style successive-over-relaxation range.
    inline constexpr f32 kFluidJacobiRelaxation = 0.4f;

    /// Per-iteration cap on the pressure correction |delta p| as a fraction
    /// of the smoothing radius h. The s_corr anti-clump term diverges as two
    /// particles approach (W(r) -> W(0) while gradW blows up), so an unclamped
    /// Jacobi step can hurl close pairs metres apart in one iteration — the
    /// classic PBF explosion. Both solver backends apply this clamp; the CPU
    /// contract tests pin the resulting stability.
    inline constexpr f32 kFluidMaxDeltaPFraction = 0.2f;

    /// Upper bound on dense-grid cells per axis; the cell size grows above the
    /// smoothing radius when a domain is too large so the head buffer stays
    /// bounded (cells >= h keeps the 27-cell neighbourhood correct).
    inline constexpr u32 kFluidMaxGridCellsPerAxis = 128;
} // namespace OloEngine
