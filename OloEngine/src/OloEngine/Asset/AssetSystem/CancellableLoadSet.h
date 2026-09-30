#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/AssetSystem/AssetLoadTicket.h"
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Task/Task.h"

#include <functional>
#include <optional>
#include <utility>

namespace OloEngine
{
    /**
     * @brief One load launched by a TCancellableLoadSet: its key, its task (whose
     *        result is the load's output), the ticket it races a cancel on, and the
     *        caller's payload (e.g. a byte estimate or the region it fills).
     */
    template<typename TKey, typename TResult, typename TPayload>
    struct TCancellableLoadRecord
    {
        TKey Key{};
        Tasks::TTask<TResult> Task;
        Ref<FAssetLoadTicket> Ticket;
        TPayload Payload{};
    };

    template<typename TKey, typename TResult, typename TPayload>
    struct TIsTriviallyRelocatable<TCancellableLoadRecord<TKey, TResult, TPayload>>
    {
        using Record = TCancellableLoadRecord<TKey, TResult, TPayload>;
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(Record::Key)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::Task)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::Ticket)> &&
                                      TIsTriviallyRelocatable_V<decltype(Record::Payload)>;
    };

    /** Cumulative counts of what cancellation did. */
    struct FCancellableLoadCounters
    {
        u64 CancelledBeforeStart = 0;
        u64 AbandonedInFlight = 0;
        u64 DiscardedCompleted = 0;
        u64 AbandonedResultsDropped = 0; // abandoned loads whose worker ran and whose result was released
    };

    /**
     * @brief The cancellable-load state machine shared by RuntimeAssetSystem (pack
     *        assets) and SceneStreamer (regions), issue #1365.
     *
     * A load's result reaches its owner only through ExtractCompleted(), and only
     * while its record is live. Cancel() takes the record out of the live set, so
     * a result that arrives afterwards can never be extracted:
     *
     * | state at cancel           | Cancel()                                    | the result                                   |
     * |---------------------------|---------------------------------------------|----------------------------------------------|
     * | queued, not started       | ticket Queued -> Cancelled; record abandoned | none: the body sees Cancelled and never runs the work |
     * | running                   | record abandoned                            | produced, never extracted; dropped when reaped |
     * | completed, not extracted  | record handed to the caller to drop          | released by the caller                       |
     * | not live                  | nothing                                     | NotPending                                   |
     *
     * An abandoned record stays until its task completes: the task may still be
     * running code that references the owner, so the owner's shutdown waits for
     * CollectTasks() — live and abandoned — before it goes away.
     *
     * Records that leave the set are handed back through `outDropped` rather than
     * destroyed inside the set, so an owner holding a lock can release them (and
     * the results they hold) after unlocking.
     *
     * Not thread-safe: the owner serialises every call (RuntimeAssetSystem under
     * its mutex, SceneStreamer on the main thread). The worker only touches the
     * ticket, which is atomic.
     */
    template<typename TKey, typename TResult, typename TPayload>
    class TCancellableLoadSet
    {
      public:
        using FRecord = TCancellableLoadRecord<TKey, TResult, TPayload>;

        /**
         * @brief Launch `work` as a task for `key`. The work runs only if the ticket
         *        moves Queued -> Running; a load cancelled first returns TResult{}.
         * @param startGate When set, a prerequisite of the task (tests hold a load
         *        genuinely queued with it).
         */
        void Launch(const char* debugName, TKey key, TPayload payload, std::function<TResult()> work,
                    const std::optional<Tasks::FTaskEvent>& startGate)
        {
            auto ticket = Ref<FAssetLoadTicket>::Create();
            auto body = [ticket, work = std::move(work)]() -> TResult
            {
                auto expected = FAssetLoadTicket::EState::Queued;
                if (!ticket->State.compare_exchange_strong(expected, FAssetLoadTicket::EState::Running))
                    return TResult{};
                return work();
            };

            Tasks::TTask<TResult> task;
            if (startGate.has_value())
            {
                Tasks::FTaskEvent gate = *startGate;
                task = Tasks::Launch(debugName, std::move(body), Tasks::Prerequisites(gate), Tasks::ETaskPriority::BackgroundNormal);
            }
            else
            {
                task = Tasks::Launch(debugName, std::move(body), Tasks::ETaskPriority::BackgroundNormal);
            }
            m_Live.Add(FRecord{ std::move(key), std::move(task), std::move(ticket), std::move(payload) });
        }

        [[nodiscard]] const FRecord* FindLive(const TKey& key) const
        {
            for (const FRecord& record : m_Live)
            {
                if (record.Key == key)
                    return &record;
            }
            return nullptr;
        }

        /** Withdraw or abandon the live load for `key`; see the class comment. */
        EAssetLoadCancelResult Cancel(const TKey& key, TArray<FRecord>& outDropped)
        {
            for (i32 i = 0; i < m_Live.Num(); ++i)
            {
                FRecord& record = m_Live[i];
                if (!(record.Key == key))
                    continue;

                EAssetLoadCancelResult result;
                auto expected = FAssetLoadTicket::EState::Queued;
                if (record.Ticket->State.compare_exchange_strong(expected, FAssetLoadTicket::EState::Cancelled))
                {
                    // Never runs its work. The (now trivial) body must still run before
                    // the task handle may go, so it waits with the abandoned loads.
                    ++m_Counters.CancelledBeforeStart;
                    result = EAssetLoadCancelResult::CancelledBeforeStart;
                    m_Abandoned.Add(std::move(record));
                }
                else if (record.Task.IsCompleted())
                {
                    ++m_Counters.DiscardedCompleted;
                    result = EAssetLoadCancelResult::DiscardedCompleted;
                    outDropped.Add(std::move(record));
                }
                else
                {
                    // Mid-load. Out of the live set, its result can never be extracted.
                    ++m_Counters.AbandonedInFlight;
                    result = EAssetLoadCancelResult::AbandonedInFlight;
                    m_Abandoned.Add(std::move(record));
                }
                m_Live.RemoveAt(i);
                return result;
            }
            return EAssetLoadCancelResult::NotPending;
        }

        /** Move every completed live record, in launch order, into `outCompleted`. */
        void ExtractCompleted(TArray<FRecord>& outCompleted)
        {
            for (i32 i = 0; i < m_Live.Num();)
            {
                if (!m_Live[i].Task.IsCompleted())
                {
                    ++i;
                    continue;
                }
                outCompleted.Add(std::move(m_Live[i]));
                m_Live.RemoveAt(i);
            }
        }

        /** Hand every finished abandoned record to `outDropped`. */
        void ReapAbandoned(TArray<FRecord>& outDropped)
        {
            for (i32 i = m_Abandoned.Num() - 1; i >= 0; --i)
            {
                if (!m_Abandoned[i].Task.IsCompleted())
                    continue;
                // A load cancelled before it started produced nothing; count only the
                // ones whose worker ran.
                if (m_Abandoned[i].Ticket->State.load() == FAssetLoadTicket::EState::Running)
                    ++m_Counters.AbandonedResultsDropped;
                outDropped.Add(std::move(m_Abandoned[i]));
                m_Abandoned.RemoveAtSwap(i);
            }
        }

        /** Stop every live load that has not started from ever running its work. */
        void CancelUnstarted()
        {
            for (const FRecord& record : m_Live)
            {
                auto expected = FAssetLoadTicket::EState::Queued;
                record.Ticket->State.compare_exchange_strong(expected, FAssetLoadTicket::EState::Cancelled);
            }
        }

        /** Every task, live and abandoned, for an owner that must wait them all out. */
        void CollectTasks(TArray<Tasks::TTask<TResult>>& outTasks) const
        {
            outTasks.Reserve(outTasks.Num() + m_Live.Num() + m_Abandoned.Num());
            for (const FRecord& record : m_Live)
                outTasks.Add(record.Task);
            for (const FRecord& record : m_Abandoned)
                outTasks.Add(record.Task);
        }

        /** Hand every record, live and abandoned, to `outDropped`. */
        void Clear(TArray<FRecord>& outDropped)
        {
            for (FRecord& record : m_Live)
                outDropped.Add(std::move(record));
            for (FRecord& record : m_Abandoned)
                outDropped.Add(std::move(record));
            m_Live.Reset();
            m_Abandoned.Reset();
        }

        [[nodiscard]] const TArray<FRecord>& Live() const
        {
            return m_Live;
        }
        [[nodiscard]] i32 LiveCount() const
        {
            return m_Live.Num();
        }
        [[nodiscard]] u32 AbandonedRunningCount() const
        {
            u32 running = 0;
            for (const FRecord& record : m_Abandoned)
            {
                if (!record.Task.IsCompleted())
                    ++running;
            }
            return running;
        }
        [[nodiscard]] u32 CompletedLiveCount() const
        {
            u32 completed = 0;
            for (const FRecord& record : m_Live)
            {
                if (record.Task.IsCompleted())
                    ++completed;
            }
            return completed;
        }
        [[nodiscard]] const FCancellableLoadCounters& Counters() const
        {
            return m_Counters;
        }
        void ResetCounters()
        {
            m_Counters = {};
        }

      private:
        TArray<FRecord> m_Live;
        TArray<FRecord> m_Abandoned;
        FCancellableLoadCounters m_Counters;
    };
} // namespace OloEngine
