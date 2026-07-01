#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    // Frame pacing for the windowed main loop (issue #456): an optional
    // frame-rate cap (coarse sleep + short busy-spin to hit a target frame time)
    // plus exponential-moving-average smoothing of the per-frame delta handed to
    // consumers.
    //
    // The cap is designed to COMPOSE with vsync rather than fight it: the loop
    // calls LimitFrameRate AFTER SwapBuffers, so when vsync has already consumed
    // the frame budget the remaining wait is <= 0 and nothing sleeps (no
    // double-throttle). A cap slower than the vsync rate throttles the extra
    // time; a cap faster than vsync is simply dominated by vsync.
    //
    // The precision-sensitive parts live behind PURE, wall-clock-free static
    // helpers (ComputeSleepSeconds / SmoothFrameTime) so the math can be unit
    // tested without any timing flakiness. The blocking wait (LimitFrameRate)
    // reads the real clock through Utils::Time and is intentionally not unit
    // tested — it is validated by feel in the running editor.
    //
    // On Windows the default scheduler tick (~15.6 ms) is far too coarse for a
    // frame limiter, so while a cap is active the pacer raises the process timer
    // resolution to 1 ms (timeBeginPeriod, winmm) and releases it when the cap is
    // turned off or the pacer is destroyed.
    class FramePacer
    {
      public:
        // Sentinel target FPS meaning "no cap" (the limiter is a no-op).
        static constexpr u32 kUncapped = 0;

        FramePacer() = default;
        ~FramePacer();

        // Owns a process-wide timer-resolution request on Windows — copying would
        // double-release it. The pacer lives as a single Application member.
        FramePacer(const FramePacer&) = delete;
        FramePacer& operator=(const FramePacer&) = delete;

        // 0 (kUncapped) disables the cap. Any positive value is the target
        // frames-per-second. Toggling the cap raises/releases the OS timer
        // resolution on Windows, so this is defined out-of-line.
        void SetTargetFPS(u32 fps);
        [[nodiscard]] u32 GetTargetFPS() const
        {
            return m_TargetFPS;
        }
        [[nodiscard]] bool IsCapEnabled() const
        {
            return m_TargetFPS != kUncapped;
        }

        // Target frame time in seconds for the current cap, or 0 when uncapped.
        [[nodiscard]] f32 GetTargetFrameTime() const
        {
            return m_TargetFPS == kUncapped ? 0.0f : 1.0f / static_cast<f32>(m_TargetFPS);
        }

        // EMA weight for the newest sample, in [kMinSmoothingAlpha, 1]. 1.0 is
        // the default and disables smoothing (the returned delta equals the raw
        // delta bit-for-bit); smaller values smooth more heavily. Non-finite or
        // out-of-range inputs are sanitized.
        void SetSmoothingFactor(f32 alpha);
        [[nodiscard]] f32 GetSmoothingFactor() const
        {
            return m_SmoothingAlpha;
        }
        [[nodiscard]] bool IsSmoothingEnabled() const
        {
            return m_SmoothingAlpha < 1.0f;
        }

        // Feed the raw per-frame delta (seconds); returns the EMA-smoothed delta
        // and stores it. The first call (or first after Reset) seeds the average
        // with rawDelta. A non-finite rawDelta is ignored (the previous smoothed
        // value is returned unchanged).
        f32 SmoothDelta(f32 rawDelta);
        [[nodiscard]] f32 GetSmoothedDelta() const
        {
            return m_SmoothedDelta;
        }

        // Forget the smoothing history so the next SmoothDelta re-seeds. Call
        // after a long stall (level load, breakpoint) so a huge stale delta
        // doesn't bleed into subsequent frames.
        void Reset()
        {
            m_HasSmoothedDelta = false;
        }

        // Block (coarse sleep + short busy-spin) until GetTargetFrameTime()
        // seconds have elapsed since frameStartTime (both from Utils::Time). A
        // no-op when uncapped, when the budget is already spent, or when the
        // clock is frozen (deterministic capture/test via Time::SetMockTime).
        void LimitFrameRate(f32 frameStartTime);

        // ---- Pure, wall-clock-free math (unit tested) --------------------------

        // How long to coarse-sleep given the time still remaining in the frame
        // budget and a busy-spin safety margin. Returns 0 when the remaining time
        // is within the margin (spin-only) or already spent, so the caller spins
        // the final margin for accuracy instead of trusting the OS sleep.
        [[nodiscard]] static f32 ComputeSleepSeconds(f32 remaining, f32 spinMargin);

        // One EMA step: previous + alpha * (sample - previous). Sanitizes
        // non-finite inputs (a bad sample keeps previous; a bad previous adopts
        // the sample; a bad alpha is treated as 1.0) and clamps alpha to [0, 1].
        [[nodiscard]] static f32 SmoothFrameTime(f32 previous, f32 sample, f32 alpha);

      private:
        void UpdateTimerResolution();

        u32 m_TargetFPS = kUncapped;
        f32 m_SmoothingAlpha = 1.0f; // 1.0 == smoothing disabled
        f32 m_SmoothedDelta = 0.0f;
        bool m_HasSmoothedDelta = false;
        bool m_TimerResolutionRaised = false;

        // Coarse-sleep until this much of the budget remains, then busy-spin the
        // rest. ~1 ms comfortably covers 1 ms-timer-resolution wake-up jitter on
        // Windows without a system-wide power-hungrier resolution bump.
        static constexpr f32 kSpinMarginSeconds = 0.001f;
        // Never let alpha reach 0 — a 0 weight would freeze the average forever.
        static constexpr f32 kMinSmoothingAlpha = 0.001f;
    };
} // namespace OloEngine
