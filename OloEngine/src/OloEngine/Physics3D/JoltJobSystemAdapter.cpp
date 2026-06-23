#include "OloEnginePCH.h"
#include "JoltJobSystemAdapter.h"
#include "OloEngine/Core/Log.h"

#include <chrono>
#include <thread>

namespace OloEngine
{
    JoltJobSystemAdapter::JoltJobSystemAdapter(u32 inMaxJobs, u32 inMaxBarriers)
    {
        // Initialize the barrier system (provided by JobSystemWithBarrier base class)
        JPH::JobSystemWithBarrier::Init(inMaxBarriers);

        // Initialize the job free list
        m_Jobs.Init(inMaxJobs, inMaxJobs);

        OLO_CORE_INFO("JoltJobSystemAdapter initialized - MaxJobs: {0}, MaxBarriers: {1}", inMaxJobs, inMaxBarriers);
    }

    JoltJobSystemAdapter::~JoltJobSystemAdapter()
    {
        // Wait for any scheduler-queued QueueJob lambdas to fully return before letting
        // m_Jobs destruct. Jolt's PhysicsSystem::Update barrier-waits on Job::Execute()
        // returning, but our lambda runs `inJob->Release()` after Execute, and Release
        // can call back into FreeJob(this) → m_Jobs.DestructObject. If the destructor
        // tears down m_Jobs first, the worker thread dereferences freed Job memory —
        // exactly the TSan race that bit MultipleScenesCoexistTest, SceneStepAdvances*,
        // TriggerEndEvent*, and CharacterControllerJumps* on Linux CI. Same drain the
        // per-step path runs after each PhysicsSystem::Update (see WaitForOutstandingTasks).
        WaitForOutstandingTasks();
    }

    void JoltJobSystemAdapter::WaitForOutstandingTasks() noexcept
    {
        // BLOCK until every in-flight QueueJob lambda has decremented
        // m_OutstandingTasks — which it does only after Job::Execute() AND
        // Job::Release() (→ possible FreeJob on m_Jobs) have both fully returned.
        // See the header for why Jolt's barrier alone isn't enough.
        //
        // Blocking (rather than the old yield-spin) is the #281 fix: the waiting
        // game thread releases its core so the scheduler worker that still has to
        // run the trailing Release() can be scheduled. The yield-spin instead
        // hogged a core the parked worker needed — on a core-starved CI runner
        // that stalled the step for seconds (the "hang to the 120s ctest timeout")
        // and, if a worker stayed parked past the deadline, let this drain return
        // and tear down / reuse m_Jobs while the lambda was mid-FreeJob (the SEH
        // 0xc0000005 use-after-free). With the block the worker drains in
        // microseconds, so the timeout below is a pure backstop that never fires
        // in healthy operation. The lambda decrements under m_DrainMutex and
        // notifies, so the wake can't be missed.
        using namespace std::chrono_literals;
        constexpr auto kTimeout = 5s;
        try
        {
            std::unique_lock<std::mutex> lock(m_DrainMutex);
            const bool drained = m_DrainCv.wait_for(lock, kTimeout, [this]() noexcept
                                                    { return m_OutstandingTasks.load(std::memory_order_acquire) == 0; });
            if (!drained)
            {
                OLO_CORE_ERROR("JoltJobSystemAdapter: timed out waiting for {} outstanding physics job(s) to drain",
                               m_OutstandingTasks.load(std::memory_order_relaxed));
            }
        }
        catch (const std::system_error&)
        {
            // Locking / waiting failed (effectively unreachable). The function is
            // noexcept, so swallow rather than std::terminate.
        }
    }

    i32 JoltJobSystemAdapter::GetMaxConcurrency() const
    {
        // Jolt treats GetMaxConcurrency() as a CONSTANT for the lifetime of the job
        // system — its own JobSystemThreadPool returns mThreads.size() + 1, fixed at
        // construction — and calls it repeatedly within a single PhysicsSystem::Update
        // to size the per-step arrays (mBodyPairQueues, mSolve*Constraints, …) and to
        // derive the job indices that read those arrays back. We bridge to the shared
        // FScheduler, whose active-worker count is a live atomic, so cache it on first
        // use: every call within (and across) an Update then agrees, even if the
        // scheduler's worker count were to change between calls (e.g. RestartWorkers).
        // +1 for the calling thread, which also executes jobs while it waits on a Jolt
        // barrier. (Jolt clamps this to PhysicsUpdateContext::cMaxConcurrency = 32, so
        // the value can never overflow those StaticArrays regardless.)
        i32 cached = m_CachedMaxConcurrency.load(std::memory_order_acquire);
        if (cached == 0)
        {
            cached = static_cast<i32>(LowLevelTasks::FScheduler::Get().GetNumWorkers()) + 1;
            m_CachedMaxConcurrency.store(cached, std::memory_order_release);
        }
        return cached;
    }

    JPH::JobSystem::JobHandle JoltJobSystemAdapter::CreateJob(const char* inName, JPH::ColorArg inColor,
                                                              const JPH::JobSystem::JobFunction& inJobFunction,
                                                              u32 inNumDependencies)
    {
        // Allocate a job from the free list. ConstructObject returns cInvalidObjectIndex
        // (0xffffffff) when the pool is exhausted; passing that into m_Jobs.Get() indexes
        // mPages[index >> shift][...] — a wild out-of-bounds access, i.e. exactly the
        // #281-class SEH 0xc0000005. Jobs are short-lived and cMaxPhysicsJobs (2048)
        // dwarfs the handful a step needs, so the pool effectively never exhausts; if it
        // ever does it signals a job leak or wedged scheduler. Unlike Jolt's own
        // JobSystemThreadPool::CreateJob (which spins forever here), retry under a bounded
        // deadline — an unbounded spin would hang PhysicsSystem::Update indefinitely,
        // reproducing #281's 120 s ctest timeout and freezing the editor/runtime. On
        // timeout, fail fast with an empty handle: Jolt's CreateJob has no error-return
        // channel, so this is the documented "no job available" signal (a fast,
        // deterministic, logged failure that the CI retry can absorb beats a silent hang).
        u32 index = m_Jobs.ConstructObject(inName, inColor, this, inJobFunction, inNumDependencies);
        if (index == FAvailableJobs::cInvalidObjectIndex)
        {
            using clock = std::chrono::steady_clock;
            constexpr auto kTimeout = std::chrono::seconds(5);
            const auto deadline = clock::now() + kTimeout;
            do
            {
                if (clock::now() > deadline)
                {
                    OLO_CORE_ERROR("JoltJobSystemAdapter::CreateJob: physics job free-list exhausted for {}s "
                                   "(likely a job leak or wedged scheduler); returning an empty handle",
                                   std::chrono::duration_cast<std::chrono::seconds>(kTimeout).count());
                    return JPH::JobSystem::JobHandle();
                }
                std::this_thread::yield();
                index = m_Jobs.ConstructObject(inName, inColor, this, inJobFunction, inNumDependencies);
            } while (index == FAvailableJobs::cInvalidObjectIndex);
        }

        // Get pointer to the job from index
        JPH::JobSystem::Job* job = &m_Jobs.Get(index);

        // Create a JobHandle
        JPH::JobSystem::JobHandle handle(job);

        // If no dependencies, queue immediately
        if (inNumDependencies == 0)
            QueueJob(job);

        return handle;
    }

    void JoltJobSystemAdapter::QueueJob(JPH::JobSystem::Job* inJob)
    {
        // Add reference since we're queuing the job
        inJob->AddRef();

        // Bump the outstanding-tasks counter BEFORE Tasks::Launch — the destructor's
        // wait must observe every in-flight lambda, and the counter has to be live by
        // the time the destructor checks. Decrement happens at the lambda end, after
        // every member access through `this` is done.
        m_OutstandingTasks.fetch_add(1, std::memory_order_release);

        // Launch as a high-priority task (physics is critical).
        // Capture inJob by value (ref-counted pointer) to keep it alive; `this` is
        // kept alive by the destructor wait above.
        Tasks::Launch(
            "JoltPhysicsJob",
            [inJob, this]()
            {
                // Execute the Jolt job
                inJob->Execute();

                // Release our reference. This can call back into FreeJob(this) →
                // m_Jobs, so it must complete before the destructor releases m_Jobs;
                // the outstanding-tasks counter is decremented strictly AFTER this.
                inJob->Release();

                // Decrement under m_DrainMutex and notify *while still holding it* so a
                // blocked WaitForOutstandingTasks wakes promptly and can't miss the wake
                // (the decrement is paired with the drain's predicate re-check by the
                // mutex). The notify MUST stay inside the lock: ~JoltJobSystemAdapter
                // destroys m_DrainCv the instant WaitForOutstandingTasks returns, so if
                // this lambda decremented to zero, released the mutex, and the game thread
                // then woke + returned + ran the destructor, a notify_one() issued after
                // the unlock would signal an already-destroyed condition variable — the
                // pthread_cond_signal-vs-pthread_cond_destroy data race ThreadSanitizer
                // flagged across the Functional physics suite. Holding the mutex across
                // notify_one() keeps the waiter from re-acquiring it (and thus from
                // returning + destroying the CV) until the notify has fully completed. The
                // brief lock is uncontended in the common case — the game thread only holds
                // it while actually draining.
                {
                    std::lock_guard<std::mutex> lock(m_DrainMutex);
                    m_OutstandingTasks.fetch_sub(1, std::memory_order_release);
                    m_DrainCv.notify_one();
                }
            },
            LowLevelTasks::ETaskPriority::High);
    }

    void JoltJobSystemAdapter::QueueJobs(JPH::JobSystem::Job** inJobs, u32 inNumJobs)
    {
        // Queue all jobs individually
        for (u32 i = 0; i < inNumJobs; ++i)
        {
            QueueJob(inJobs[i]);
        }
    }

    void JoltJobSystemAdapter::FreeJob(JPH::JobSystem::Job* inJob)
    {
        // DestructObject accepts a Job pointer directly and will look up its index internally
        m_Jobs.DestructObject(inJob);
    }

} // namespace OloEngine
