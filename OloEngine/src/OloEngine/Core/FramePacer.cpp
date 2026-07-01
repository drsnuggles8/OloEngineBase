#include "OloEnginePCH.h"
#include "OloEngine/Core/FramePacer.h"

#include "OloEngine/Utils/PlatformUtils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#ifdef OLO_PLATFORM_WINDOWS
#include <Windows.h>
#include <timeapi.h> // timeBeginPeriod / timeEndPeriod (winmm)
#endif

namespace OloEngine
{
    FramePacer::~FramePacer()
    {
        // Release the OS timer-resolution request if we still hold it.
        m_TargetFPS = kUncapped;
        UpdateTimerResolution();
    }

    void FramePacer::SetTargetFPS(u32 fps)
    {
        m_TargetFPS = fps;
        UpdateTimerResolution();
    }

    void FramePacer::UpdateTimerResolution()
    {
        const bool wantHigh = (m_TargetFPS != kUncapped);
        if (wantHigh == m_TimerResolutionRaised)
        {
            return;
        }

#ifdef OLO_PLATFORM_WINDOWS
        if (wantHigh)
        {
            ::timeBeginPeriod(1);
        }
        else
        {
            ::timeEndPeriod(1);
        }
#endif

        m_TimerResolutionRaised = wantHigh;
    }

    void FramePacer::SetSmoothingFactor(f32 alpha)
    {
        if (!std::isfinite(alpha))
        {
            alpha = 1.0f;
        }
        m_SmoothingAlpha = std::clamp(alpha, kMinSmoothingAlpha, 1.0f);
    }

    f32 FramePacer::SmoothDelta(f32 rawDelta)
    {
        if (!m_HasSmoothedDelta)
        {
            m_SmoothedDelta = std::isfinite(rawDelta) ? rawDelta : 0.0f;
            m_HasSmoothedDelta = true;
            return m_SmoothedDelta;
        }

        m_SmoothedDelta = SmoothFrameTime(m_SmoothedDelta, rawDelta, m_SmoothingAlpha);
        return m_SmoothedDelta;
    }

    void FramePacer::LimitFrameRate(f32 frameStartTime)
    {
        const f32 targetFrameTime = GetTargetFrameTime();
        if (targetFrameTime <= 0.0f)
        {
            return; // uncapped
        }

        // A frozen clock (deterministic capture/test) never advances — spinning
        // on it would hang the loop forever, so leave the pacing to real time.
        if (Time::HasMockTime())
        {
            return;
        }

        for (;;)
        {
            const f32 elapsed = Time::GetTime() - frameStartTime;
            const f32 remaining = targetFrameTime - elapsed;
            if (!std::isfinite(remaining) || remaining <= 0.0f)
            {
                break;
            }

            if (const f32 sleepSeconds = ComputeSleepSeconds(remaining, kSpinMarginSeconds); sleepSeconds > 0.0f)
            {
                std::this_thread::sleep_for(std::chrono::duration<f32>(sleepSeconds));
            }
            else
            {
                // Busy-spin the final sub-millisecond for accuracy the OS sleep
                // can't deliver; yield so a hyperthread sibling can progress.
                std::this_thread::yield();
            }
        }
    }

    f32 FramePacer::ComputeSleepSeconds(f32 remaining, f32 spinMargin)
    {
        if (!std::isfinite(spinMargin) || spinMargin < 0.0f)
        {
            spinMargin = 0.0f;
        }
        if (!std::isfinite(remaining) || remaining <= spinMargin)
        {
            return 0.0f;
        }
        return remaining - spinMargin;
    }

    f32 FramePacer::SmoothFrameTime(f32 previous, f32 sample, f32 alpha)
    {
        if (!std::isfinite(sample))
        {
            return previous;
        }
        if (!std::isfinite(previous))
        {
            return sample;
        }
        if (!std::isfinite(alpha))
        {
            alpha = 1.0f;
        }
        alpha = std::clamp(alpha, 0.0f, 1.0f);
        return previous + alpha * (sample - previous);
    }
} // namespace OloEngine
