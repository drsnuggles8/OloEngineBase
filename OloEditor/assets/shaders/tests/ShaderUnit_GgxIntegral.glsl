#version 460 core

// =============================================================================
// ShaderUnit_GgxIntegral.glsl
//
// Numerical hemisphere integration of the GGX normal distribution function.
// The integral of D(h) * cos(theta_h) over the hemisphere must equal 1 for
// any roughness value — this is the normalization property of a PDF.
//
// Scheme: stratified-grid integration on (theta, phi). Each thread owns a
// single cell; it writes D * cos(theta) * sin(theta) * dθ * dφ to its
// output slot. The CPU side sums all slots and asserts the result
// approaches 1 (within discretization error).
//
// Dispatch: (kThetaSteps / 8) × (kPhiSteps / 8, 1) workgroups of 8×8.
// Buffer layout:
//   binding 0 (in)  : single float u_Roughness   — packed as u_Inputs[0]
//   binding 1 (out) : f32 per cell, row-major  [phi][theta]
//
// Note: we integrate w.r.t. the macro-surface normal (N = +Y). Each sample
// uses H = (sin θ cos φ, cos θ, sin θ sin φ) and D(N, H, rough).
// =============================================================================

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(std430, binding = 0) readonly  buffer Inputs  { float u_Inputs[]; };
layout(std430, binding = 1) writeonly buffer Cells   { float u_Cells[]; };

uniform int u_ThetaSteps;
uniform int u_PhiSteps;

const float PI = 3.14159265358979;
// Vector GGX density matches distributionGGXUnclampedNH in PBRCommon.
float distributionGGXUnclampedNH(vec3 N, vec3 H, float roughness)
{
    float alpha = roughness * roughness;
    float a2 = alpha * alpha;
    float c = dot(N, H);
    vec3 t = cross(N, H);
    float q = dot(t, t) + a2 * c * c;
    return a2 / (PI * q * q);
}

void main()
{
    uint thetaIdx = gl_GlobalInvocationID.x;
    uint phiIdx = gl_GlobalInvocationID.y;
    if (int(thetaIdx) >= u_ThetaSteps || int(phiIdx) >= u_PhiSteps)
        return;

    float roughness = u_Inputs[0];
    float dTheta = (0.5 * PI) / float(u_ThetaSteps);
    float dPhi   = (2.0 * PI) / float(u_PhiSteps);

    // Midpoint rule for improved accuracy.
    float theta = (float(thetaIdx) + 0.5) * dTheta;
    float phi   = (float(phiIdx)   + 0.5) * dPhi;

    vec3 N = vec3(0.0, 1.0, 0.0);
    vec3 H = vec3(sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi));

    float D = distributionGGXUnclampedNH(N, H, roughness);
    float cosTheta = cos(theta);
    float sinTheta = sin(theta);

    float contribution = D * cosTheta * sinTheta * dTheta * dPhi;
    uint flat_ = phiIdx * uint(u_ThetaSteps) + thetaIdx;
    u_Cells[flat_] = contribution;
}
