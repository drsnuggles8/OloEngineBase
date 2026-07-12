#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Fluid/FluidSolverTypes.h"

#include <glm/glm.hpp>
#include <span>
#include <vector>

namespace OloEngine
{
    /// World-space axis-aligned kill box: particles inside are removed at the
    /// start of a step (drains, level-exit volumes).
    struct FluidKillBox
    {
        glm::vec3 Min{ 0.0f };
        glm::vec3 Max{ 0.0f };
    };

    // =========================================================================
    // CPUFluidSolver — deterministic reference implementation of the
    // Position-Based Fluids step (Macklin & Müller 2013).
    //
    // This is the ground truth the GPU solver is parity-tested against, the
    // backend for headless contexts (OloServer, Functional tests — no GL), and
    // the OLO_FLUID_SEQUENTIAL determinism fallback. Every loop is serial and
    // index-ordered: two runs over the same inputs are bit-identical.
    //
    // The neighbour search is the same dense-grid linked-list scheme the GPU
    // uses (head[cell] stores index+1, 0 = empty), built once per step from
    // the predicted positions.
    // =========================================================================
    class CPUFluidSolver
    {
      public:
        CPUFluidSolver() = default;
        explicit CPUFluidSolver(u32 maxParticles);

        /// Drop all particles and reallocate for a new capacity.
        void Reset(u32 maxParticles);

        /// Append staged particles (clamped to remaining capacity).
        void Emit(std::span<const GPUFluidEmitEntry> entries);

        /// Advance one fixed step. `outBodyFeedback`, when non-empty, must be
        /// the same length as `bodyProxies`; it receives the accumulated
        /// reaction impulses (zeroed first).
        void Step(const FluidSolverParams& params, f32 dt,
                  std::span<const FluidBodyProxy> bodyProxies = {},
                  std::span<FluidBodyFeedback> outBodyFeedback = {},
                  std::span<const FluidKillBox> killBoxes = {});

        [[nodiscard]] u32 GetCount() const
        {
            return static_cast<u32>(m_Positions.size());
        }
        [[nodiscard]] u32 GetMaxParticles() const
        {
            return m_MaxParticles;
        }
        [[nodiscard]] std::span<const glm::vec3> GetPositions() const
        {
            return m_Positions;
        }
        [[nodiscard]] std::span<const glm::vec3> GetVelocities() const
        {
            return m_Velocities;
        }

        /// Density statistics from the final constraint iteration of the last
        /// Step() — the contract tests' incompressibility probes.
        [[nodiscard]] f32 GetLastAverageDensity() const
        {
            return m_LastAverageDensity;
        }
        [[nodiscard]] f32 GetLastMaxDensityError() const
        {
            return m_LastMaxDensityError;
        }

      private:
        void BuildGrid(const FluidSolverParams& params);

        /// Visit the indices of every particle in the 27-cell neighbourhood of
        /// `position` (includes the querying particle itself when present).
        template<typename Fn>
        void ForEachNeighbour(const glm::vec3& position, Fn&& fn) const;

        u32 m_MaxParticles = 0;

        std::vector<glm::vec3> m_Positions;
        std::vector<glm::vec3> m_Velocities;
        std::vector<glm::vec3> m_Predicted;
        std::vector<glm::vec3> m_DeltaP;
        std::vector<glm::vec3> m_Omega;
        std::vector<f32> m_Lambda;
        std::vector<f32> m_Density;
        std::vector<glm::vec3> m_VelocityScratch;

        // Dense uniform grid (linked lists): head stores index+1, 0 = empty.
        std::vector<u32> m_GridHead;
        std::vector<u32> m_GridNext;
        glm::uvec3 m_GridDims{ 0 };
        glm::vec3 m_GridOrigin{ 0.0f };
        f32 m_CellSize = 0.0f;

        f32 m_LastAverageDensity = 0.0f;
        f32 m_LastMaxDensityError = 0.0f;
    };
} // namespace OloEngine
