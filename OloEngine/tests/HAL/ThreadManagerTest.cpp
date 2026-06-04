/**
 * @file ThreadManagerTest.cpp
 * @brief Unit tests for FThreadManager / FRunnableThread registration wiring.
 *
 * Verifies that every thread created via FRunnableThread::Create() registers
 * itself in the global FThreadManager registry on creation and deregisters on
 * destruction. This is the data source behind the editor's Thread Inspector
 * panel, so the registration contract is what we pin here.
 */

#include <gtest/gtest.h>

#include "OloEngine/Core/PlatformTLS.h"
#include "OloEngine/HAL/Runnable.h"
#include "OloEngine/HAL/RunnableThread.h"
#include "OloEngine/HAL/ThreadManager.h"
#include "OloEngine/Task/NamedThreads.h"

#include <atomic>
#include <chrono>
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

    while (!runnable.m_Started.load(std::memory_order_acquire))
    {
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
