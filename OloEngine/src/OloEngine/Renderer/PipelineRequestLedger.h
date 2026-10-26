#pragma once

// PipelineRequestLedger: the bookkeeping half of PSO precaching (issue #1345).
//
// Every pipeline a backend creates moves through one state machine here:
//
//   (absent) --Request--> Queued --TakeNextQueued--> InFlight --Complete--> Ready
//                            |                         ^   \------------> Failed
//                            \--ClaimMiss (draw)-------/
//
// and every draw-time miss is classified by the state it found:
//
//   - NeverRequested     nothing asked for this key before the draw needed it;
//   - TooLateQueued      a request existed but no worker had started it, so the
//                        draw took it over and created it synchronously;
//   - TooLateInFlight    a worker was creating it, so the draw waited for it;
//   - RetriedAfterFailure the key's last creation failed and the draw retries.
//
// A draw that finds the pipeline already prepared by a request is a PREPARED
// first use: the only outcome with no synchronous creation on the draw thread.
//
// "Ready" claims (issue #1345, acceptance criterion 2): a collector may declare a
// shader ready once every request it knows for that shader is prepared. A
// NeverRequested miss on a shader declared ready is then a broken claim, and is
// counted separately (ReadyClaimViolations) so it can never hide behind the
// ordinary miss count.
//
// Pure: no backend, no locking, no clock. The owner (VulkanPipelineBuilder)
// holds its own mutex around every call and passes timings in. That is what
// lets PipelineRequestLedgerTest pin every transition and counter without a
// device.

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/Deque.h"
#include "OloEngine/Core/Base.h"

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace OloEngine
{
    enum class PipelineMissKind : u8
    {
        NeverRequested,
        TooLateQueued,
        TooLateInFlight,
        RetriedAfterFailure,
    };

    [[nodiscard]] constexpr const char* ToString(const PipelineMissKind kind)
    {
        switch (kind)
        {
            case PipelineMissKind::NeverRequested:
                return "never requested";
            case PipelineMissKind::TooLateQueued:
                return "requested too late (still queued)";
            case PipelineMissKind::TooLateInFlight:
                return "requested too late (in flight)";
            case PipelineMissKind::RetriedAfterFailure:
                return "retried after a failed creation";
        }
        return "unknown";
    }

    enum class PipelineRequestPriority : u8
    {
        Selected,    ///< The selected rendering configuration: prepared first.
        Speculative, ///< Another configuration seen before (MSAA, path, upscale): prepared after.
    };

    // Counters since the last reset. Times are milliseconds of CPU wall time on
    // the thread that paid them.
    struct PipelineRequestCounters
    {
        u64 Requests = 0;          ///< Accepted requests (duplicates excluded).
        u64 DuplicateRequests = 0; ///< Requests for a key already queued, in flight or ready.
        u64 PreparedFirstUses = 0; ///< A draw's first use found the pipeline prepared: the complete-PSO hit.
        u64 NeverRequested = 0;
        u64 TooLateQueued = 0;
        u64 TooLateInFlight = 0;
        u64 RetriedAfterFailure = 0;
        u64 ReadyClaimViolations = 0; ///< NeverRequested misses on a shader declared ready.
        u64 PreparedCreations = 0;    ///< Pipelines a precache worker created.
        u64 BlockingCreations = 0;    ///< Pipelines a draw created synchronously.
        u64 FailedCreations = 0;      ///< Either path; the key is then Failed.
        u64 DiscardedStale = 0;       ///< Worker results dropped because the shader was invalidated meanwhile.
        u64 DriverCacheHits = 0;      ///< Creation feedback: served from the pipeline cache.
        u64 DriverCacheMisses = 0;    ///< Creation feedback: compiled.
        u64 DriverCacheUnknown = 0;   ///< Creation feedback unavailable or not valid.
        f64 PreparedCreateMs = 0.0;   ///< Worker time spent creating, off the draw threads.
        f64 BlockingCreateMs = 0.0;   ///< Draw-thread time creating or waiting: the hitch.
        f64 BlockingCreateMaxMs = 0.0;

        [[nodiscard]] u64 Misses() const
        {
            return NeverRequested + TooLateQueued + TooLateInFlight + RetriedAfterFailure;
        }
    };

    enum class PipelineDriverCacheResult : u8
    {
        Unknown,
        Hit,
        Miss,
    };

    template<typename Key, typename Hash, typename Payload>
    class PipelineRequestLedger
    {
      public:
        enum class State : u8
        {
            Queued,
            InFlight,
            Ready,
            Failed,
        };

        // What a draw that missed the pipeline map must do next.
        enum class MissAction : u8
        {
            CreateNow, ///< This thread owns the creation; call Complete() after.
            Wait,      ///< Another thread is creating it; wait, then look the map up again.
        };

        struct MissResult
        {
            MissAction Action = MissAction::CreateNow;
            PipelineMissKind Kind = PipelineMissKind::NeverRequested;
            bool ReadyClaimViolation = false; ///< NeverRequested on a shader declared ready.
            u64 Ticket = 0;                   ///< With CreateNow: hand back to Complete().
        };

        // A queued request a worker took over.
        struct Job
        {
            Key RequestKey{};
            Payload Data{};
            u64 Ticket = 0; ///< Hand back to Complete().
        };

        // Returns true when the request was accepted (a new key, or one whose
        // last creation failed and is retried).
        bool Request(const Key& key, u64 shaderKey, Payload payload,
                     PipelineRequestPriority priority = PipelineRequestPriority::Selected)
        {
            if (const auto it = m_Entries.find(key); it != m_Entries.end() && it->second.Status != State::Failed)
            {
                // A speculative request whose configuration became the selected
                // one is promoted, once: queued a second time at the front
                // priority. The speculative slot it leaves behind is skipped.
                if (it->second.Status == State::Queued && priority == PipelineRequestPriority::Selected &&
                    it->second.Priority == PipelineRequestPriority::Speculative)
                {
                    it->second.Priority = PipelineRequestPriority::Selected;
                    m_SelectedQueue.PushLast(key);
                }
                ++m_Counters.DuplicateRequests;
                return false;
            }
            Entry& entry = m_Entries[key];
            entry.Status = State::Queued;
            entry.ShaderKey = shaderKey;
            entry.Prepared = true;
            entry.Priority = priority;
            entry.Data = std::move(payload);
            (priority == PipelineRequestPriority::Selected ? m_SelectedQueue : m_SpeculativeQueue).PushLast(key);
            ++m_Counters.Requests;
            return true;
        }

        // The next queued request for a worker, selected configuration first.
        // The entry moves to InFlight; the worker must Complete() it with the
        // job's ticket.
        [[nodiscard]] std::optional<Job> TakeNextQueued()
        {
            for (auto* queue : { &m_SelectedQueue, &m_SpeculativeQueue })
            {
                while (!queue->IsEmpty())
                {
                    Key key = queue->First();
                    queue->PopFirst();
                    const auto it = m_Entries.find(key);
                    // A key leaves the queue lazily: a draw that took it over
                    // (ClaimMiss), a promotion or an invalidation leaves a
                    // stale queue slot.
                    if (it == m_Entries.end() || it->second.Status != State::Queued)
                    {
                        continue;
                    }
                    it->second.Status = State::InFlight;
                    it->second.Ticket = ++m_LastTicket;
                    return Job{ std::move(key), std::move(it->second.Data), it->second.Ticket };
                }
            }
            return std::nullopt;
        }

        // A draw missed the pipeline map for `key`. Classify, count, and say
        // whether this thread creates it or waits for the thread that is.
        // `countMiss` is false when the same draw asks again after waiting: it
        // was counted once, as requested too late, and is one hitch.
        MissResult ClaimMiss(const Key& key, u64 shaderKey, bool countMiss = true)
        {
            const auto count = [countMiss](u64& counter)
            {
                if (countMiss)
                {
                    ++counter;
                }
            };
            const auto it = m_Entries.find(key);
            if (it == m_Entries.end())
            {
                count(m_Counters.NeverRequested);
                const bool brokenClaim = countMiss && m_ReadyShaders.contains(shaderKey);
                if (brokenClaim)
                {
                    ++m_Counters.ReadyClaimViolations;
                }
                Entry& entry = m_Entries[key];
                entry.Status = State::InFlight;
                entry.ShaderKey = shaderKey;
                entry.Prepared = false;
                entry.Ticket = ++m_LastTicket;
                return { MissAction::CreateNow, PipelineMissKind::NeverRequested, brokenClaim, entry.Ticket };
            }
            Entry& entry = it->second;
            PipelineMissKind kind = PipelineMissKind::NeverRequested;
            switch (entry.Status)
            {
                case State::Queued:
                    count(m_Counters.TooLateQueued);
                    entry.Data = Payload{}; // the draw snapshots its own inputs
                    kind = PipelineMissKind::TooLateQueued;
                    break;
                case State::InFlight:
                    count(m_Counters.TooLateInFlight);
                    return { MissAction::Wait, PipelineMissKind::TooLateInFlight, false, 0 };
                case State::Failed:
                    count(m_Counters.RetriedAfterFailure);
                    kind = PipelineMissKind::RetriedAfterFailure;
                    break;
                case State::Ready:
                    // The owner's map and this ledger disagree; the map is the
                    // authority for hits, so recreate rather than wait forever.
                    count(m_Counters.NeverRequested);
                    break;
            }
            entry.Status = State::InFlight;
            entry.Prepared = false;
            entry.Ticket = ++m_LastTicket;
            return { MissAction::CreateNow, kind, false, entry.Ticket };
        }

        // The creation the caller owned (`ticket` from ClaimMiss or the Job) has
        // finished. `blocking` is true when a draw thread paid for it. Returns
        // false when the result must be discarded: the key was invalidated
        // while it ran, and possibly claimed again since. The ticket tells this
        // creation from the newer one, so an old result can never be published
        // over the claim that replaced it.
        bool Complete(const Key& key, u64 ticket, bool success, bool blocking, f64 createMs,
                      PipelineDriverCacheResult cache = PipelineDriverCacheResult::Unknown)
        {
            const auto it = m_Entries.find(key);
            const bool stale =
                it == m_Entries.end() || it->second.Status != State::InFlight || it->second.Ticket != ticket;
            if (blocking)
            {
                ++m_Counters.BlockingCreations;
                m_Counters.BlockingCreateMs += createMs;
                m_Counters.BlockingCreateMaxMs = std::max(m_Counters.BlockingCreateMaxMs, createMs);
            }
            else
            {
                ++m_Counters.PreparedCreations;
                m_Counters.PreparedCreateMs += createMs;
            }
            switch (cache)
            {
                case PipelineDriverCacheResult::Hit:
                    ++m_Counters.DriverCacheHits;
                    break;
                case PipelineDriverCacheResult::Miss:
                    ++m_Counters.DriverCacheMisses;
                    break;
                case PipelineDriverCacheResult::Unknown:
                    ++m_Counters.DriverCacheUnknown;
                    break;
            }
            if (!success)
            {
                ++m_Counters.FailedCreations;
            }
            if (stale)
            {
                if (success)
                {
                    ++m_Counters.DiscardedStale;
                }
                return false;
            }
            it->second.Status = success ? State::Ready : State::Failed;
            return true;
        }

        // A draw waited for another thread's creation: its wait is blocking time
        // too, but not a second creation.
        void NoteBlockingWait(f64 waitMs)
        {
            m_Counters.BlockingCreateMs += waitMs;
            m_Counters.BlockingCreateMaxMs = std::max(m_Counters.BlockingCreateMaxMs, waitMs);
        }

        // A draw's FIRST use of a ready pipeline. Counts a prepared hit when a
        // request (not a draw) created it; returns whether it was prepared.
        bool NoteFirstUse(const Key& key)
        {
            const auto it = m_Entries.find(key);
            if (it == m_Entries.end() || it->second.Status != State::Ready || !it->second.Prepared)
            {
                return false;
            }
            ++m_Counters.PreparedFirstUses;
            return true;
        }

        // Drop every entry of one shader (hot reload, destruction). An in-flight
        // entry is erased too, which turns its later Complete() into a discard.
        // Returns the keys that were Ready or Queued, so the caller can request
        // them again against the rebuilt shader.
        TArray<Key> InvalidateShader(u64 shaderKey)
        {
            TArray<Key> invalidated;
            m_ReadyShaders.erase(shaderKey);
            for (auto it = m_Entries.begin(); it != m_Entries.end();)
            {
                if (it->second.ShaderKey != shaderKey)
                {
                    ++it;
                    continue;
                }
                if (it->second.Status == State::Ready || it->second.Status == State::Queued)
                {
                    invalidated.Add(it->first);
                }
                it = m_Entries.erase(it);
            }
            return invalidated;
        }

        // Forget this shader's queued requests (their payload names modules
        // about to be destroyed); created and in-flight entries are untouched.
        void DropQueued(u64 shaderKey)
        {
            std::erase_if(m_Entries, [shaderKey](const auto& item)
                          { return item.second.ShaderKey == shaderKey && item.second.Status == State::Queued; });
        }

        void Clear()
        {
            m_Entries.clear();
            m_SelectedQueue.Empty();
            m_SpeculativeQueue.Empty();
            m_ReadyShaders.clear();
        }

        // The collector's readiness claim for one shader; cleared by
        // InvalidateShader and by DeclareShaderNotReady.
        void DeclareShaderReady(u64 shaderKey)
        {
            m_ReadyShaders.insert(shaderKey);
        }
        void DeclareShaderNotReady(u64 shaderKey)
        {
            m_ReadyShaders.erase(shaderKey);
        }
        [[nodiscard]] bool IsShaderDeclaredReady(u64 shaderKey) const
        {
            return m_ReadyShaders.contains(shaderKey);
        }

        [[nodiscard]] std::optional<State> GetState(const Key& key) const
        {
            if (const auto it = m_Entries.find(key); it != m_Entries.end())
            {
                return it->second.Status;
            }
            return std::nullopt;
        }

        [[nodiscard]] sizet QueuedCount() const
        {
            sizet count = 0;
            for (const auto& [key, entry] : m_Entries)
            {
                count += entry.Status == State::Queued ? 1u : 0u;
            }
            return count;
        }

        [[nodiscard]] sizet InFlightCount() const
        {
            sizet count = 0;
            for (const auto& [key, entry] : m_Entries)
            {
                count += entry.Status == State::InFlight ? 1u : 0u;
            }
            return count;
        }

        [[nodiscard]] bool HasInFlightFor(u64 shaderKey) const
        {
            for (const auto& [key, entry] : m_Entries)
            {
                if (entry.ShaderKey == shaderKey && entry.Status == State::InFlight)
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] const PipelineRequestCounters& GetCounters() const
        {
            return m_Counters;
        }
        void ResetCounters()
        {
            m_Counters = {};
        }

      private:
        struct Entry
        {
            State Status = State::Queued;
            u64 ShaderKey = 0;
            bool Prepared = false; ///< Created through a request rather than by a draw.
            PipelineRequestPriority Priority = PipelineRequestPriority::Selected;
            u64 Ticket = 0; ///< Which claim the in-flight creation belongs to.
            Payload Data{};
        };

        std::unordered_map<Key, Entry, Hash> m_Entries;
        TDeque<Key> m_SelectedQueue;
        TDeque<Key> m_SpeculativeQueue;
        std::unordered_set<u64> m_ReadyShaders;
        PipelineRequestCounters m_Counters;
        u64 m_LastTicket = 0;
    };
} // namespace OloEngine
