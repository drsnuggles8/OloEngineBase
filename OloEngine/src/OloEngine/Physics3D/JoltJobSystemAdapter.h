#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Task/Task.h"
#include "OloEngine/Task/Scheduler.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/FixedSizeFreeList.h>

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace OloEngine
{
    /// Adapter class that integrates Jolt Physics job system with OloEngine's UE5.7-based task system.
    /// Inherits from JobSystemWithBarrier which provides Barrier implementation,
    /// leaving only job queueing to be implemented.
    class JoltJobSystemAdapter final : public JPH::JobSystemWithBarrier
    {
      public:
        JPH_OVERRIDE_NEW_DELETE

        /// Constructor
        /// @param inMaxJobs Maximum number of concurrent jobs
        /// @param inMaxBarriers Maximum number of barriers
        explicit JoltJobSystemAdapter(u32 inMaxJobs, u32 inMaxBarriers);

        /// Destructor
        virtual ~JoltJobSystemAdapter() override;

        // JobSystem interface
        i32 GetMaxConcurrency() const override;
        JPH::JobSystem::JobHandle CreateJob(const char* inName, JPH::ColorArg inColor,
                                            const JPH::JobSystem::JobFunction& inJobFunction,
                                            u32 inNumDependencies = 0) override;

        /// Block until every QueueJob-launched scheduler lambda has fully returned
        /// (Job::Execute() → Job::Release() → the FreeJob()/m_Jobs free-list callback,
        /// then the m_OutstandingTasks decrement). Jolt's barrier Wait() inside
        /// PhysicsSystem::Update only synchronises on Job::Execute() *returning*, so on
        /// Update's return a scheduler worker can still be mid-Release()→FreeJob(), i.e.
        /// still touching m_Jobs. Call this immediately after PhysicsSystem::Update()
        /// returns so the engine never reads or mutates state — nor begins the next step,
        /// whose CreateJob() reuses m_Jobs slots — while a worker is still inside the
        /// adapter. This is the runtime sibling of the drain the destructor performs at
        /// teardown (see #281). Spin-waits with a timeout; on timeout it logs and returns
        /// rather than hanging the simulation. Cheap in the common case: the barrier has
        /// already waited on Execute(), so the trailing Release() is normally done and the
        /// counter reads zero on the first check.
        void WaitForOutstandingTasks() noexcept;

      protected:
        // JobSystem interface - protected methods
        void QueueJob(JPH::JobSystem::Job* inJob) override;
        void QueueJobs(JPH::JobSystem::Job** inJobs, u32 inNumJobs) override;
        void FreeJob(JPH::JobSystem::Job* inJob) override;

      private:
        /// Fixed-size free list for job allocation (using Jolt's implementation)
        using FAvailableJobs = JPH::FixedSizeFreeList<JPH::JobSystem::Job>;
        FAvailableJobs m_Jobs;

        /// Number of QueueJob-launched task lambdas currently in flight. Jolt's barrier
        /// wait only synchronises on Job::Execute() returning — but our scheduler lambda
        /// also runs Job::Release() after that, and Release can call back into FreeJob()
        /// on m_Jobs. If the destructor (or the next step) runs while a lambda is still in
        /// Release(), the scheduler worker thread dereferences memory that ~m_Jobs has
        /// just freed. WaitForOutstandingTasks BLOCKS on this counter (via m_DrainCv) so
        /// all lambdas have fully returned before m_Jobs is reused or torn down.
        std::atomic<i32> m_OutstandingTasks{ 0 };

        /// Drain synchronisation. WaitForOutstandingTasks BLOCKS on m_DrainCv rather
        /// than busy-spinning, so the waiting (game) thread yields its core to the
        /// scheduler worker that still has to run the trailing Release()→FreeJob().
        /// The busy-spin was the #281 pathology: on a core-starved CI runner the
        /// game thread's yield-spin hogged a core the parked worker needed, stalling
        /// the step for seconds (the "hang to the 120s ctest timeout") and — if a
        /// worker stayed parked past the drain timeout — letting the drain tear down
        /// m_Jobs while a lambda was mid-FreeJob (the SEH 0xc0000005 use-after-free).
        /// The QueueJob lambda decrements m_OutstandingTasks under m_DrainMutex and
        /// notifies m_DrainCv; the brief lock is uncontended unless the game thread is
        /// actively draining.
        std::mutex m_DrainMutex;
        std::condition_variable m_DrainCv;

        /// Cached result of GetMaxConcurrency(). Jolt assumes this is constant for the
        /// job system's lifetime (and calls it many times per PhysicsSystem::Update to
        /// size per-step arrays and index them back), but our value derives from the
        /// shared FScheduler's live active-worker count. Computed on first use (0 is the
        /// "not yet computed" sentinel) and never changed after, so every call agrees.
        mutable std::atomic<i32> m_CachedMaxConcurrency{ 0 };
    };

} // namespace OloEngine
