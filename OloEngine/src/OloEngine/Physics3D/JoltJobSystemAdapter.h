#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Task/Task.h"
#include "OloEngine/Task/Scheduler.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/FixedSizeFreeList.h>

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
        virtual i32 GetMaxConcurrency() const override;
        virtual JPH::JobSystem::JobHandle CreateJob(const char* inName, JPH::ColorArg inColor,
                                                    const JPH::JobSystem::JobFunction& inJobFunction,
                                                    u32 inNumDependencies = 0) override;

      protected:
        // JobSystem interface - protected methods
        virtual void QueueJob(JPH::JobSystem::Job* inJob) override;
        virtual void QueueJobs(JPH::JobSystem::Job** inJobs, u32 inNumJobs) override;
        virtual void FreeJob(JPH::JobSystem::Job* inJob) override;

      private:
        /// Fixed-size free list for job allocation (using Jolt's implementation)
        using FAvailableJobs = JPH::FixedSizeFreeList<JPH::JobSystem::Job>;
        FAvailableJobs m_Jobs;
    };

} // namespace OloEngine
