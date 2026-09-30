#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <atomic>

namespace OloEngine
{
    /**
     * @brief Shared between one load request and the worker task running it.
     *
     * The worker moves Queued -> Running when it starts; a cancel moves
     * Queued -> Cancelled. Both are one compare-exchange, so exactly one side wins
     * and "cancelled before start" is a guarantee that the load body never runs,
     * not a guess made from timing.
     */
    class FAssetLoadTicket : public RefCounted
    {
      public:
        enum class EState : u8
        {
            Queued,
            Running,
            Cancelled
        };

        // mutable: Ref<T> is const-propagating, and a ticket reached through a const
        // record is still the live state both sides race on.
        mutable std::atomic<EState> State{ EState::Queued };
    };

    /**
     * @brief What a cancel did, for an asset load (RuntimeAssetSystem::CancelAssetLoad)
     *        or a region load (SceneStreamer::CancelRegionLoad).
     *
     * Every value except NotPending means the request is gone: it no longer counts
     * as pending, and its result, if one is ever produced, is never integrated.
     */
    enum class EAssetLoadCancelResult : u8
    {
        NotPending,           // nothing was in flight for the handle: never queued, already retrieved, or already cancelled
        CancelledBeforeStart, // the worker had not started it; the load body never runs
        AbandonedInFlight,    // the worker was mid-load; it finishes, and its result is dropped
        DiscardedCompleted    // the load had finished but was not yet retrieved; its result was released
    };

    [[nodiscard]] const char* ToString(EAssetLoadCancelResult result) noexcept;

} // namespace OloEngine
