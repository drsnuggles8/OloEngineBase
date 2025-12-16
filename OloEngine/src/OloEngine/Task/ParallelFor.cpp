// ParallelFor.cpp - Global configuration for ParallelFor
// Ported from UE5.7 Async/ParallelFor.cpp

#include "OloEngine/Task/ParallelFor.h"
#include "OloEngine/HAL/PlatformMisc.h"

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
	static bool s_bShouldUseThreadingForPerformance = true;

	// Whether s_bShouldUseThreadingForPerformance has been initialized
	static bool s_bThreadingForPerformanceInitialized = false;

	/**
	 * @brief Initialize threading configuration from environment/command line
	 * 
	 * This matches UE5.7's approach of allowing runtime configuration via
	 * command line parameters like -NoThreading, -ForceMultithread, etc.
	 */
	static void InitializeThreadingConfiguration()
	{
		if (s_bThreadingForPerformanceInitialized)
		{
			return;
		}
		s_bThreadingForPerformanceInitialized = true;

		// Start with hardware-based decision
		const u32 NumCores = std::thread::hardware_concurrency();
		s_bShouldUseThreadingForPerformance = (NumCores > 1);

		// Check environment variables for configuration
		// OLO_NO_THREADING=1 disables threading
		if (const char* EnvNoThreading = std::getenv("OLO_NO_THREADING"))
		{
			if (std::strcmp(EnvNoThreading, "1") == 0 || std::strcmp(EnvNoThreading, "true") == 0)
			{
				s_bShouldUseThreadingForPerformance = false;
			}
		}

		// OLO_FORCE_MULTITHREAD=1 forces threading even on single-core
		if (const char* EnvForceMultithread = std::getenv("OLO_FORCE_MULTITHREAD"))
		{
			if (std::strcmp(EnvForceMultithread, "1") == 0 || std::strcmp(EnvForceMultithread, "true") == 0)
			{
				s_bShouldUseThreadingForPerformance = true;
			}
		}

		// OLO_PARALLEL_FOR_YIELD_MS sets the background yield timeout
		if (const char* EnvYieldMs = std::getenv("OLO_PARALLEL_FOR_YIELD_MS"))
		{
			i32 Value = std::atoi(EnvYieldMs);
			if (Value >= 0)
			{
				GParallelForBackgroundYieldingTimeoutMs = Value;
			}
		}

		// OLO_DISABLE_OVERSUBSCRIPTION=1 disables oversubscription
		if (const char* EnvDisableOversub = std::getenv("OLO_DISABLE_OVERSUBSCRIPTION"))
		{
			if (std::strcmp(EnvDisableOversub, "1") == 0 || std::strcmp(EnvDisableOversub, "true") == 0)
			{
				GParallelForDisableOversubscription = true;
			}
		}
	}

	bool ShouldUseThreadingForPerformance()
	{
		// Lazy initialization on first call
		if (!s_bThreadingForPerformanceInitialized)
		{
			InitializeThreadingConfiguration();
		}
		return s_bShouldUseThreadingForPerformance;
	}

} // namespace OloEngine
