#include "OloEnginePCH.h"
#include "LagCompensator.h"
#include "OloEngine/Networking/Replication/EntitySnapshot.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"

namespace OloEngine
{
    LagCompensator::LagCompensator() = default;

    bool LagCompensator::PerformLagCompensatedCheck(Scene& scene, const SnapshotBuffer& history,
                                                    const LagCompensationParams& params, const RewindCallback& callback)
    {
        OLO_PROFILE_FUNCTION();

        u32 const targetTick = params.TargetTick;
        u32 const currentTick = params.CurrentTick;
        u32 const tickRateHz = params.TickRateHz;

        if (targetTick >= currentTick)
        {
            // Can't rewind to the future
            return false;
        }

        // Clamp rewind duration
        u32 const tickDelta = currentTick - targetTick;
        if (tickRateHz == 0)
        {
            OLO_CORE_WARN("[LagCompensator] tickRateHz is 0, cannot compute rewind duration");
            return false;
        }
        f32 const rewindMs = (static_cast<f32>(tickDelta) / static_cast<f32>(tickRateHz)) * 1000.0f;
        if (rewindMs > m_MaxRewindMs)
        {
            OLO_CORE_WARN("[LagCompensator] Rewind {}ms exceeds max {}ms, rejecting", rewindMs, m_MaxRewindMs);
            return false;
        }

        // Capture current state so we can restore it after the callback
        auto currentState = EntitySnapshot::Capture(scene);

        // Find the snapshot at or nearest to the target tick
        const auto* targetSnapshot = history.GetByTick(targetTick);
        if (!targetSnapshot)
        {
            // Try interpolation between bracketing entries
            auto bracket = history.GetBracketingEntries(targetTick);
            if (!bracket.has_value())
            {
                OLO_CORE_WARN("[LagCompensator] No snapshot data for tick {}", targetTick);
                return false;
            }
            // Use the earlier snapshot (conservative — slightly older state)
            targetSnapshot = bracket->Before;
        }

        // Rewind: apply the historical snapshot
        EntitySnapshot::Apply(scene, targetSnapshot->Data);

        // Execute the lag-compensated callback (e.g. raycast / hit detection)
        // Use RAII to guarantee state restoration even if callback throws
        struct StateRestorer
        {
            Scene& SceneRef;
            const std::vector<u8>& State;
            ~StateRestorer()
            {
                EntitySnapshot::Apply(SceneRef, State);
            }
        } restorer{ scene, currentState };

        callback(scene);

        return true;
    }

    void LagCompensator::SetMaxRewindMs(f32 maxMs)
    {
        OLO_PROFILE_FUNCTION();

        if (maxMs <= 0.0f)
        {
            OLO_CORE_WARN("[LagCompensator] SetMaxRewindMs: invalid value {}ms, must be > 0", maxMs);
            return;
        }
        m_MaxRewindMs = maxMs;
    }

    f32 LagCompensator::GetMaxRewindMs() const
    {
        return m_MaxRewindMs;
    }
} // namespace OloEngine
