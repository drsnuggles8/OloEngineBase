#pragma once

/**
 * @file PlatformTLS.h
 * @brief Platform Thread-Local Storage abstraction
 * 
 * Provides manual TLS slot management matching UE5.7's FPlatformTLS.
 * 
 * This is preferred over C++ thread_local because:
 * 1. No destructor ordering issues during thread exit
 * 2. No DLL boundary problems on Windows
 * 3. Explicit control over lifetime (can intentionally skip cleanup on thread exit)
 * 4. Works correctly in static initialization/destruction order
 * 5. Matches UE5.7 pattern for easier code porting
 * 
 * Ported from Unreal Engine's HAL/PlatformTLS.h and Windows/WindowsPlatformTLS.h
 */

#include "OloEngine/Core/Base.h"

#if defined(OLO_PLATFORM_WINDOWS)
    #include <Windows.h>
#else
    #include <pthread.h>
#endif

namespace OloEngine
{
    /**
     * @class FPlatformTLS
     * @brief Cross-platform Thread-Local Storage API
     * 
     * Provides low-level TLS slot management. Each slot can store a void* per thread.
     * Slots must be explicitly allocated and freed.
     * 
     * @note Unlike C++ thread_local, destructors are NOT automatically called on thread exit.
     *       This is intentional - it avoids issues during thread teardown and matches UE5.7 behavior.
     * 
     * Usage:
     * @code
     *     // At initialization (main thread)
     *     u32 Slot = FPlatformTLS::AllocTlsSlot();
     *     
     *     // Per-thread usage
     *     FPlatformTLS::SetTlsValue(Slot, myData);
     *     void* data = FPlatformTLS::GetTlsValue(Slot);
     *     
     *     // At shutdown
     *     FPlatformTLS::FreeTlsSlot(Slot);
     * @endcode
     */
    class FPlatformTLS
    {
    public:
        /** Invalid TLS slot sentinel value */
        static constexpr u32 InvalidTlsSlot = 0xFFFFFFFF;

        /**
         * @brief Check if a TLS slot index is valid
         * @param SlotIndex The slot index to check
         * @return true if valid, false if it's the invalid sentinel
         */
        static bool IsValidTlsSlot(u32 SlotIndex)
        {
            return SlotIndex != InvalidTlsSlot;
        }

        /**
         * @brief Get the current thread's unique identifier
         * @return Thread ID (platform-specific value)
         */
        static u32 GetCurrentThreadId()
        {
#if defined(OLO_PLATFORM_WINDOWS)
            return ::GetCurrentThreadId();
#else
            return static_cast<u32>(pthread_self());
#endif
        }

#if defined(OLO_PLATFORM_WINDOWS)
        /**
         * @brief Allocate a new TLS slot
         * @return Slot index, or InvalidTlsSlot on failure
         */
        static u32 AllocTlsSlot()
        {
            DWORD Slot = ::TlsAlloc();
            return Slot == TLS_OUT_OF_INDEXES ? InvalidTlsSlot : static_cast<u32>(Slot);
        }

        /**
         * @brief Free a previously allocated TLS slot
         * @param SlotIndex The slot to free
         */
        static void FreeTlsSlot(u32 SlotIndex)
        {
            ::TlsFree(SlotIndex);
        }

        /**
         * @brief Set the value in a TLS slot for the current thread
         * @param SlotIndex The slot to set
         * @param Value The value to store (can be nullptr)
         */
        static void SetTlsValue(u32 SlotIndex, void* Value)
        {
            ::TlsSetValue(SlotIndex, Value);
        }

        /**
         * @brief Get the value from a TLS slot for the current thread
         * @param SlotIndex The slot to read
         * @return The stored value, or nullptr if not set
         */
        static void* GetTlsValue(u32 SlotIndex)
        {
            return ::TlsGetValue(SlotIndex);
        }
#else
        // POSIX implementation using pthread_key_t
        static u32 AllocTlsSlot()
        {
            pthread_key_t Key;
            if (pthread_key_create(&Key, nullptr) != 0)
            {
                return InvalidTlsSlot;
            }
            return static_cast<u32>(Key);
        }

        static void FreeTlsSlot(u32 SlotIndex)
        {
            pthread_key_delete(static_cast<pthread_key_t>(SlotIndex));
        }

        static void SetTlsValue(u32 SlotIndex, void* Value)
        {
            pthread_setspecific(static_cast<pthread_key_t>(SlotIndex), Value);
        }

        static void* GetTlsValue(u32 SlotIndex)
        {
            return pthread_getspecific(static_cast<pthread_key_t>(SlotIndex));
        }
#endif
    };

} // namespace OloEngine
