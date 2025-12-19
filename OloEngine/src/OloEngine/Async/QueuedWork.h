/**
 * @file QueuedWork.h
 * @brief Queued work interface for thread pools
 *
 * Ported from Unreal Engine 5.7 - Epic Games, Inc.
 */

#pragma once

#include "OloEngine/Core/Base.h"
#include <atomic>

namespace OLO
{
    /** Special flags that can be associated with queued work. */
    enum class EQueuedWorkFlags : u8
    {
        None = 0,

        /**
         * Tells the scheduler if this task is allowed to run during another task's
         * busy wait. The default should be true for most cases but it
         * is sometimes useful to avoid it if this task is going to wait on another one,
         * and that other task busy waits, this could cause a cycle that could deadlock.
         * (i.e. T1 -> busywait -> picks T2 that then waits on T1 -> deadlock)
         * In this case, we can decide that T2 should never be picked up by busy waits.
         */
        DoNotRunInsideBusyWait = (1 << 0),
    };

    inline EQueuedWorkFlags operator|(EQueuedWorkFlags a, EQueuedWorkFlags b)
    {
        return static_cast<EQueuedWorkFlags>(static_cast<u8>(a) | static_cast<u8>(b));
    }

    inline EQueuedWorkFlags operator&(EQueuedWorkFlags a, EQueuedWorkFlags b)
    {
        return static_cast<EQueuedWorkFlags>(static_cast<u8>(a) & static_cast<u8>(b));
    }

    inline EQueuedWorkFlags& operator|=(EQueuedWorkFlags& a, EQueuedWorkFlags b)
    {
        return a = a | b;
    }

    /**
     * Interface for internal data of queued work objects.
     *
     * This interface can be used to track some data between the individual function invocations.
     * Usually it is used to store some internal state to support cancellation.
     */
    class IQueuedWorkInternalData
    {
      public:
        virtual ~IQueuedWorkInternalData() = default;

        /**
         * Called during retraction, when a task is pulled from being worked on.
         * @return true if the cancellation succeeded
         */
        virtual bool Retract() = 0;

        void AddRef()
        {
            m_RefCount.fetch_add(1, std::memory_order_relaxed);
        }

        u32 Release()
        {
            u32 refs = m_RefCount.fetch_sub(1, std::memory_order_acq_rel);
            if (refs == 1)
            {
                delete this;
            }
            return refs - 1;
        }

        u32 GetRefCount() const
        {
            return m_RefCount.load(std::memory_order_relaxed);
        }

      private:
        std::atomic<u32> m_RefCount{ 0 };
    };

    /**
     * Reference-counted pointer to internal data
     */
    class FQueuedWorkInternalDataRef
    {
      public:
        FQueuedWorkInternalDataRef() = default;

        explicit FQueuedWorkInternalDataRef(IQueuedWorkInternalData* ptr) : m_Ptr(ptr)
        {
            if (m_Ptr)
                m_Ptr->AddRef();
        }

        FQueuedWorkInternalDataRef(const FQueuedWorkInternalDataRef& other) : m_Ptr(other.m_Ptr)
        {
            if (m_Ptr)
                m_Ptr->AddRef();
        }

        FQueuedWorkInternalDataRef(FQueuedWorkInternalDataRef&& other) noexcept : m_Ptr(other.m_Ptr)
        {
            other.m_Ptr = nullptr;
        }

        ~FQueuedWorkInternalDataRef()
        {
            if (m_Ptr)
                m_Ptr->Release();
        }

        FQueuedWorkInternalDataRef& operator=(const FQueuedWorkInternalDataRef& other)
        {
            if (this != &other)
            {
                if (m_Ptr)
                    m_Ptr->Release();
                m_Ptr = other.m_Ptr;
                if (m_Ptr)
                    m_Ptr->AddRef();
            }
            return *this;
        }

        FQueuedWorkInternalDataRef& operator=(FQueuedWorkInternalDataRef&& other) noexcept
        {
            if (this != &other)
            {
                if (m_Ptr)
                    m_Ptr->Release();
                m_Ptr = other.m_Ptr;
                other.m_Ptr = nullptr;
            }
            return *this;
        }

        FQueuedWorkInternalDataRef& operator=(std::nullptr_t)
        {
            if (m_Ptr)
            {
                m_Ptr->Release();
                m_Ptr = nullptr;
            }
            return *this;
        }

        IQueuedWorkInternalData* Get() const
        {
            return m_Ptr;
        }
        IQueuedWorkInternalData* operator->() const
        {
            return m_Ptr;
        }
        explicit operator bool() const
        {
            return m_Ptr != nullptr;
        }
        bool IsValid() const
        {
            return m_Ptr != nullptr;
        }

      private:
        IQueuedWorkInternalData* m_Ptr = nullptr;
    };

    /**
     * Interface for queued work objects.
     *
     * This interface is a type of runnable object that requires no per-thread
     * initialization. It is meant to be used with pools of threads in an
     * abstract way that prevents the pool from needing to know any details
     * about the object being run. This allows queuing of disparate tasks and
     * servicing those tasks with a generic thread pool.
     */
    class IQueuedWork
    {
      public:
        virtual ~IQueuedWork() = default;

        /**
         * This is where the real thread work is done. All work that is done for
         * this queued object should be done from within the call to this function.
         */
        virtual void DoThreadedWork() = 0;

        /**
         * Tells the queued work that it is being abandoned so that it can do
         * per-object clean up as needed. This will only be called if it is being
         * abandoned before completion. NOTE: This requires the object to delete
         * itself using whatever heap it was allocated in.
         */
        virtual void Abandon() = 0;

        /**
         * Returns any special work flags.
         */
        virtual EQueuedWorkFlags GetQueuedWorkFlags() const
        {
            return EQueuedWorkFlags::None;
        }

        /**
         * Returns an approximation of the peak memory (in bytes) this task could require during execution.
         * @return Negative value means unknown
         */
        virtual i64 GetRequiredMemory() const
        {
            return -1;
        }

        /**
         * Returns text to identify the Work, for debug/log purposes only
         */
        virtual const char* GetDebugName() const
        {
            return nullptr;
        }

        /**
         * Internal data can be used by the pool for tracking/cancellation
         */
        FQueuedWorkInternalDataRef InternalData;
    };

} // namespace OLO
