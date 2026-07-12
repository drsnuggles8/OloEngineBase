// FluidKernels.glsl - shared GLSL for the GPU Position-Based Fluids solver
// (issue #630): SPH smoothing kernels, quaternion rotation helpers, and the
// FluidUBO parameter block every Fluid_*.comp pass consumes.
//
// CPU mirror: OloEngine/src/OloEngine/Fluid/FluidKernels.h - poly6Scale /
// spikyGradScale / poly6 / spikyGrad MUST stay formula-identical with the
// kernels there. The GPU-vs-CPU parity test
// (OloEngine/tests/Rendering/PropertyTests/GPUFluidSolverParityTest.cpp) pins
// them transitively through full CPU-vs-GPU solver runs.
//
// The FluidUBO block is the GLSL twin of UBOStructures::FluidUBO
// (OloEngine/src/OloEngine/Renderer/ShaderBindingLayout.h, std140 binding 47,
// 416 bytes, static_assert-pinned on the C++ side). It is declared here ONCE
// so all twelve Fluid_*.comp passes agree on the layout - a drifted copy in a
// single pass would be silent data corruption.

#ifndef FLUID_KERNELS_GLSL
#define FLUID_KERNELS_GLSL

// ---------------------------------------------------------------------------
// Solver parameter UBO (std140, binding 47 = ShaderBindingLayout::UBO_FLUID).
// Field-for-field mirror of UBOStructures::FluidUBO - keep in sync by hand.
// ---------------------------------------------------------------------------
layout(std140, binding = 47) uniform FluidUBO
{
    vec4 BoundsMinCellSize;   // xyz = domain AABB min, w = grid cell size
    vec4 BoundsMaxDt;         // xyz = domain AABB max, w = step dt
    vec4 GravityH;            // xyz = gravity, w = smoothing radius h
    vec4 KernelScales;        // x = poly6 scale, y = spiky-gradient scale, z = W(deltaQ*h) (s_corr denominator), w = particle mass
    vec4 PbfParams;           // x = 1/restDensity, y = CFM epsilon, z = s_corr k, w = s_corr n
    vec4 ViscosityParams;     // x = XSPH c, y = vorticity epsilon, z = max speed, w = particle radius
    vec4 CouplingParams;      // x = coupling stiffness, y = impulse fixed-point scale, z = max |delta p| per iteration, w = Jacobi under-relaxation
    uvec4 GridDims;           // xyz = grid cell counts, w = total cell count
    uvec4 Counts;             // x = max particles, y = emit count, z = body proxy count, w = kill box count
    uvec4 StepFlags;          // x = Jacobi read-parity (0 = A, 1 = B), y/z/w = unused
    vec4 KillBoxMin[8];       // xyz = kill AABB min (w unused); count in Counts.w (kMaxKillBoxes = 8)
    vec4 KillBoxMax[8];       // xyz = kill AABB max (w unused)
} u_Fluid;

// ---------------------------------------------------------------------------
// SPH smoothing kernels (Macklin & Mueller, "Position Based Fluids", 2013;
// kernels from Mueller et al. 2003 eq. 20/21).
// ---------------------------------------------------------------------------

// poly6 normalization: 315 / (64 * pi * h^9).
// CPU mirror: FluidKernels.h (Poly6Scale)
float poly6Scale(float smoothingRadius)
{
    float h3 = smoothingRadius * smoothingRadius * smoothingRadius;
    float h9 = h3 * h3 * h3;
    return 315.0 / (64.0 * 3.14159265358979323846 * h9);
}

// spiky gradient normalization: -45 / (pi * h^6).
// CPU mirror: FluidKernels.h (SpikyGradScale)
float spikyGradScale(float smoothingRadius)
{
    float h2 = smoothingRadius * smoothingRadius;
    float h6 = h2 * h2 * h2;
    return -45.0 / (3.14159265358979323846 * h6);
}

// W_poly6(r, h) = scale * (h^2 - r^2)^3 for 0 <= r < h, else 0.
// CPU mirror: FluidKernels.h (Poly6)
float poly6(float dist, float smoothingRadius, float scale)
{
    if (dist >= smoothingRadius || dist < 0.0)
    {
        return 0.0;
    }
    float diff = (smoothingRadius * smoothingRadius) - (dist * dist);
    return scale * diff * diff * diff;
}

// grad W_spiky(rVec, h) = scale * (h - r)^2 * rVec / r for 0 < r < h.
// Returns the zero vector at r = 0 (undefined gradient - a coincident pair
// contributes no push direction) and outside the support radius.
// CPU mirror: FluidKernels.h (SpikyGrad)
vec3 spikyGrad(vec3 offsetVec, float smoothingRadius, float scale)
{
    float dist = length(offsetVec);
    if (dist <= 0.0 || dist >= smoothingRadius)
    {
        return vec3(0.0);
    }
    float diff = smoothingRadius - dist;
    return offsetVec * (scale * diff * diff / dist);
}

// ---------------------------------------------------------------------------
// Quaternion helpers for the body-proxy local/world transforms. The proxy
// uploads its world-from-local rotation as vec4(x, y, z, w).
// ---------------------------------------------------------------------------

// Rotate v by unit quaternion q. Term order mirrors glm's
// operator*(quat, vec3): v + ((cross(q.xyz, v) * q.w) + cross(q.xyz, cross(q.xyz, v))) * 2.
// CPU mirror: CPUFluidSolver.cpp (ProxyRotation + glm::quat * glm::vec3)
vec3 quatRotate(vec4 q, vec3 v)
{
    vec3 uv = cross(q.xyz, v);
    vec3 uuv = cross(q.xyz, uv);
    return v + ((uv * q.w) + uuv) * 2.0;
}

// Rotate v by the conjugate of q (the inverse rotation for a unit quaternion):
// world -> proxy-local. CPU mirror: glm::conjugate(rotation) * (p - com).
vec3 quatConjugateRotate(vec4 q, vec3 v)
{
    return quatRotate(vec4(-q.xyz, q.w), v);
}

#endif // FLUID_KERNELS_GLSL
