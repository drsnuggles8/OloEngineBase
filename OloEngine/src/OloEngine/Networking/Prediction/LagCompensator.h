#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/Replication/SnapshotBuffer.h"

#include <functional>
#include <vector>

namespace OloEngine
{
    class Scene;

    // Named parameter bundle for lag compensation checks.
    struct LagCompensationParams
    {
        u32 TargetTick = 0;
        u32 CurrentTick = 0;
        u32 TickRateHz = 0;
    };

    // Server-side lag compensation.
    // Temporarily rewinds entity positions to a past tick using the SnapshotBuffer,
    // executes a callback (e.g. hit detection), then restores the current state.
    class LagCompensator
    {
      public:
        LagCompensator();

        // Rewind the scene to the given tick using snapshot history, execute the callback,
        // then restore the current state. Rejects rewind if duration exceeds MaxRewindMs.
        // Returns true if rewind was performed, false if tick was out of range or rejected.
        using RewindCallback = std::function<void(Scene&)>;
        bool PerformLagCompensatedCheck(Scene& scene, const SnapshotBuffer& history,
                                        const LagCompensationParams& params, const RewindCallback& callback);

        // Set the maximum allowed rewind in milliseconds (default 200ms).
        void SetMaxRewindMs(f32 maxMs);
        [[nodiscard]] f32 GetMaxRewindMs() const;

      private:
        f32 m_MaxRewindMs = 200.0f;
    };
} // namespace OloEngine
