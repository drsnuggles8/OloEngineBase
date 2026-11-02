/**
 * @file ThreadManagerTest.cpp
 * @brief Unit tests for FThreadManager / FRunnableThread registration wiring.
 *
 * Verifies that every thread created via FRunnableThread::Create() registers
 * itself in the global FThreadManager registry on creation and deregisters on
 * destruction. This is the data source behind the editor's Thread Inspector
 * panel, so the registration contract is what we pin here.
 */

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Core/PlatformTLS.h"
#include "OloEngine/HAL/Runnable.h"
#include "OloEngine/HAL/RunnableThread.h"
#include "OloEngine/HAL/ManualResetEvent.h"
#include "OloEngine/HAL/ThreadManager.h"
#include "OloEngine/Async/Async.h"
#include "OloEngine/Task/NamedThreads.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

using namespace OloEngine;

namespace
{
    // A runnable that parks in Run() until asked to stop, so the worker stays
    // alive (and registered) while the test inspects the registry. Kill() on the
    // owning FRunnableThread calls Stop() then joins, which releases the loop.
    class ParkingRunnable final : public FRunnable
    {
      public:
        u32 Run() override
        {
            m_Started.store(true, std::memory_order_release);
            while (!m_Stop.load(std::memory_order_acquire))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return 0;
        }

        void Stop() override
        {
            m_Stop.store(true, std::memory_order_release);
        }

        std::atomic<bool> m_Started{ false };
        std::atomic<bool> m_Stop{ false };
    };

    // True iff a thread with the given name is currently in the registry. Only
    // dereferences pointers for live (registered) threads, so it is safe to call
    // after a thread has been destroyed.
    bool RegistryContainsName(std::string_view name)
    {
        bool found = false;
        FThreadManager::Get().ForEachThread(
            [&](u32, FRunnableThread* thread)
            {
                if (thread != nullptr && thread->GetThreadName() == name)
                {
                    found = true;
                }
            });
        return found;
    }

    // Attaches its worker thread to a named-thread role, then parks until stopped
    // (detaching on the way out). Used to verify the named-thread manager and the
    // global registry agree on the OS thread id.
    class NamedAttachRunnable final : public FRunnable
    {
      public:
        explicit NamedAttachRunnable(Tasks::ENamedThread role) : m_Role(role) {}

        u32 Run() override
        {
            Tasks::FNamedThreadManager::Get().AttachToThread(m_Role);
            m_AttachedId.store(FPlatformTLS::GetCurrentThreadId(), std::memory_order_release);
            m_Started.store(true, std::memory_order_release);
            while (!m_Stop.load(std::memory_order_acquire))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            Tasks::FNamedThreadManager::Get().DetachFromThread(m_Role);
            return 0;
        }

        void Stop() override
        {
            m_Stop.store(true, std::memory_order_release);
        }

        Tasks::ENamedThread m_Role;
        std::atomic<u32> m_AttachedId{ 0 };
        std::atomic<bool> m_Started{ false };
        std::atomic<bool> m_Stop{ false };
    };

    struct AutoDeleteState
    {
        std::atomic<u32> InitCalls{ 0 };
        std::atomic<u32> RunCalls{ 0 };
        std::atomic<u32> ExitCalls{ 0 };
        std::atomic<u32> Destructions{ 0 };
        std::atomic<u32> ThreadId{ 0 };
        FManualResetEvent Started;
        FManualResetEvent Release;
        FManualResetEvent Destroyed;
    };

    class AutoDeleteRunnable final : public FRunnable
    {
      public:
        AutoDeleteRunnable(std::shared_ptr<AutoDeleteState> state, bool initSucceeds, bool park)
            : m_State(std::move(state)), m_InitSucceeds(initSucceeds), m_Park(park)
        {
        }

        ~AutoDeleteRunnable() override
        {
            m_State->Destructions.fetch_add(1, std::memory_order_release);
            m_State->Destroyed.Notify();
        }

        bool Init() override
        {
            m_State->InitCalls.fetch_add(1, std::memory_order_relaxed);
            return m_InitSucceeds;
        }

        u32 Run() override
        {
            m_State->RunCalls.fetch_add(1, std::memory_order_relaxed);
            m_State->ThreadId.store(FPlatformTLS::GetCurrentThreadId(), std::memory_order_release);
            m_State->Started.Notify();
            if (m_Park)
            {
                m_State->Release.Wait();
            }
            return 0;
        }

        void Exit() override
        {
            m_State->ExitCalls.fetch_add(1, std::memory_order_release);
        }

      private:
        std::shared_ptr<AutoDeleteState> m_State;
        bool m_InitSucceeds;
        bool m_Park;
    };

    bool WaitForRegistryRemoval(std::string_view name)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (RegistryContainsName(name))
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return false;
            }
            std::this_thread::yield();
        }
        return true;
    }

    struct AsyncCallbackState
    {
        std::atomic<u32> Callbacks{ 0 };
        std::atomic<u32> CaptureDestructions{ 0 };
        FManualResetEvent Started;
        FManualResetEvent Release;
        FManualResetEvent CallbackDone;
        FManualResetEvent CaptureDestroyed;
    };

    struct AsyncCaptureLifetime
    {
        explicit AsyncCaptureLifetime(std::shared_ptr<AsyncCallbackState> state) : State(std::move(state)) {}
        ~AsyncCaptureLifetime()
        {
            State->CaptureDestructions.fetch_add(1, std::memory_order_release);
            State->CaptureDestroyed.Notify();
        }
        std::shared_ptr<AsyncCallbackState> State;
    };

    int RunAsyncFromRegistryCallback()
    {
        // This watchdog runs only inside the death-test child. A lock-order
        // regression exits that child instead of hanging the whole suite.
        std::thread([]
                    {
                        std::this_thread::sleep_for(std::chrono::seconds(5));
                        std::_Exit(124); })
            .detach();

        ParkingRunnable parked;
        auto sentinel = std::unique_ptr<FRunnableThread>(FRunnableThread::Create(&parked, "OloTest::RegistryCallbackSentinel"));
        if (!sentinel)
            return 1;

        auto state = std::make_shared<AsyncCallbackState>();
        u32 outerCallbacks = 0;
        u32 nestedCallbacks = 0;
        bool completed = false;
        FThreadManager::Get().ForEachThread(
            [&](u32, FRunnableThread*)
            {
                if (++outerCallbacks != 1u)
                    return;
                auto lifetime = std::make_unique<AsyncCaptureLifetime>(state);
                auto future = AsyncThread([state, lifetime = std::move(lifetime)]
                                          {
                                              state->Started.Notify();
                                              state->Release.Wait();
                                              return lifetime ? 37 : -1; },
                                          0, EThreadPriority::TPri_Normal,
                                          [state]
                                          {
                                              state->Callbacks.fetch_add(1, std::memory_order_release);
                                              state->CallbackDone.Notify();
                                          });
                const bool started = state->Started.WaitFor(FMonotonicTimeSpan::FromSeconds(2.0));
                // The parked async thread is still running, so its registration
                // mutates the outer enumeration. A nested read must not hide it.
                FThreadManager::Get().ForEachThread([&](u32, FRunnableThread*)
                                                    { ++nestedCallbacks; });
                state->Release.Notify();
                completed = started && future.WaitFor(FMonotonicTimeSpan::FromSeconds(2.0)) &&
                            future.Get() == 37 &&
                            state->CallbackDone.WaitFor(FMonotonicTimeSpan::FromSeconds(2.0));
            });
        // Cleanup can need the registry lock held by ForEachThread. Await it
        // after leaving the callback, while the callable itself needs no registry.
        if (outerCallbacks != 1u || nestedCallbacks < 2u || !completed ||
            !state->CaptureDestroyed.WaitFor(FMonotonicTimeSpan::FromSeconds(2.0)))
            return 2;
        if (state->Callbacks.load(std::memory_order_acquire) != 1u ||
            state->CaptureDestructions.load(std::memory_order_acquire) != 1u)
            return 3;
        sentinel.reset();
        return 0;
    }
} // namespace

TEST(ThreadManagerTest, RegistersThreadOnCreateAndDeregistersOnDestroy)
{
    auto& manager = FThreadManager::Get();

    constexpr const char* kThreadName = "OloTest::InspectorWorker";
    ASSERT_FALSE(RegistryContainsName(kThreadName)) << "Stale registration from a prior run";

    const i32 countBefore = manager.NumThreads();

    ParkingRunnable runnable;
    FRunnableThread* thread = FRunnableThread::Create(&runnable, kThreadName);
    ASSERT_NE(thread, nullptr);

    // Create() only returns after the worker signalled init, so the registry is
    // already populated and the OS thread id is assigned.
    EXPECT_EQ(manager.NumThreads(), countBefore + 1);
    EXPECT_NE(thread->GetThreadID(), 0u);
    EXPECT_TRUE(RegistryContainsName(kThreadName));

    // The registry entry must point back at the exact thread object, keyed by id.
    bool foundByPointer = false;
    u32 foundId = 0;
    manager.ForEachThread(
        [&](u32 id, FRunnableThread* t)
        {
            if (t == thread)
            {
                foundByPointer = true;
                foundId = id;
            }
        });
    EXPECT_TRUE(foundByPointer);
    EXPECT_EQ(foundId, thread->GetThreadID());

    // Name lookup by id resolves through the registry.
    EXPECT_EQ(FThreadManager::GetThreadName(thread->GetThreadID()), kThreadName);

    const u32 threadId = thread->GetThreadID();

    // Destruction (which stops + joins the worker) must deregister.
    delete thread;

    EXPECT_EQ(manager.NumThreads(), countBefore);
    EXPECT_FALSE(RegistryContainsName(kThreadName));
    // Unknown ids fall back to the sentinel name.
    EXPECT_EQ(FThreadManager::GetThreadName(threadId), "UnknownThread");
}

TEST(ThreadManagerTest, TracksMultipleConcurrentThreads)
{
    auto& manager = FThreadManager::Get();
    const i32 countBefore = manager.NumThreads();

    constexpr i32 kCount = 4;
    ParkingRunnable runnables[kCount];
    FRunnableThread* threads[kCount] = {};

    for (i32 i = 0; i < kCount; ++i)
    {
        threads[i] = FRunnableThread::Create(&runnables[i], "OloTest::MultiWorker");
        ASSERT_NE(threads[i], nullptr);
    }

    EXPECT_EQ(manager.NumThreads(), countBefore + kCount);

    for (i32 i = 0; i < kCount; ++i)
    {
        delete threads[i];
    }

    EXPECT_EQ(manager.NumThreads(), countBefore);
}

// The point of folding the id-consistency fix into #282: the named-thread manager
// and the global registry must key threads by the same OS thread id, so the editor
// can label registry rows with their precise named role.
TEST(ThreadManagerTest, NamedThreadIdAgreesWithRegistryId)
{
    auto& named = Tasks::FNamedThreadManager::Get();
    constexpr Tasks::ENamedThread kRole = Tasks::ENamedThread::NetworkThread;
    ASSERT_EQ(named.GetThreadId(kRole), 0u) << "Role unexpectedly attached before test";

    NamedAttachRunnable runnable(kRole);
    FRunnableThread* thread = FRunnableThread::Create(&runnable, "OloTest::NamedWorker");
    ASSERT_NE(thread, nullptr);

    // Wait for the worker to attach itself, bounded so a stuck thread fails CI fast
    // instead of hanging forever.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!runnable.m_Started.load(std::memory_order_acquire))
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            GTEST_FAIL() << "Worker did not reach AttachToThread() / set runnable.m_Started within 5s";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const u32 osId = runnable.m_AttachedId.load(std::memory_order_acquire);
    EXPECT_NE(osId, 0u);
    // All three id sources must agree: the raw OS id captured on the worker, the
    // FRunnableThread registry entry, and the named-thread manager's record.
    EXPECT_EQ(thread->GetThreadID(), osId);
    EXPECT_EQ(named.GetThreadId(kRole), osId);
    EXPECT_TRUE(named.IsThreadAttached(kRole));

    delete thread; // worker detaches from the role as it exits
    EXPECT_EQ(named.GetThreadId(kRole), 0u);
    EXPECT_FALSE(named.IsThreadAttached(kRole));
}

// A dedicated async thread owns both its runnable and native thread bookkeeping.
// Completion can beat the factory's return, so the creator must remain protected
// until registration/publication has finished. This crosses the production HAL
// seam without substituting thread creation, entry or registry cleanup.
TEST(ThreadManagerTest, AutoDeleteImmediateCompletionReleasesOwnershipExactlyOnce)
{
    constexpr const char* name = "OloTest::AutoDeleteImmediate";
    ASSERT_FALSE(RegistryContainsName(name));
    for (u32 iteration = 0; iteration < 128; ++iteration)
    {
        SCOPED_TRACE(iteration);
        auto state = std::make_shared<AutoDeleteState>();
        auto runnable = std::make_unique<AutoDeleteRunnable>(state, true, false);
        const bool created = FRunnableThread::CreateAutoDelete(runnable.get(), name);
        if (created)
        {
            // The pointer may already be destroyed. Releasing the former caller
            // owner does not dereference it and prevents a second destruction.
            runnable.release();
        }
        ASSERT_TRUE(created);
        ASSERT_TRUE(state->Destroyed.WaitFor(FMonotonicTimeSpan::FromSeconds(5.0)));
        EXPECT_EQ(state->InitCalls.load(std::memory_order_acquire), 1u);
        EXPECT_EQ(state->RunCalls.load(std::memory_order_acquire), 1u);
        EXPECT_EQ(state->ExitCalls.load(std::memory_order_acquire), 1u);
        EXPECT_EQ(state->Destructions.load(std::memory_order_acquire), 1u);
        ASSERT_TRUE(WaitForRegistryRemoval(name));
    }
}

TEST(ThreadManagerTest, AutoDeleteParkedRunnableRemainsRegisteredUntilCompletion)
{
    constexpr const char* name = "OloTest::AutoDeleteParkedNameSnapshotLongEnoughToAllocate";
    auto state = std::make_shared<AutoDeleteState>();
    auto runnable = std::make_unique<AutoDeleteRunnable>(state, true, true);
    const bool created = FRunnableThread::CreateAutoDelete(runnable.get(), name);
    if (created)
    {
        runnable.release();
    }
    ASSERT_TRUE(created);
    EXPECT_TRUE(state->Started.WaitFor(FMonotonicTimeSpan::FromSeconds(5.0)));
    EXPECT_TRUE(RegistryContainsName(name));
    EXPECT_EQ(state->Destructions.load(std::memory_order_acquire), 0u);
    const u32 threadId = state->ThreadId.load(std::memory_order_acquire);
    EXPECT_NE(threadId, 0u);
    // Binding a returned value to a const reference keeps the copied name alive
    // across registry removal; returning a reference into the thread cannot.
    const std::string& capturedName = FThreadManager::GetThreadName(threadId);

    // Always release the parked thread, including after a failed expectation.
    state->Release.Notify();
    ASSERT_TRUE(state->Destroyed.WaitFor(FMonotonicTimeSpan::FromSeconds(5.0)));
    EXPECT_EQ(state->RunCalls.load(std::memory_order_acquire), 1u);
    EXPECT_EQ(state->ExitCalls.load(std::memory_order_acquire), 1u);
    EXPECT_EQ(state->Destructions.load(std::memory_order_acquire), 1u);
    EXPECT_TRUE(WaitForRegistryRemoval(name));
    EXPECT_EQ(capturedName, name);
}

TEST(ThreadManagerTest, AutoDeleteInitFailureRetainsCallerOwnership)
{
    constexpr const char* name = "OloTest::AutoDeleteRejected";
    auto state = std::make_shared<AutoDeleteState>();
    auto runnable = std::make_unique<AutoDeleteRunnable>(state, false, false);
    const bool created = FRunnableThread::CreateAutoDelete(runnable.get(), name);
    if (created)
    {
        runnable.release();
    }
    ASSERT_FALSE(created);
    EXPECT_EQ(state->InitCalls.load(std::memory_order_acquire), 1u);
    EXPECT_EQ(state->RunCalls.load(std::memory_order_acquire), 0u);
    EXPECT_EQ(state->ExitCalls.load(std::memory_order_acquire), 1u);
    EXPECT_EQ(state->Destructions.load(std::memory_order_acquire), 0u);
    EXPECT_FALSE(RegistryContainsName(name));

    runnable.reset();
    EXPECT_EQ(state->Destructions.load(std::memory_order_acquire), 1u);
}

TEST(ThreadManagerTest, CompletedConditionalRegistrationDoesNotReplaceLiveReusedId)
{
    auto state = std::make_shared<AutoDeleteState>();
    AutoDeleteRunnable completedRunnable(state, true, false);
    auto completed = std::unique_ptr<FRunnableThread>(FRunnableThread::Create(&completedRunnable, "OloTest::CompletedRegistration"));
    ASSERT_NE(completed, nullptr);
    completed->WaitForCompletion();
    ASSERT_FALSE(completed->IsRunning());
    // Ordinary joinable threads retain their registration until destruction.
    EXPECT_TRUE(RegistryContainsName("OloTest::CompletedRegistration"));

    ParkingRunnable liveRunnable;
    auto live = std::unique_ptr<FRunnableThread>(FRunnableThread::Create(&liveRunnable, "OloTest::ReusedRegistrationId"));
    ASSERT_NE(live, nullptr);
    const u32 reusedId = live->GetThreadID();
    FThreadManager::Get().AddThread(reusedId, completed.get(), true);
    bool liveRetained = false;
    FThreadManager::Get().ForEachThread(
        [&](u32 id, FRunnableThread* thread)
        {
            if (id == reusedId)
                liveRetained = thread == live.get();
        });
    EXPECT_TRUE(liveRetained) << "late registration of a completed thread must not replace a live reused id";

    // Restore the supplied key even on regression, before destroying the stale
    // object, so a failing expectation cannot leave a dangling registry pointer.
    FThreadManager::Get().AddThread(reusedId, live.get());
    completed.reset();
    EXPECT_TRUE(RegistryContainsName("OloTest::ReusedRegistrationId"));
}

TEST(ThreadManagerTest, RegistryCallbackCanLaunchAndAwaitDedicatedAsyncWork)
{
    const std::string previousStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({ std::_Exit(RunAsyncFromRegistryCallback()); }, ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, previousStyle);
}
