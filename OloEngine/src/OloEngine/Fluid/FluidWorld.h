#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/FastRandom.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Fluid/FluidSolverTypes.h"

#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class CPUFluidSolver;
    class GPUFluidSolver;

    /// Runtime state for one FluidComponent entity: exactly one solver backend
    /// plus emission/coupling bookkeeping. NOT an ECS component — lives in the
    /// scene-owned FluidWorld registry so component structs stay trivially
    /// copyable and GPU teardown stays on the game thread.
    struct FluidInstance
    {
        Scope<CPUFluidSolver> Cpu;
        Scope<GPUFluidSolver> Gpu;

        // Proxies passed to the previous GPU Step — HarvestFeedback's results
        // correspond to these (one-step readback latency), so the impulses are
        // applied to these bodies, not this tick's fresh overlap set.
        std::vector<FluidBodyProxy> PrevProxies;
        std::vector<UUID> PrevProxyEntities;

        std::unordered_map<u64, f32> EmitCarry; // emitter entity UUID -> fractional particles carried
        FastRandomPCG EmitRng;                  // deterministic emission jitter (seeded per fluid entity)

        bool Prefilled = false;
        bool UsingGpu = false;
        u32 ConfiguredMaxParticles = 0;
        u64 LastTouchedTick = 0;

        FluidInstance();
        ~FluidInstance();
        FluidInstance(FluidInstance&&) noexcept;
        FluidInstance& operator=(FluidInstance&&) noexcept;
        FluidInstance(const FluidInstance&) = delete;
        FluidInstance& operator=(const FluidInstance&) = delete;
    };

    /// Per-Scene registry of fluid solver instances, keyed by entity UUID.
    /// Instances whose entity (or FluidComponent) disappears are destroyed by
    /// Sweep() on the game thread — no OnComponentRemoved hook needed.
    class FluidWorld
    {
      public:
        FluidInstance& GetOrCreate(UUID entityID);
        [[nodiscard]] FluidInstance* Find(UUID entityID);

        /// Destroy every instance whose LastTouchedTick != currentTick.
        void Sweep(u64 currentTick);

        void Clear();

        [[nodiscard]] sizet GetInstanceCount() const
        {
            return m_Instances.size();
        }

        /// Monotonic tick used by the touch-and-sweep lifetime scheme; advanced
        /// once per FluidSystem::OnUpdate.
        [[nodiscard]] u64 AdvanceTick()
        {
            return ++m_Tick;
        }

      private:
        std::unordered_map<u64, FluidInstance> m_Instances;
        u64 m_Tick = 0;
    };
} // namespace OloEngine
