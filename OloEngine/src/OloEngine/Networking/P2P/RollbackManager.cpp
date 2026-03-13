#include "OloEnginePCH.h"
#include "RollbackManager.h"
#include "OloEngine/Networking/Replication/EntitySnapshot.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"

namespace OloEngine
{
    RollbackManager::RollbackManager() = default;

    void RollbackManager::SetMaxRollbackFrames(u32 maxFrames)
    {
        m_MaxRollbackFrames = maxFrames;
    }

    u32 RollbackManager::GetMaxRollbackFrames() const
    {
        return m_MaxRollbackFrames;
    }

    void RollbackManager::SetInputApplyCallback(RollbackInputCallback callback)
    {
        m_ApplyCallback = std::move(callback);
    }

    void RollbackManager::SaveState(u32 tick, Scene& scene)
    {
        OLO_PROFILE_FUNCTION();
        m_StateBuffer.Push(tick, EntitySnapshot::Capture(scene));
    }

    void RollbackManager::SubmitLocalInput(u32 localPeerID, u32 tick, std::vector<u8> data)
    {
        m_Inputs[localPeerID][tick] = std::move(data);
    }

    u32 RollbackManager::ReceiveRemoteInput(u32 peerID, u32 tick, std::vector<u8> data, Scene& scene)
    {
        OLO_PROFILE_FUNCTION();

        m_Inputs[peerID][tick] = std::move(data);

        // If the input is for the current or future tick, no rollback needed
        if (tick >= m_CurrentTick)
        {
            return 0;
        }

        // Check rollback depth
        u32 const rollbackDepth = m_CurrentTick - tick;
        if (rollbackDepth > m_MaxRollbackFrames)
        {
            OLO_CORE_WARN("[RollbackManager] Input {} frames late (max {}), dropping", rollbackDepth,
                          m_MaxRollbackFrames);
            return 0;
        }

        // Perform rollback
        Rollback(tick, scene);
        return rollbackDepth;
    }

    const std::vector<u8>* RollbackManager::GetInputForTick(u32 peerID, u32 tick) const
    {
        auto peerIt = m_Inputs.find(peerID);
        if (peerIt == m_Inputs.end())
        {
            return nullptr;
        }

        // Try exact tick
        auto tickIt = peerIt->second.find(tick);
        if (tickIt != peerIt->second.end())
        {
            return &tickIt->second;
        }

        // Input prediction: find the most recent input before this tick
        const std::vector<u8>* bestInput = nullptr;
        u32 bestTick = 0;
        for (auto const& [t, d] : peerIt->second)
        {
            if (t < tick && t > bestTick)
            {
                bestTick = t;
                bestInput = &d;
            }
        }
        return bestInput;
    }

    u32 RollbackManager::GetCurrentTick() const
    {
        return m_CurrentTick;
    }

    void RollbackManager::SetCurrentTick(u32 tick)
    {
        m_CurrentTick = tick;
    }

    u32 RollbackManager::GetRollbackCount() const
    {
        return m_RollbackCount;
    }

    void RollbackManager::Rollback(u32 toTick, Scene& scene)
    {
        OLO_PROFILE_FUNCTION();

        // Restore state at the rollback tick
        const auto* savedState = m_StateBuffer.GetByTick(toTick);
        if (!savedState)
        {
            OLO_CORE_ERROR("[RollbackManager] No saved state for tick {}", toTick);
            return;
        }

        EntitySnapshot::Apply(scene, savedState->Data);

        // Re-simulate from toTick to current tick
        u32 const originalTick = m_CurrentTick;
        for (u32 tick = toTick + 1; tick <= originalTick; ++tick)
        {
            // Apply all peer inputs for this tick (using real or predicted)
            if (m_ApplyCallback)
            {
                for (auto const& [peerID, tickInputs] : m_Inputs)
                {
                    const auto* input = GetInputForTick(peerID, tick);
                    if (input)
                    {
                        m_ApplyCallback(scene, peerID, input->data(), static_cast<u32>(input->size()));
                    }
                }
            }

            // Re-save state for this tick (with corrected inputs)
            m_StateBuffer.Push(tick, EntitySnapshot::Capture(scene));
        }

        ++m_RollbackCount;
    }
} // namespace OloEngine
