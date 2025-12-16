// CoreGlobals.h - Global engine variables
// Ported from UE5.7 CoreGlobals.h (subset)

#pragma once

/**
 * @file CoreGlobals.h
 * @brief Global engine state variables
 *
 * Contains global thread IDs and other core engine state.
 * Ported subset from Unreal Engine 5.7
 */

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    /**
     * @brief Thread ID of the game/main thread
     * Set during engine initialization
     */
    inline u32 GGameThreadId = 0;

    /**
     * @brief Thread ID of the render thread
     * Set when the render thread is created (0 if single-threaded rendering)
     */
    inline u32 GRenderThreadId = 0;

    /**
     * @brief Thread ID of the RHI thread
     * Set when the RHI thread is created (0 if not using separate RHI thread)
     */
    inline u32 GRHIThreadId = 0;

    /**
     * @brief Whether the engine is running in multithreaded mode
     */
    inline bool GIsThreadedRendering = false;

    /**
     * @brief Check if the current thread is the game thread
     */
    bool IsInGameThread();

    /**
     * @brief Check if the current thread is the render thread
     */
    bool IsInRenderingThread();

    /**
     * @brief Check if the current thread is the RHI thread
     */
    bool IsInRHIThread();

} // namespace OloEngine

//=============================================================================
// IMPLEMENTATION
//=============================================================================

#include "OloEngine/Core/PlatformTLS.h"

namespace OloEngine
{
    inline bool IsInGameThread()
    {
        if (GGameThreadId == 0)
        {
            // Not yet initialized, assume we're on game thread during startup
            return true;
        }
        return FPlatformTLS::GetCurrentThreadId() == GGameThreadId;
    }

    inline bool IsInRenderingThread()
    {
        if (!GIsThreadedRendering || GRenderThreadId == 0)
        {
            // Single-threaded rendering: game thread is also render thread
            return IsInGameThread();
        }
        return FPlatformTLS::GetCurrentThreadId() == GRenderThreadId;
    }

    inline bool IsInRHIThread()
    {
        if (GRHIThreadId == 0)
        {
            // No separate RHI thread: check if we're on render thread
            return IsInRenderingThread();
        }
        return FPlatformTLS::GetCurrentThreadId() == GRHIThreadId;
    }

} // namespace OloEngine

