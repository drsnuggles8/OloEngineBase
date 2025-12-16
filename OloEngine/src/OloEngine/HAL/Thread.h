// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#pragma once

/**
 * @file Thread.h
 * @brief High-level thread wrapper with automatic lifetime management
 * 
 * FThread is the preferred way to create threads in OloEngine. It provides:
 * - Automatic thread naming for debugging
 * - Thread priority and affinity configuration
 * - TLS integration via FRunnableThread
 * - Move-only semantics (no copies)
 * - RAII-style resource management
 * 
 * Ported from Unreal Engine's HAL/Thread.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/PlatformTLS.h"
#include "OloEngine/HAL/Runnable.h"
#include "OloEngine/HAL/RunnableThread.h"
#include "OloEngine/HAL/PlatformMisc.h"
#include "OloEngine/Templates/UnrealTemplate.h"

#include <functional>
#include <memory>
#include <atomic>

namespace OloEngine
{
    /**
     * @class FThread
     * @brief High-level thread wrapper
     * 
     * FThread wraps a system thread with a simple interface. It takes a
     * callable (function, lambda, functor) and runs it on a new thread.
     * 
     * Usage:
     * @code
     *     FThread MyThread(
     *         "WorkerThread",
     *         []() { DoWork(); },
     *         0,  // stack size (0 = default)
     *         EThreadPriority::TPri_Normal,
     *         FThreadAffinity{}
     *     );
     *     // ... do other work ...
     *     MyThread.Join();
     * @endcode
     */
    class FThread final
    {
    public:
        /**
         * @enum EForkable
         * @brief Whether the thread can survive process forking
         */
        enum EForkable
        {
            Forkable,       // Thread can survive fork (Linux/Unix)
            NonForkable     // Thread is killed on fork (default)
        };

        /**
         * @brief Invalid thread ID constant
         */
        static constexpr u32 InvalidThreadId = ~u32(0);

        /**
         * @brief Default constructor - creates empty thread object
         * 
         * An empty FThread is not joinable and has no associated system thread.
         */
        FThread() = default;

        /**
         * @brief Main constructor - creates and starts a new thread
         * 
         * @param ThreadName Name for the thread (shows in debuggers/profilers)
         * @param ThreadFunction The function to run on the thread
         * @param StackSize Stack size in bytes (0 = default)
         * @param ThreadPriority Thread scheduling priority
         * @param ThreadAffinity CPU affinity settings
         * @param IsForkable Whether thread survives process fork
         */
        FThread(
            const char* ThreadName,
            std::function<void()> ThreadFunction,
            u32 StackSize = 0,
            EThreadPriority ThreadPriority = EThreadPriority::TPri_Normal,
            FThreadAffinity ThreadAffinity = FThreadAffinity{},
            EForkable IsForkable = NonForkable);

        /**
         * @brief Constructor with single-thread tick function
         * 
         * For platforms that don't support multithreading, the tick function
         * is called repeatedly from the main thread instead.
         * 
         * @param ThreadName Name for the thread
         * @param ThreadFunction The function to run on the thread
         * @param SingleThreadTickFunction Function to tick when threading unavailable
         * @param StackSize Stack size in bytes (0 = default)
         * @param ThreadPriority Thread scheduling priority
         * @param ThreadAffinity CPU affinity settings
         * @param IsForkable Whether thread survives process fork
         */
        FThread(
            const char* ThreadName,
            std::function<void()> ThreadFunction,
            std::function<void()> SingleThreadTickFunction,
            u32 StackSize = 0,
            EThreadPriority ThreadPriority = EThreadPriority::TPri_Normal,
            FThreadAffinity ThreadAffinity = FThreadAffinity{},
            EForkable IsForkable = NonForkable);

        // Non-copyable
        FThread(const FThread&) = delete;
        FThread& operator=(const FThread&) = delete;

        // Move-only
        FThread(FThread&& Other) noexcept;
        FThread& operator=(FThread&& Other) noexcept;

        /**
         * @brief Destructor
         * 
         * Asserts if the thread is still joinable (not joined or detached).
         * Always call Join() before destruction.
         */
        ~FThread();

        /**
         * @brief Check if the thread is joinable
         * 
         * A thread is joinable if it has an associated system thread that
         * hasn't been joined yet, and is not the current thread.
         * 
         * @return True if Join() can be called
         */
        bool IsJoinable() const;

        /**
         * @brief Wait for the thread to complete
         * 
         * Blocks the calling thread until this thread finishes execution.
         * After Join() returns, the thread is no longer joinable.
         */
        void Join();

        /**
         * @brief Get the thread's ID
         * 
         * @return The thread ID, or InvalidThreadId if not valid
         */
        u32 GetThreadId() const;

    private:
        /**
         * @class FThreadImpl
         * @brief Internal implementation that adapts function to FRunnable
         */
        class FThreadImpl final : public FRunnable, public FSingleThreadRunnable
        {
        public:
            FThreadImpl(
                const char* ThreadName,
                std::function<void()> InThreadFunction,
                std::function<void()> InSingleThreadTickFunction,
                u32 StackSize,
                EThreadPriority ThreadPriority,
                FThreadAffinity ThreadAffinity,
                EForkable IsForkable);

            ~FThreadImpl();

            // Provide reference to self (can't use shared_from_this in constructor)
            void Initialize(const std::shared_ptr<FThreadImpl>& InSelf);

            bool IsJoinable() const;
            void Join();
            u32 GetThreadId() const;

        private:
            // FRunnable interface
            u32 Run() override;
            void Exit() override;

            // FSingleThreadRunnable interface
            FSingleThreadRunnable* GetSingleThreadInterface() override;
            void Tick() override;

        private:
            // Two references: one in FThread, one here (Self)
            // Self is released in Exit(), FThread's is released on destruction
            std::shared_ptr<FThreadImpl> m_Self;
            std::atomic<bool> m_bIsInitialized{ false };

            std::function<void()> m_ThreadFunction;
            std::function<void()> m_SingleThreadTickFunction;
            std::unique_ptr<FRunnableThread> m_RunnableThread;
        };

        std::shared_ptr<FThreadImpl> m_Impl;
    };

    //=============================================================================
    // IMPLEMENTATION
    //=============================================================================

    // FThread::FThreadImpl implementation

    inline FThread::FThreadImpl::FThreadImpl(
        const char* ThreadName,
        std::function<void()> InThreadFunction,
        std::function<void()> InSingleThreadTickFunction,
        u32 StackSize,
        EThreadPriority ThreadPriority,
        FThreadAffinity ThreadAffinity,
        EForkable IsForkable)
        : m_ThreadFunction(MoveTemp(InThreadFunction))
        , m_SingleThreadTickFunction(MoveTemp(InSingleThreadTickFunction))
    {
        (void)IsForkable; // Forkable threads not yet implemented

        // Create the underlying thread
        m_RunnableThread.reset(FRunnableThread::Create(
            this,
            ThreadName,
            StackSize,
            ThreadPriority,
            ThreadAffinity.ThreadAffinityMask,
            EThreadCreateFlags::None));

        OLO_CORE_ASSERT(m_RunnableThread != nullptr, "Failed to create thread");

        // Apply processor group affinity if specified
        if (m_RunnableThread && ThreadAffinity.ProcessorGroup != 0)
        {
            m_RunnableThread->SetThreadAffinity(ThreadAffinity);
        }
    }

    inline FThread::FThreadImpl::~FThreadImpl() = default;

    inline void FThread::FThreadImpl::Initialize(const std::shared_ptr<FThreadImpl>& InSelf)
    {
        m_Self = InSelf;
        m_bIsInitialized.store(true, std::memory_order_release);
    }

    inline bool FThread::FThreadImpl::IsJoinable() const
    {
        if (!m_RunnableThread)
        {
            return false;
        }

        // Can't join from the same thread - use platform API for thread ID
        u32 CurrentThreadId = FPlatformTLS::GetCurrentThreadId();
        return CurrentThreadId != m_RunnableThread->GetThreadID();
    }

    inline void FThread::FThreadImpl::Join()
    {
        OLO_CORE_ASSERT(IsJoinable(), "Thread is not joinable");
        m_RunnableThread->WaitForCompletion();
        m_RunnableThread.reset();
    }

    inline u32 FThread::FThreadImpl::GetThreadId() const
    {
        return m_RunnableThread ? m_RunnableThread->GetThreadID() : FThread::InvalidThreadId;
    }

    inline u32 FThread::FThreadImpl::Run()
    {
        if (m_ThreadFunction)
        {
            m_ThreadFunction();
        }
        return 0;
    }

    inline void FThread::FThreadImpl::Exit()
    {
        // Busy-wait until Self is initialized
        while (!m_bIsInitialized.load(std::memory_order_acquire))
        {
            // Spin
        }

        OLO_CORE_ASSERT(m_Self != nullptr, "Self should be valid");
        m_Self.reset(); // Release self-reference, may trigger deletion
    }

    inline FSingleThreadRunnable* FThread::FThreadImpl::GetSingleThreadInterface()
    {
        return m_SingleThreadTickFunction ? this : nullptr;
    }

    inline void FThread::FThreadImpl::Tick()
    {
        if (m_SingleThreadTickFunction)
        {
            m_SingleThreadTickFunction();
        }
    }

    // FThread implementation

    inline FThread::FThread(
        const char* ThreadName,
        std::function<void()> ThreadFunction,
        u32 StackSize,
        EThreadPriority ThreadPriority,
        FThreadAffinity ThreadAffinity,
        EForkable IsForkable)
        : FThread(ThreadName, MoveTemp(ThreadFunction), std::function<void()>{}, 
                  StackSize, ThreadPriority, ThreadAffinity, IsForkable)
    {
    }

    inline FThread::FThread(
        const char* ThreadName,
        std::function<void()> ThreadFunction,
        std::function<void()> SingleThreadTickFunction,
        u32 StackSize,
        EThreadPriority ThreadPriority,
        FThreadAffinity ThreadAffinity,
        EForkable IsForkable)
    {
        m_Impl = std::make_shared<FThreadImpl>(
            ThreadName,
            MoveTemp(ThreadFunction),
            MoveTemp(SingleThreadTickFunction),
            StackSize,
            ThreadPriority,
            ThreadAffinity,
            IsForkable);

        m_Impl->Initialize(m_Impl);
    }

    inline FThread::FThread(FThread&& Other) noexcept
        : m_Impl(MoveTemp(Other.m_Impl))
    {
    }

    inline FThread& FThread::operator=(FThread&& Other) noexcept
    {
        OLO_CORE_ASSERT(!IsJoinable(), "Cannot move-assign to a joinable thread");
        m_Impl = MoveTemp(Other.m_Impl);
        return *this;
    }

    inline FThread::~FThread()
    {
        OLO_CORE_ASSERT(!IsJoinable(), "Thread must be joined before destruction");
    }

    inline bool FThread::IsJoinable() const
    {
        return m_Impl && m_Impl->IsJoinable();
    }

    inline void FThread::Join()
    {
        OLO_CORE_ASSERT(m_Impl, "Cannot join empty thread");
        m_Impl->Join();
    }

    inline u32 FThread::GetThreadId() const
    {
        return m_Impl ? m_Impl->GetThreadId() : InvalidThreadId;
    }

} // namespace OloEngine
