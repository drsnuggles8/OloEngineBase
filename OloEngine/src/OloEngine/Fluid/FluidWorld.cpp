#include "OloEnginePCH.h"
#include "OloEngine/Fluid/FluidWorld.h"

#include "OloEngine/Fluid/CPUFluidSolver.h"
#include "OloEngine/Fluid/GPUFluidSolver.h"

namespace OloEngine
{
    // Out-of-line so Scope<CPUFluidSolver>/Scope<GPUFluidSolver> destroy with
    // complete types (the header only forward-declares them).
    FluidInstance::FluidInstance() = default;
    FluidInstance::~FluidInstance() = default;
    FluidInstance::FluidInstance(FluidInstance&&) noexcept = default;
    FluidInstance& FluidInstance::operator=(FluidInstance&&) noexcept = default;

    FluidInstance& FluidWorld::GetOrCreate(UUID entityID)
    {
        return m_Instances[static_cast<u64>(entityID)];
    }

    FluidInstance* FluidWorld::Find(UUID entityID)
    {
        auto it = m_Instances.find(static_cast<u64>(entityID));
        return it != m_Instances.end() ? &it->second : nullptr;
    }

    void FluidWorld::Sweep(u64 currentTick)
    {
        for (auto it = m_Instances.begin(); it != m_Instances.end();)
        {
            if (it->second.LastTouchedTick != currentTick)
            {
                it = m_Instances.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void FluidWorld::Clear()
    {
        m_Instances.clear();
    }
} // namespace OloEngine
