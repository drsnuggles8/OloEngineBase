// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine::LowLevelTasks
{
    namespace Private
    {
        // @class FOversubscriptionTls
        // @brief Thread-local storage for oversubscription state
        //
        // Tracks whether oversubscription is allowed on the current thread.
        // This is used to control whether additional worker threads can be
        // spawned during blocking operations.
        class FOversubscriptionTls
        {
            static thread_local bool s_bIsOversubscriptionAllowed;

            friend class FOversubscriptionAllowedScope;

            static bool& GetIsOversubscriptionAllowedRef();

          public:
            static bool IsOversubscriptionAllowed()
            {
                return s_bIsOversubscriptionAllowed;
            }
        };

        // @class FOversubscriptionAllowedScope
        // @brief RAII scope guard for temporarily changing oversubscription permission
        class FOversubscriptionAllowedScope
        {
          public:
            FOversubscriptionAllowedScope(const FOversubscriptionAllowedScope&) = delete;
            FOversubscriptionAllowedScope& operator=(const FOversubscriptionAllowedScope&) = delete;

            explicit FOversubscriptionAllowedScope(bool bIsOversubscriptionAllowed)
                : m_bValue(FOversubscriptionTls::GetIsOversubscriptionAllowedRef()), m_bPreviousValue(m_bValue)
            {
                m_bValue = bIsOversubscriptionAllowed;
            }

            ~FOversubscriptionAllowedScope()
            {
                m_bValue = m_bPreviousValue;
            }

          private:
            bool& m_bValue;
            bool m_bPreviousValue;
        };

    } // namespace Private

    // @class FOversubscriptionScope
    // @brief RAII scope guard for incrementing/decrementing oversubscription count
    //
    // When a blocking operation occurs (like waiting for I/O), this scope can be
    // used to allow additional worker threads to be spawned to maintain throughput.
    class FOversubscriptionScope
    {
      public:
        FOversubscriptionScope(const FOversubscriptionScope&) = delete;
        FOversubscriptionScope& operator=(const FOversubscriptionScope&) = delete;

        explicit FOversubscriptionScope(bool bCondition = true)
        {
            if (bCondition)
            {
                TryIncrementOversubscription();
            }
        }

        ~FOversubscriptionScope()
        {
            if (m_bIncrementOversubscriptionEmitted)
            {
                DecrementOversubscription();
            }
        }

      private:
        void TryIncrementOversubscription();
        void DecrementOversubscription();

        bool m_bIncrementOversubscriptionEmitted = false;
        // Note: m_bCpuBeginEventEmitted was removed as it was dead code (never set)
        // UE uses it for CPU profiler trace integration. If CPU tracing is needed,
        // integrate with OloEngine's Tracy profiler using OLO_PROFILE_SCOPE instead.
    };

} // namespace OloEngine::LowLevelTasks
