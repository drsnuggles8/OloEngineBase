#pragma once

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Core/Base.h"

#include <algorithm>

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
    // its rebuild no longer needed) leaves the queue at the next frame. The
    // reservation is decided before the frame knows who will ask, so on the
    // frame a camera cuts away from waiting coats their reservations go unused
    // and the coats now in view wait one frame more; deciding inline, frame by
    // frame, cannot know the frame's requests in advance.
    //
    // A FORCED bake (Force) is one the caller must not defer -- a moving
    // coat's drift rebake, which a deferral would turn into a shadow sliding
    // off the coat. It always runs, is counted, and spends the budget like any
    // other, so on a frame of moving coats fewer other rebuilds run; only
    // forced bakes take a frame past PerFrame.
    //
    // Keys are the caller's, one per COAT, so a coat keeps its place in the
    // queue when what it needs changes while it waits (another LOD step).
    class CoatRebuildBudget
    {
      public:
        /// Passes of one frame id, at most, before a new frame is started
        /// anyway: a host that never advances the frame id must not freeze the
        /// budget, and no frame draws grooms through this many views.
        static constexpr u64 kMaxPassesPerFrame = 8;

        /// Bakes allowed per frame; 0 is no limit.
        void SetPerFrame(u32 perFrame) noexcept
        {
            m_PerFrame = perFrame;
        }
        [[nodiscard]] u32 PerFrame() const noexcept
        {
            return m_PerFrame;
        }

        /// Starts the frame `frameId` names, from the pass tick `passTick`
        /// that is running it. Every pass of one frame id -- one per view --
        /// spends the same budget; kMaxPassesPerFrame passes of an unchanged id
        /// start the next frame anyway. True when this call started a frame,
        /// so a per-pass report can count the frame's deferrals once.
        bool BeginFrame(u64 frameId, u64 passTick)
        {
            if (m_Started && frameId == m_FrameId && passTick - m_FrameStartTick < kMaxPassesPerFrame)
            {
                return false;
            }
            m_Started = true;
            m_FrameId = frameId;
            m_FrameStartTick = passTick;
            ++m_Frame;

            // A waiter that did not ask last frame no longer needs its rebuild.
            // The queue stays oldest first: a waiter is appended at the frame it
            // was first deferred and removals keep the order.
            m_Waiting.RemoveAll([](const Waiter& waiter)
                                { return !waiter.Requested; });
            m_UnservedReservations = 0;
            for (i32 i = 0; i < m_Waiting.Num(); ++i)
            {
                Waiter& waiter = m_Waiting[i];
                waiter.Requested = false;
                waiter.Reserved = m_PerFrame != 0u && static_cast<u32>(i) < m_PerFrame;
                m_UnservedReservations += waiter.Reserved ? 1u : 0u;
            }
            m_Granted = 0;
            m_Deferred = 0;
            m_Forced = 0;
            return true;
        }

        /// A bake that must run this frame (see the class comment): granted
        /// whatever the budget says, counted, and spending it.
        void Force(u64 key)
        {
            Withdraw(key);
            ++m_Granted;
            ++m_Forced;
        }

        /// The coat `key` no longer needs a bake of its own (it took a shared
        /// one): it leaves the queue, and its reservation, if it held one, goes
        /// back to the frame's other requests.
        void Withdraw(u64 key)
        {
            const i32 index = IndexOf(key);
            if (index != INDEX_NONE)
            {
                m_UnservedReservations -= m_Waiting[index].Reserved ? 1u : 0u;
                m_Waiting.RemoveAt(index);
            }
        }

        /// A coat needs a bake this frame. True: bake now, and it is counted
        /// against the budget. False: deferred, counted, and queued.
        [[nodiscard]] bool Request(u64 key)
        {
            const i32 index = IndexOf(key);
            const bool reserved = index != INDEX_NONE && m_Waiting[index].Reserved;
            if (m_PerFrame == 0u || reserved || m_Granted + m_UnservedReservations < m_PerFrame)
            {
                Withdraw(key);
                ++m_Granted;
                return true;
            }
            ++m_Deferred;
            if (index != INDEX_NONE)
            {
                m_Waiting[index].Requested = true;
            }
            else
            {
                m_Waiting.Add({ key, m_Frame, true, false });
            }
            return false;
        }

        /// A bake granted this frame (by Request or Force) that will not run
        /// after all -- the resident-volume budget held it back. The grant goes
        /// back to the frame, and the coat waits at the FRONT of the queue, so
        /// a bake that could not run does not cost it its place.
        void Refund(u64 key, bool forced)
        {
            if (m_Granted > 0u)
            {
                --m_Granted;
            }
            if (forced && m_Forced > 0u)
            {
                --m_Forced;
            }
            if (IndexOf(key) != INDEX_NONE)
            {
                return;
            }
            const u64 since = m_Waiting.IsEmpty() ? m_Frame : std::min(m_Frame, m_Waiting[0].Since);
            m_Waiting.Insert(Waiter{ key, since, true, false }, 0);
        }

        /// Bakes granted (forced ones included), forced, and requests
        /// deferred, since BeginFrame.
        [[nodiscard]] u32 GrantedThisFrame() const noexcept
        {
            return m_Granted;
        }
        [[nodiscard]] u32 ForcedThisFrame() const noexcept
        {
            return m_Forced;
        }
        [[nodiscard]] u32 DeferredThisFrame() const noexcept
        {
            return m_Deferred;
        }
        /// Bakes still queued, and how many frames the oldest has waited.
        [[nodiscard]] u32 Waiting() const noexcept
        {
            return static_cast<u32>(m_Waiting.Num());
        }
        [[nodiscard]] u32 OldestWaitFrames() const noexcept
        {
            return m_Waiting.IsEmpty() ? 0u : static_cast<u32>(m_Frame - m_Waiting[0].Since);
        }

        /// Forgets every waiter, as when the coats it held are released.
        void Clear() noexcept
        {
            m_Waiting.Empty();
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

        [[nodiscard]] i32 IndexOf(u64 key) const
        {
            return m_Waiting.IndexOfByPredicate([key](const Waiter& waiter)
                                                { return waiter.Key == key; });
        }

        TArray<Waiter> m_Waiting;
        u32 m_PerFrame = 0;
        u32 m_Granted = 0;
        u32 m_Deferred = 0;
        u32 m_Forced = 0;
        u32 m_UnservedReservations = 0;
        u64 m_Frame = 0;
        u64 m_FrameId = 0;
        u64 m_FrameStartTick = 0;
        bool m_Started = false;
    };
} // namespace OloEngine::GroomCoatShadow
