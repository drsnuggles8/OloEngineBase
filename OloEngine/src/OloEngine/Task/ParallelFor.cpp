// ParallelFor.cpp - Global configuration for ParallelFor
// Ported from UE5.7 Async/ParallelFor.cpp

#include "OloEnginePCH.h"
#include <optional>
#include "OloEngine/Core/Environment.h"
#include "OloEngine/Task/ParallelFor.h"
#include "OloEngine/HAL/PlatformMisc.h"

#include <mutex>
#include <thread>
#include <cstdlib>
#include <cstring>

namespace OloEngine
{
    // The timeout (in ms) when background priority parallel for task will yield execution
    // to give higher priority tasks the chance to run.
    i32 GParallelForBackgroundYieldingTimeoutMs = 8;

    // If true, do not enable new threads to handle tasks while waiting for a ParallelFor to finish,
    // because new threads can decrease overall performance.
    bool GParallelForDisableOversubscription = false;

    // Whether to use threading for performance-critical code paths
    // Can be disabled for debugging or on single-core systems
    static bool s_ShouldUseThreadingForPerformance = true;

    // One-time initialization guard. ShouldUseThreadingForPerformance() is
    // called from many worker threads concurrently; the previous ad-hoc
    // `bool s_…Initialized` check was a data race. std::call_once provides
    // the happens-before edges we need without each call paying a mutex cost
    // after the first successful initialization.
    static std::once_flag s_ThreadingInitFlag;

    // @brief Initialize threading configuration from environment/command line
    //
    // This matches UE5.7's approach of allowing runtime configuration via
    // command line parameters like -NoThreading, -ForceMultithread, etc.
    static void InitializeThreadingConfiguration()
    {
        // Start with hardware-based decision
        const u32 NumCores = std::thread::hardware_concurrency();
        s_ShouldUseThreadingForPerformance = (NumCores > 1);

        // Startup tuning knobs. Read once, here, through the engine's single
        // environment accessor — these used to be four hand-rolled strcmp /
        // atoi parses that disagreed about what counted as "on".
        if (Env::IsTruthy("OLO_NO_THREADING"))
        {
            s_ShouldUseThreadingForPerformance = false;
        }
        if (Env::IsTruthy("OLO_FORCE_MULTITHREAD"))
        {
            s_ShouldUseThreadingForPerformance = true;
        }
        // Unparseable is ignored rather than silently read as 0, which is what
        // std::atoi did here.
        if (const std::optional<i64> yieldMs = Env::GetInt("OLO_PARALLEL_FOR_YIELD_MS"); yieldMs && *yieldMs >= 0)
        {
            GParallelForBackgroundYieldingTimeoutMs = static_cast<i32>(*yieldMs);
        }
        if (Env::IsTruthy("OLO_DISABLE_OVERSUBSCRIPTION"))
        {
            GParallelForDisableOversubscription = true;
        }
    }

    bool ShouldUseThreadingForPerformance()
    {
        // Race-free lazy init. std::call_once both guarantees single execution
        // and establishes a happens-before edge so the subsequent read of
        // s_ShouldUseThreadingForPerformance sees the writes done inside.
        std::call_once(s_ThreadingInitFlag, InitializeThreadingConfiguration);
        return s_ShouldUseThreadingForPerformance;
    }

} // namespace OloEngine
