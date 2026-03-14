#include "OloEnginePCH.h"
#include "LockstepManager.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"

namespace OloEngine
{
    LockstepManager::LockstepManager() = default;

    void LockstepManager::SetTickRate(u32 hz)
    {
        if (hz == 0)
        {
            OLO_CORE_WARN("[LockstepManager] SetTickRate(0) is invalid, using 1");
            hz = 1;
        }
        m_TickRate = hz;
    }

    u32 LockstepManager::GetTickRate() const
    {
        return m_TickRate;
    }

    void LockstepManager::SetInputDelay(u32 delayTicks)
    {
        m_InputDelay = delayTicks;
    }

    u32 LockstepManager::GetInputDelay() const
    {
        return m_InputDelay;
    }

    void LockstepManager::SetPeers(std::set<u32> peerIDs)
    {
        m_Peers = std::move(peerIDs);
    }

    void LockstepManager::SubmitInput(u32 localPeerID, std::vector<u8> data)
    {
        u32 const targetTick = m_CurrentTick + m_InputDelay;
        m_InputsByTick[targetTick][localPeerID] = std::move(data);
    }

    void LockstepManager::ReceiveInput(u32 peerID, u32 tick, std::vector<u8> data)
    {
        m_InputsByTick[tick][peerID] = std::move(data);
    }

    bool LockstepManager::HasAllInputsForTick(u32 tick) const
    {
        auto it = m_InputsByTick.find(tick);
        if (it == m_InputsByTick.end())
        {
            return false;
        }

        for (u32 peerID : m_Peers)
        {
            if (it->second.find(peerID) == it->second.end())
            {
                return false;
            }
        }
        return true;
    }

    bool LockstepManager::AdvanceTick(Scene& scene)
    {
        OLO_PROFILE_FUNCTION();

        u32 const nextTick = m_CurrentTick + 1;

        if (!HasAllInputsForTick(nextTick))
        {
            return false;
        }

        // Apply all peer inputs for this tick in deterministic order
        if (m_ApplyCallback)
        {
            auto& inputs = m_InputsByTick[nextTick];
            for (u32 peerID : m_Peers)
            {
                if (auto it = inputs.find(peerID); it != inputs.end())
                {
                    m_ApplyCallback(scene, peerID, it->second.data(), static_cast<u32>(it->second.size()));
                }
            }
        }

        m_CurrentTick = nextTick;
        m_WaitAccumulator = 0.0f;

        // Clean up old inputs (keep some history for debugging)
        if (m_CurrentTick > s_HistoryRetentionTicks)
        {
            u32 const cutoff = m_CurrentTick - s_HistoryRetentionTicks;
            std::erase_if(m_InputsByTick, [cutoff](const auto& entry)
                          { return entry.first < cutoff; });
            std::erase_if(m_LocalHashes, [cutoff](const auto& entry)
                          { return entry.first < cutoff; });
        }

        return true;
    }

    u32 LockstepManager::GetCurrentTick() const
    {
        return m_CurrentTick;
    }

    void LockstepManager::SetHashCheckInterval(u32 interval)
    {
        m_HashCheckInterval = interval;
    }

    void LockstepManager::RecordStateHash(const std::vector<u8>& snapshotData)
    {
        if (m_HashCheckInterval == 0)
        {
            return;
        }

        if (m_CurrentTick % m_HashCheckInterval == 0)
        {
            m_LocalHashes[m_CurrentTick] = StateHash::Compute(snapshotData);
        }
    }

    bool LockstepManager::CompareRemoteHash(u32 peerID, u32 tick, u32 remoteHash)
    {
        auto it = m_LocalHashes.find(tick);
        if (it == m_LocalHashes.end())
        {
            return true; // No local hash for this tick, assume match
        }

        if (it->second != remoteHash)
        {
            OLO_CORE_ERROR("[LockstepManager] Desync detected at tick {} from peer {}! Local hash {:08X} != remote {:08X}",
                           tick, peerID, it->second, remoteHash);
            m_Desynced = true;
            return false;
        }
        return true;
    }

    bool LockstepManager::IsDesynced() const
    {
        return m_Desynced;
    }

    void LockstepManager::ClearDesync()
    {
        m_Desynced = false;
    }

    void LockstepManager::SetInputApplyCallback(InputApplyCallback callback)
    {
        m_ApplyCallback = std::move(callback);
    }

    void LockstepManager::SetInputTimeout(f32 seconds)
    {
        m_InputTimeout = seconds;
    }

    f32 LockstepManager::GetInputTimeout() const
    {
        return m_InputTimeout;
    }

    bool LockstepManager::HasTimedOut() const
    {
        return m_InputTimeout > 0.0f && m_WaitAccumulator >= m_InputTimeout;
    }

    void LockstepManager::AccumulateWaitTime(f32 dt)
    {
        m_WaitAccumulator += dt;
    }
} // namespace OloEngine
