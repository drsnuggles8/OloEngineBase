#pragma once

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Debug/GPUTimingStatus.h"
#include "OloEngine/Threading/Mutex.h"
#include "OloEngine/Threading/UniqueLock.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace OloEngine
{
    // Explicitly armed diagnostic storage. Allocation and page touching happen
    // only on Arm; frames and events reuse their slots until the next Arm.
    // All mutation (including worker completions) shares one lock. A token
    // identifies its session AND frame so a late destructor cannot corrupt a
    // wrapped slot or a new recording. Dump copies only completed frames.
    class CpuScopeRing
    {
      public:
        struct Event
        {
            std::array<char, 96> Name{};
            u64 Thread = 0;
            u32 Depth = 0;
            f64 StartMs = 0;
            f64 DurationMs = 0;
            bool Complete = false;
            bool NameTruncated = false;
        };

        struct Metrics
        {
            f64 RendererCpuMs = 0;
            f64 GpuMs = 0;
            GpuTimingStatus GpuStatus = GpuTimingStatus::Unavailable;
            u64 GpuFrameId = 0;
            u64 GpuIssueFrameId = 0;
            u64 RendererFrameId = 0;
            bool RendererFrameAdvanced = false;
            bool GpuFrameAdvanced = false;
        };

        struct Frame
        {
            u64 Id = 0;
            u64 StartNs = 0;
            f64 ElapsedMs = 0;
            Metrics Render;
            // Derived only when taking a snapshot; raw delayed observations remain in Render.
            GpuTimingSample SameFrameGpu;
            u32 EventCount = 0;
            u32 EventOffset = 0; // Offset into Snapshot::Events; unused in live slots.
            u32 DroppedEvents = 0;
            bool Complete = false;
        };

        struct Token
        {
            u64 Session = 0;
            u64 FrameId = 0;
            u32 Slot = 0;
            u32 EventIndex = 0;
            u64 StartNs = 0;
        };

        struct Snapshot
        {
            bool Armed = false;
            u32 FrameCapacity = 0;
            u32 EventsPerFrame = 0;
            u64 FramesWritten = 0;
            u64 FramesOverwritten = 0;
            u64 LateCompletions = 0;
            u64 PartialFramesDiscarded = 0;
            u64 RetainedBytes = 0;
            TArray<Frame> Frames;
            TArray<Event> Events;
        };

        [[nodiscard]] static u64 NowNs()
        {
            return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
        }

        void Arm(u32 frames = 512, u32 eventsPerFrame = 1024)
        {
            const u64 bytes = static_cast<u64>(frames) * (sizeof(Frame) + static_cast<u64>(eventsPerFrame) * sizeof(Event));
            if (frames < 2 || frames > 2048 || eventsPerFrame < 1 || eventsPerFrame > 4096 || bytes > 256ULL * 1024 * 1024)
                throw std::invalid_argument("CPU scope ring needs 2..2048 frames, 1..4096 events/frame and at most 256 MiB.");

            // Allocate before publishing, so failure leaves the old recording intact.
            TArray<Frame> newFrames;
            TArray<Event> newEvents;
            newFrames.SetNum(static_cast<i32>(frames));
            newEvents.SetNum(static_cast<i32>(frames * eventsPerFrame));
            TUniqueLock<FMutex> lock(m_Mutex);
            m_Armed.store(false, std::memory_order_relaxed);
            m_Frames = std::move(newFrames);
            m_Events = std::move(newEvents);
            m_EventsPerFrame = eventsPerFrame;
            ++m_Session;
            m_CurrentSlot = 0;
            m_NextId = 0;
            m_FramesWritten = 0;
            m_LateCompletions = 0;
            m_PartialFramesDiscarded = 0;
            m_FrameOpen = false;
            m_Armed.store(true, std::memory_order_release);
        }

        void Disarm()
        {
            TUniqueLock<FMutex> lock(m_Mutex);
            if (m_FrameOpen)
                ++m_PartialFramesDiscarded;
            m_Armed.store(false, std::memory_order_release);
            m_FrameOpen = false; // A partial frame is never offered as a complete measurement.
        }

        [[nodiscard]] bool IsArmed() const
        {
            return m_Armed.load(std::memory_order_acquire);
        }

        void BeginFrame(u64 startNs)
        {
            if (!IsArmed())
                return;
            TUniqueLock<FMutex> lock(m_Mutex);
            if (!IsArmed())
                return;
            if (m_FrameOpen)
                ++m_PartialFramesDiscarded;
            m_CurrentSlot = static_cast<u32>(m_NextId % static_cast<u64>(m_Frames.Num()));
            auto& frame = m_Frames[static_cast<i32>(m_CurrentSlot)];
            frame = Frame{};
            frame.Id = ++m_NextId;
            frame.StartNs = startNs;
            m_FrameOpen = true;
        }

        [[nodiscard]] Token BeginScope(std::string_view name, u32 depth, u64 thread, u64 startNs)
        {
            if (!IsArmed())
                return {};
            TUniqueLock<FMutex> lock(m_Mutex);
            if (!IsArmed() || !m_FrameOpen)
                return {};
            auto& frame = m_Frames[static_cast<i32>(m_CurrentSlot)];
            if (frame.EventCount == m_EventsPerFrame)
            {
                ++frame.DroppedEvents;
                return {};
            }
            const u32 index = frame.EventCount++;
            auto& event = At(m_CurrentSlot, index);
            event = Event{};
            const sizet length = std::min(name.size(), event.Name.size() - 1);
            std::copy_n(name.data(), length, event.Name.data());
            event.NameTruncated = name.size() > length;
            event.Thread = thread;
            event.Depth = depth;
            event.StartMs = Millis(frame.StartNs, startNs);
            return { m_Session, frame.Id, m_CurrentSlot, index, startNs };
        }

        void EndScope(const Token& token, u64 endNs)
        {
            if (token.FrameId == 0 || !IsArmed())
                return;
            TUniqueLock<FMutex> lock(m_Mutex);
            if (!IsArmed() || token.Session != m_Session)
                return;
            if (m_Frames[static_cast<i32>(token.Slot)].Id != token.FrameId)
            {
                ++m_LateCompletions;
                return;
            }
            auto& event = At(token.Slot, token.EventIndex);
            event.DurationMs = Millis(token.StartNs, endNs);
            event.Complete = true;
        }

        void EndFrame(u64 endNs, const Metrics& metrics)
        {
            if (!IsArmed())
                return;
            TUniqueLock<FMutex> lock(m_Mutex);
            if (!IsArmed() || !m_FrameOpen)
                return;
            auto& frame = m_Frames[static_cast<i32>(m_CurrentSlot)];
            frame.ElapsedMs = Millis(frame.StartNs, endNs);
            frame.Render = metrics;
            frame.Complete = true;
            m_FrameOpen = false;
            ++m_FramesWritten;
        }

        // Threshold applies to whole-loop elapsed time. Context is in retained
        // chronological frames, clipped at either end and deduplicated.
        [[nodiscard]] Snapshot Capture(f64 thresholdMs = 0, u32 context = 0, bool includeFrames = true) const
        {
            if (!std::isfinite(thresholdMs) || thresholdMs < 0 || context > 2048)
                throw std::invalid_argument("Threshold must be finite and nonnegative; context must be at most 2048.");
            TUniqueLock<FMutex> lock(m_Mutex);
            Snapshot out;
            out.Armed = IsArmed();
            out.FrameCapacity = static_cast<u32>(m_Frames.Num());
            out.EventsPerFrame = m_EventsPerFrame;
            out.FramesWritten = m_FramesWritten;
            out.LateCompletions = m_LateCompletions;
            out.PartialFramesDiscarded = m_PartialFramesDiscarded;
            out.RetainedBytes = static_cast<u64>(m_Frames.GetAllocatedSize()) + static_cast<u64>(m_Events.GetAllocatedSize());
            if (m_Frames.IsEmpty())
                return out;

            TArray<u32> ordered;
            const u64 firstId = m_NextId > out.FrameCapacity ? m_NextId - out.FrameCapacity + 1 : 1;
            for (u64 id = firstId; id <= m_NextId; ++id)
            {
                const auto slot = static_cast<u32>((id - 1) % out.FrameCapacity);
                if (m_Frames[static_cast<i32>(slot)].Complete)
                    ordered.Add(slot);
            }
            out.FramesOverwritten = m_FramesWritten - static_cast<u64>(ordered.Num());
            if (!includeFrames)
                return out;

            const i32 count = ordered.Num();
            TArray<u8> selected;
            selected.SetNumZeroed(count);
            for (i32 i = 0; i < count; ++i)
            {
                if (m_Frames[static_cast<i32>(ordered[i])].ElapsedMs < thresholdMs)
                    continue;
                const i32 begin = std::max(0, i - static_cast<i32>(context));
                const i32 end = std::min(count - 1, i + static_cast<i32>(context));
                for (i32 j = begin; j <= end; ++j)
                    selected[j] = 1;
            }
            for (i32 i = 0; i < count; ++i)
            {
                if (selected[i] == 0)
                    continue;
                const u32 slot = ordered[i];
                Frame captured = m_Frames[static_cast<i32>(slot)];
                captured.SameFrameGpu = GpuTimingSample::Absent(
                    captured.Render.GpuFrameAdvanced ? GpuTimingStatus::Pending : GpuTimingStatus::Unavailable);
                if (captured.Render.GpuFrameAdvanced)
                {
                    // Search all retained observations, even those outside the
                    // selected context. Only later reports can resolve this issue.
                    for (i32 j = i; j < count; ++j)
                    {
                        const auto& observation = m_Frames[static_cast<i32>(ordered[j])].Render;
                        // A timer-pool reset starts a new identity sequence.
                        if (j > i && observation.GpuIssueFrameId < m_Frames[static_cast<i32>(ordered[j - 1])].Render.GpuIssueFrameId)
                            break;
                        if (observation.GpuFrameId != 0 && observation.GpuFrameId == captured.Render.GpuIssueFrameId)
                        {
                            captured.SameFrameGpu = { observation.GpuMs, observation.GpuStatus };
                            break;
                        }
                    }
                }
                captured.EventOffset = static_cast<u32>(out.Events.Num());
                for (u32 j = 0; j < captured.EventCount; ++j)
                    out.Events.Add(At(slot, j));
                out.Frames.Add(std::move(captured));
            }
            return out;
        }

      private:
        [[nodiscard]] static f64 Millis(u64 begin, u64 end)
        {
            return end >= begin ? static_cast<f64>(end - begin) / 1000000.0 : 0.0;
        }
        Event& At(u32 slot, u32 index)
        {
            return m_Events[static_cast<i32>(slot * m_EventsPerFrame + index)];
        }
        const Event& At(u32 slot, u32 index) const
        {
            return m_Events[static_cast<i32>(slot * m_EventsPerFrame + index)];
        }

        mutable FMutex m_Mutex;
        std::atomic<bool> m_Armed{ false };
        TArray<Frame> m_Frames;
        TArray<Event> m_Events;
        u32 m_EventsPerFrame = 0;
        u32 m_CurrentSlot = 0;
        u64 m_Session = 0;
        u64 m_NextId = 0;
        u64 m_FramesWritten = 0;
        u64 m_LateCompletions = 0;
        u64 m_PartialFramesDiscarded = 0;
        bool m_FrameOpen = false;
    };

} // namespace OloEngine
