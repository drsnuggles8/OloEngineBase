#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Task/Task.h"
#include "OloEngine/Task/Scheduler.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/FixedSizeFreeList.h>

#include <atomic>

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
        /// on m_Jobs. If the destructor runs while a lambda is still in Release(), the
        /// scheduler worker thread dereferences memory that ~m_Jobs has just freed.
        /// The destructor spin-waits on this counter so all lambdas have fully returned
        /// before m_Jobs is torn down.
        std::atomic<i32> m_OutstandingTasks{ 0 };
    };

} // namespace OloEngine
