#include "OloEnginePCH.h"
#include "OloEngine/Fluid/CPUFluidSolver.h"

#include "OloEngine/Fluid/FluidKernels.h"

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    namespace
    {
        /// Signed distance from `localPoint` to the proxy surface in the
        /// proxy's local frame, plus the outward surface normal. Mirrors the
        /// GLSL in Fluid_Displace.comp — keep formula-identical.
        f32 ProxySignedDistance(const FluidBodyProxy& proxy, const glm::vec3& localPoint, glm::vec3& outLocalNormal)
        {
            const auto shape = static_cast<FluidBodyProxyShape>(static_cast<u32>(proxy.Position.w + 0.5f));
            switch (shape)
            {
                case FluidBodyProxyShape::Sphere:
                {
                    const f32 radius = proxy.HalfExtents.x;
                    const f32 dist = glm::length(localPoint);
                    outLocalNormal = dist > 1.0e-6f ? localPoint / dist : glm::vec3(0.0f, 1.0f, 0.0f);
                    return dist - radius;
                }
                case FluidBodyProxyShape::Capsule:
                {
                    const f32 radius = proxy.HalfExtents.x;
                    const f32 halfHeight = proxy.HalfExtents.y;
                    const glm::vec3 toAxis(localPoint.x, localPoint.y - std::clamp(localPoint.y, -halfHeight, halfHeight), localPoint.z);
                    const f32 dist = glm::length(toAxis);
                    outLocalNormal = dist > 1.0e-6f ? toAxis / dist : glm::vec3(0.0f, 1.0f, 0.0f);
                    return dist - radius;
                }
                case FluidBodyProxyShape::Box:
                default:
                {
                    const glm::vec3 halfExtents(proxy.HalfExtents);
                    const glm::vec3 q = glm::abs(localPoint) - halfExtents;
                    if (q.x > 0.0f || q.y > 0.0f || q.z > 0.0f)
                    {
                        // Outside: distance to the nearest face/edge/corner.
                        const glm::vec3 clamped = glm::max(q, glm::vec3(0.0f));
                        const f32 dist = glm::length(clamped);
                        glm::vec3 outward = glm::sign(localPoint) * clamped;
                        outLocalNormal = dist > 1.0e-6f ? outward / dist : glm::vec3(0.0f, 1.0f, 0.0f);
                        return dist;
                    }
                    // Inside: push toward the closest face.
                    const glm::vec3 depths = -q; // positive penetration per axis
                    if (depths.x <= depths.y && depths.x <= depths.z)
                    {
                        outLocalNormal = glm::vec3(localPoint.x >= 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f);
                        return -depths.x;
                    }
                    if (depths.y <= depths.z)
                    {
                        outLocalNormal = glm::vec3(0.0f, localPoint.y >= 0.0f ? 1.0f : -1.0f, 0.0f);
                        return -depths.y;
                    }
                    outLocalNormal = glm::vec3(0.0f, 0.0f, localPoint.z >= 0.0f ? 1.0f : -1.0f);
                    return -depths.z;
                }
            }
        }

        glm::quat ProxyRotation(const FluidBodyProxy& proxy)
        {
            return glm::quat(proxy.Rotation.w, proxy.Rotation.x, proxy.Rotation.y, proxy.Rotation.z);
        }
    } // namespace

    CPUFluidSolver::CPUFluidSolver(u32 maxParticles)
    {
        Reset(maxParticles);
    }

    void CPUFluidSolver::Reset(u32 maxParticles)
    {
        m_MaxParticles = maxParticles;
        m_Positions.clear();
        m_Velocities.clear();
        m_Positions.reserve(maxParticles);
        m_Velocities.reserve(maxParticles);
        m_LastAverageDensity = 0.0f;
        m_LastMaxDensityError = 0.0f;
    }

    void CPUFluidSolver::Emit(std::span<const GPUFluidEmitEntry> entries)
    {
        for (const GPUFluidEmitEntry& entry : entries)
        {
            if (static_cast<u32>(m_Positions.size()) >= m_MaxParticles)
            {
                break;
            }
            m_Positions.emplace_back(entry.Position);
            m_Velocities.emplace_back(entry.Velocity);
        }
    }

    void CPUFluidSolver::BuildGrid(const FluidSolverParams& params)
    {
        const f32 h = params.SmoothingRadius();
        const glm::vec3 extent = params.BoundsMax - params.BoundsMin;
        const f32 maxExtent = std::max({ extent.x, extent.y, extent.z, h });
        m_CellSize = std::max(h, maxExtent / static_cast<f32>(kFluidMaxGridCellsPerAxis));
        m_GridOrigin = params.BoundsMin;
        m_GridDims = glm::uvec3(
            std::max(1u, static_cast<u32>(std::ceil(extent.x / m_CellSize))),
            std::max(1u, static_cast<u32>(std::ceil(extent.y / m_CellSize))),
            std::max(1u, static_cast<u32>(std::ceil(extent.z / m_CellSize))));

        const sizet cellCount = static_cast<sizet>(m_GridDims.x) * m_GridDims.y * m_GridDims.z;
        m_GridHead.assign(cellCount, 0u);
        m_GridNext.assign(m_Predicted.size(), 0u);

        for (u32 i = 0; i < static_cast<u32>(m_Predicted.size()); ++i)
        {
            const glm::vec3 rel = (m_Predicted[i] - m_GridOrigin) / m_CellSize;
            const glm::uvec3 cell = glm::min(
                glm::uvec3(glm::max(rel, glm::vec3(0.0f))),
                m_GridDims - glm::uvec3(1));
            const sizet cellIndex = (static_cast<sizet>(cell.z) * m_GridDims.y + cell.y) * m_GridDims.x + cell.x;
            m_GridNext[i] = m_GridHead[cellIndex];
            m_GridHead[cellIndex] = i + 1;
        }
    }

    template<typename Fn>
    void CPUFluidSolver::ForEachNeighbour(const glm::vec3& position, Fn&& fn) const
    {
        const glm::vec3 rel = (position - m_GridOrigin) / m_CellSize;
        const glm::ivec3 center(glm::floor(rel));
        for (i32 dz = -1; dz <= 1; ++dz)
        {
            for (i32 dy = -1; dy <= 1; ++dy)
            {
                for (i32 dx = -1; dx <= 1; ++dx)
                {
                    const glm::ivec3 cell = center + glm::ivec3(dx, dy, dz);
                    if (cell.x < 0 || cell.y < 0 || cell.z < 0 ||
                        cell.x >= static_cast<i32>(m_GridDims.x) ||
                        cell.y >= static_cast<i32>(m_GridDims.y) ||
                        cell.z >= static_cast<i32>(m_GridDims.z))
                    {
                        continue;
                    }
                    const sizet cellIndex = (static_cast<sizet>(cell.z) * m_GridDims.y + cell.y) * m_GridDims.x + cell.x;
                    for (u32 cursor = m_GridHead[cellIndex]; cursor != 0u; cursor = m_GridNext[cursor - 1])
                    {
                        fn(cursor - 1);
                    }
                }
            }
        }
    }

    void CPUFluidSolver::Step(const FluidSolverParams& params, f32 dt,
                              std::span<const FluidBodyProxy> bodyProxies,
                              std::span<FluidBodyFeedback> outBodyFeedback,
                              std::span<const FluidKillBox> killBoxes)
    {
        OLO_PROFILE_FUNCTION();

        if (dt <= 0.0f || !std::isfinite(dt))
        {
            return;
        }

        for (FluidBodyFeedback& feedback : outBodyFeedback)
        {
            feedback = FluidBodyFeedback{};
        }

        // ---- Kill volumes (swap-with-last keeps the array dense; iteration is
        // back-to-front so the swapped-in element is already processed) -------
        if (!killBoxes.empty())
        {
            for (sizet i = m_Positions.size(); i-- > 0;)
            {
                const glm::vec3& p = m_Positions[i];
                bool kill = false;
                for (const FluidKillBox& box : killBoxes)
                {
                    if (p.x >= box.Min.x && p.x <= box.Max.x &&
                        p.y >= box.Min.y && p.y <= box.Max.y &&
                        p.z >= box.Min.z && p.z <= box.Max.z)
                    {
                        kill = true;
                        break;
                    }
                }
                if (kill)
                {
                    m_Positions[i] = m_Positions.back();
                    m_Velocities[i] = m_Velocities.back();
                    m_Positions.pop_back();
                    m_Velocities.pop_back();
                }
            }
        }

        const u32 count = static_cast<u32>(m_Positions.size());
        if (count == 0)
        {
            m_LastAverageDensity = 0.0f;
            m_LastMaxDensityError = 0.0f;
            return;
        }

        const f32 h = params.SmoothingRadius();
        const f32 poly6Scale = FluidKernels::Poly6Scale(h);
        const f32 spikyScale = FluidKernels::SpikyGradScale(h);
        const f32 mass = params.ParticleMass();
        const f32 restDensityInv = 1.0f / params.RestDensity;
        const f32 sCorrDenom = FluidKernels::Poly6(params.SCorrDeltaQ * h, h, poly6Scale);
        const f32 particleRadius = params.ParticleRadius;
        const glm::vec3 innerMin = params.BoundsMin + glm::vec3(particleRadius);
        const glm::vec3 innerMax = params.BoundsMax - glm::vec3(particleRadius);

        m_Predicted.resize(count);
        m_DeltaP.resize(count);
        m_Lambda.resize(count);
        m_Density.resize(count);
        m_Omega.resize(count);
        m_VelocityScratch.resize(count);

        // ---- 1. External forces + prediction --------------------------------
        for (u32 i = 0; i < count; ++i)
        {
            glm::vec3 v = m_Velocities[i] + params.Gravity * dt;
            const f32 speed2 = glm::dot(v, v);
            if (speed2 > params.MaxSpeed * params.MaxSpeed)
            {
                v *= params.MaxSpeed / std::sqrt(speed2);
            }
            m_Velocities[i] = v;
            m_Predicted[i] = glm::clamp(m_Positions[i] + v * dt, innerMin, innerMax);
        }

        // ---- 2. Neighbour grid (once per step, on predictions) --------------
        BuildGrid(params);

        // ---- 3. Constraint projection (Jacobi) -------------------------------
        const f32 dtInv = 1.0f / dt;
        for (u32 iteration = 0; iteration < params.SolverIterations; ++iteration)
        {
            // 3a. Density + lambda.
            f32 densitySum = 0.0f;
            f32 maxError = 0.0f;
            for (u32 i = 0; i < count; ++i)
            {
                const glm::vec3 pi = m_Predicted[i];
                f32 density = 0.0f;
                glm::vec3 gradI(0.0f);
                f32 gradSum = 0.0f;
                ForEachNeighbour(pi, [&](u32 j)
                                 {
                    const glm::vec3 offset = pi - m_Predicted[j];
                    const f32 dist = glm::length(offset);
                    if (dist >= h)
                    {
                        return;
                    }
                    density += mass * FluidKernels::Poly6(dist, h, poly6Scale);
                    if (j != i)
                    {
                        const glm::vec3 grad = FluidKernels::SpikyGrad(offset, h, spikyScale) * (mass * restDensityInv);
                        gradI += grad;
                        gradSum += glm::dot(grad, grad);
                    } });
                m_Density[i] = density;
                const f32 constraint = density * restDensityInv - 1.0f;
                m_Lambda[i] = -constraint / (gradSum + glm::dot(gradI, gradI) + params.CfmEpsilon);
                densitySum += density;
                // Compression only: surface particles are legitimately
                // under-dense (C < 0), that is not an incompressibility error.
                maxError = std::max(maxError, constraint);
            }
            m_LastAverageDensity = densitySum / static_cast<f32>(count);
            m_LastMaxDensityError = maxError;

            // 3b. Position correction + collisions.
            for (u32 i = 0; i < count; ++i)
            {
                const glm::vec3 pi = m_Predicted[i];
                glm::vec3 delta(0.0f);
                ForEachNeighbour(pi, [&](u32 j)
                                 {
                    if (j == i)
                    {
                        return;
                    }
                    const glm::vec3 offset = pi - m_Predicted[j];
                    const f32 dist = glm::length(offset);
                    if (dist >= h)
                    {
                        return;
                    }
                    const f32 w = FluidKernels::Poly6(dist, h, poly6Scale);
                    f32 sCorr = 0.0f;
                    if (sCorrDenom > 0.0f)
                    {
                        const f32 ratio = w / sCorrDenom;
                        sCorr = -params.SCorrK * std::pow(ratio, params.SCorrN);
                    }
                    delta += (m_Lambda[i] + m_Lambda[j] + sCorr) * FluidKernels::SpikyGrad(offset, h, spikyScale); });
                // Delta p_i = (m/rho0) * sum_j (lambda_i + lambda_j + s_corr) * gradW
                // (Macklin & Müller eq. 12, generalised to non-unit mass: every
                // constraint gradient carries m/rho0, so the projection does too).
                glm::vec3 correction = delta * (mass * restDensityInv * kFluidJacobiRelaxation);
                // Explosion guard: cap the per-iteration correction (see
                // kFluidMaxDeltaPFraction). GPU mirror: Fluid_Displace.comp.
                const f32 maxDeltaP = kFluidMaxDeltaPFraction * h;
                const f32 correctionLen2 = glm::dot(correction, correction);
                if (correctionLen2 > maxDeltaP * maxDeltaP)
                {
                    correction *= maxDeltaP / std::sqrt(correctionLen2);
                }
                m_DeltaP[i] = correction;
            }

            for (u32 i = 0; i < count; ++i)
            {
                glm::vec3 p = m_Predicted[i] + m_DeltaP[i];

                // Rigid-body proxies: SDF push-out + reaction impulse.
                for (sizet b = 0; b < bodyProxies.size(); ++b)
                {
                    const FluidBodyProxy& proxy = bodyProxies[b];
                    const glm::quat rotation = ProxyRotation(proxy);
                    const glm::vec3 com(proxy.Position);
                    const glm::vec3 local = glm::conjugate(rotation) * (p - com);
                    glm::vec3 localNormal(0.0f);
                    const f32 signedDist = ProxySignedDistance(proxy, local, localNormal);
                    const f32 penetration = particleRadius - signedDist;
                    if (penetration <= 0.0f)
                    {
                        continue;
                    }
                    const glm::vec3 worldNormal = rotation * localNormal;
                    const glm::vec3 correction = worldNormal * penetration;
                    p += correction;

                    if (b < outBodyFeedback.size())
                    {
                        // Reaction: the impulse that produced the particle's
                        // push-out, scaled by the coupling stiffness. Summed
                        // over all iterations this equals the penetration the
                        // body caused this step (steady state: m*g per second).
                        const glm::vec3 impulse = -correction * (mass * dtInv * params.CouplingStiffness);
                        outBodyFeedback[b].Impulse += impulse;
                        outBodyFeedback[b].AngularImpulse += glm::cross(p - com, impulse);
                    }
                }

                m_Predicted[i] = glm::clamp(p, innerMin, innerMax);
            }
        }

        // The proxy loop above runs once per constraint iteration, and the
        // lambda solve pushes particles back toward a body between iterations,
        // so the raw sum over-counts the physical contact impulse by roughly
        // the iteration count (empirically: a floating box gets catapulted).
        // Average it back to one resolution's worth. GPU mirror:
        // GPUFluidSolver::HarvestFeedback divides by the same factor.
        if (!outBodyFeedback.empty() && params.SolverIterations > 1)
        {
            const f32 invIterations = 1.0f / static_cast<f32>(params.SolverIterations);
            for (FluidBodyFeedback& feedback : outBodyFeedback)
            {
                feedback.Impulse *= invIterations;
                feedback.AngularImpulse *= invIterations;
            }
        }

        // ---- 4. Velocity update ---------------------------------------------
        for (u32 i = 0; i < count; ++i)
        {
            m_Velocities[i] = (m_Predicted[i] - m_Positions[i]) * dtInv;
        }

        // ---- 5. Vorticity omega_i = sum_j vol * (v_j - v_i) x gradW ----------
        // vol = m/rho0 is the SPH volume weight: without it the discrete curl
        // is ~1/vol too large and the confinement force detonates the sim
        // (the unit-mass paper formulas fold vol into their kernel scale).
        const f32 volumeWeight = mass * restDensityInv;
        for (u32 i = 0; i < count; ++i)
        {
            const glm::vec3 pi = m_Predicted[i];
            const glm::vec3 vi = m_Velocities[i];
            glm::vec3 omega(0.0f);
            ForEachNeighbour(pi, [&](u32 j)
                             {
                if (j == i)
                {
                    return;
                }
                const glm::vec3 offset = pi - m_Predicted[j];
                omega += volumeWeight * glm::cross(m_Velocities[j] - vi, FluidKernels::SpikyGrad(offset, h, spikyScale)); });
            m_Omega[i] = omega;
        }

        // ---- 6. Vorticity confinement + XSPH viscosity -----------------------
        for (u32 i = 0; i < count; ++i)
        {
            const glm::vec3 pi = m_Predicted[i];
            const glm::vec3 vi = m_Velocities[i];

            // Vorticity confinement: f = eps * (N x omega), N = normalized
            // gradient of |omega| (Macklin & Müller eq. 16).
            glm::vec3 gradMag(0.0f);
            glm::vec3 xsph(0.0f);
            ForEachNeighbour(pi, [&](u32 j)
                             {
                if (j == i)
                {
                    return;
                }
                const glm::vec3 offset = pi - m_Predicted[j];
                const f32 dist = glm::length(offset);
                if (dist >= h)
                {
                    return;
                }
                gradMag += volumeWeight * glm::length(m_Omega[j]) * FluidKernels::SpikyGrad(offset, h, spikyScale);
                xsph += volumeWeight * (m_Velocities[j] - vi) * FluidKernels::Poly6(dist, h, poly6Scale); });

            glm::vec3 v = vi;
            const f32 gradLen = glm::length(gradMag);
            if (gradLen > 1.0e-6f)
            {
                const glm::vec3 confinement = params.VorticityEpsilon * glm::cross(gradMag / gradLen, m_Omega[i]);
                v += confinement * dt;
            }
            v += params.XsphViscosity * xsph;

            const f32 speed2 = glm::dot(v, v);
            if (speed2 > params.MaxSpeed * params.MaxSpeed)
            {
                v *= params.MaxSpeed / std::sqrt(speed2);
            }
            m_VelocityScratch[i] = v;
        }

        // ---- 7. Commit --------------------------------------------------------
        for (u32 i = 0; i < count; ++i)
        {
            m_Velocities[i] = m_VelocityScratch[i];
            m_Positions[i] = m_Predicted[i];
        }
    }
} // namespace OloEngine
