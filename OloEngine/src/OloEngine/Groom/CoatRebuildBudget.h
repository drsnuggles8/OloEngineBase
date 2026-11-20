#pragma once

#include "OloEngine/Core/Base.h"

#include <algorithm>
#include <vector>

namespace OloEngine::GroomCoatShadow
{
    // THE PER-FRAME COAT-VOLUME REBUILD BUDGET (#1594).
    //
    // A camera cut can move dozens of coats across a coat-LOD step in one
    // frame, and every one of them used to rebuild its volume in that frame:
    // ~44 coats at 40-50 ms each was the acceptance scene's 1-2 s cut hitch.
    // This caps the bakes a frame makes. A coat over the cap keeps drawing the
    // volume it already holds (its rebuild is deferred, and counted) and is
    // served on a later frame.
    //
    // NO STARVATION. A deferred coat waits in a queue ordered by the frame it
    // was first deferred. At the start of each frame the oldest PerFrame
    // waiters are RESERVED: only they, and newcomers while the frame still has
    // budget left after every unserved reservation, may rebuild. A newcomer
    // always ranks behind every waiter, so a coat that keeps being deferred
    // only moves forward and is served within ceil(position / PerFrame)
    // frames of being drawn. A waiter that stops asking (no longer drawn, or
    // its rebuild no longer needed) leaves the queue at the next frame.
    //
    // Keys are the caller's: one per bake, so coats that would share one
    // bake share one ticket.
    class CoatRebuildBudget
    {
      public:
        /// Bakes allowed per frame; 0 is no limit.
        void SetPerFrame(u32 perFrame) noexcept
        {
            m_PerFrame = perFrame;
        }
        [[nodiscard]] u32 PerFrame() const noexcept
        {
            return m_PerFrame;
        }

        /// Starts the frame `frameId` names; a repeat call with the same id is
        /// a no-op, so a caller that may start one frame twice spends one
        /// budget.
        void BeginFrame(u64 frameId)
        {
            if (m_Started && frameId == m_FrameId)
            {
                return;
            }
            m_Started = true;
            m_FrameId = frameId;
            ++m_Frame;

            // A waiter that did not ask last frame no longer needs its rebuild.
            std::erase_if(m_Waiting, [](const Waiter& waiter) { return !waiter.Requested; });
            // Oldest first; equal ages keep the order they arrived in.
            std::ranges::stable_sort(m_Waiting, {}, &Waiter::Since);
            m_UnservedReservations = 0;
            for (sizet i = 0; i < m_Waiting.size(); ++i)
            {
                Waiter& waiter = m_Waiting[i];
                waiter.Requested = false;
                waiter.Reserved = m_PerFrame != 0u && i < m_PerFrame;
                m_UnservedReservations += waiter.Reserved ? 1u : 0u;
            }
            m_Granted = 0;
            m_Deferred = 0;
        }

        /// A coat needs the bake `key` names this frame. True: bake now, and it
        /// is counted against the budget. False: deferred, counted, and queued.
        [[nodiscard]] bool Request(u64 key)
        {
            const auto it = std::ranges::find(m_Waiting, key, &Waiter::Key);
            if (m_PerFrame == 0u)
            {
                if (it != m_Waiting.end())
                {
                    m_Waiting.erase(it);
                }
                ++m_Granted;
                return true;
            }
            if (it != m_Waiting.end() && it->Reserved)
            {
                --m_UnservedReservations;
                m_Waiting.erase(it);
                ++m_Granted;
                return true;
            }
            if (m_Granted + m_UnservedReservations < m_PerFrame)
            {
                if (it != m_Waiting.end())
                {
                    m_Waiting.erase(it);
                }
                ++m_Granted;
                return true;
            }
            ++m_Deferred;
            if (it != m_Waiting.end())
            {
                it->Requested = true;
            }
            else
            {
                m_Waiting.push_back({ key, m_Frame, true, false });
            }
            return false;
        }

        /// Bakes granted, and requests deferred, since BeginFrame.
        [[nodiscard]] u32 GrantedThisFrame() const noexcept
        {
            return m_Granted;
        }
        [[nodiscard]] u32 DeferredThisFrame() const noexcept
        {
            return m_Deferred;
        }
        /// Bakes still queued, and how many frames the oldest has waited.
        [[nodiscard]] u32 Waiting() const noexcept
        {
            return static_cast<u32>(m_Waiting.size());
        }
        [[nodiscard]] u32 OldestWaitFrames() const noexcept
        {
            u64 oldest = m_Frame;
            for (const Waiter& waiter : m_Waiting)
            {
                oldest = std::min(oldest, waiter.Since);
            }
            return static_cast<u32>(m_Frame - oldest);
        }

        /// Forgets every waiter, as when the coats it held are released.
        void Clear() noexcept
        {
            m_Waiting.clear();
            m_UnservedReservations = 0;
        }

      private:
        struct Waiter
        {
            u64 Key = 0;
            u64 Since = 0;
            bool Requested = false;
            bool Reserved = false;
        };
        std::vector<Waiter> m_Waiting;
        u32 m_PerFrame = 0;
        u32 m_Granted = 0;
        u32 m_Deferred = 0;
        u32 m_UnservedReservations = 0;
        u64 m_Frame = 0;
        u64 m_FrameId = 0;
        bool m_Started = false;
    };
} // namespace OloEngine::GroomCoatShadow
