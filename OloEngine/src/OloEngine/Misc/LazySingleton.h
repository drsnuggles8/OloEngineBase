// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#pragma once

/**
 * @file LazySingleton.h
 * @brief Lazy singleton pattern that can be torn down explicitly
 * 
 * Ported from Unreal Engine's Misc/LazySingleton.h
 */

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    /**
     * @class FLazySingleton
     * @brief Base class for lazy singletons - allows inline friend declaration
     */
    class FLazySingleton
    {
    protected:
        template<class T>
        static void Construct(void* Place)
        {
            ::new (Place) T;
        }

        template<class T>
        static void Destruct(T* Instance)
        {
            Instance->~T();
        }
    };

    /**
     * @class TLazySingleton
     * @brief Lazy singleton that can be torn down explicitly
     * 
     * T must be default constructible.
     *
     * Example use case:
     *
     * @code
     * struct FFoo
     * {
     *     static FFoo& Get();
     *     static void TearDown();
     * 
     *     // If default constructor is private
     *     friend class FLazySingleton;
     * };
     * 
     * // Only include in .cpp and do *not* inline Get() and TearDown()
     * #include "OloEngine/Misc/LazySingleton.h"
     * 
     * FFoo& FFoo::Get()
     * {
     *     return TLazySingleton<FFoo>::Get();
     * }
     * 
     * void FFoo::TearDown()
     * {
     *     TLazySingleton<FFoo>::TearDown();
     * }
     * @endcode
     */
    template<class T>
    class TLazySingleton final : public FLazySingleton
    {
    public:
        /**
         * @brief Creates singleton once on first call
         * 
         * Thread-safe w.r.t. other Get() calls.
         * Do not call after TearDown(). 
         */
        static T& Get()
        {
            return GetLazy(Construct<T>).GetValue();
        }

        /**
         * @brief Destroys singleton
         * 
         * No thread must access the singleton during or after this call.
         */
        static void TearDown()
        {
            return GetLazy(nullptr).Reset();
        }

        /**
         * @brief Get or create singleton unless it's torn down
         * @return Pointer to singleton, or nullptr if torn down
         */
        static T* TryGet()
        {
            return GetLazy(Construct<T>).TryGetValue();
        }

    private:
        static TLazySingleton& GetLazy(void(*Constructor)(void*))
        {
            static TLazySingleton Singleton(Constructor);
            return Singleton;
        }

        alignas(T) unsigned char Data[sizeof(T)];
        T* Ptr;

        TLazySingleton(void(*Constructor)(void*))
        {
            if (Constructor)
            {
                Constructor(Data);
            }

            Ptr = Constructor ? reinterpret_cast<T*>(Data) : nullptr;
        }

#if !defined(DISABLE_LAZY_SINGLETON_DESTRUCTION) || !DISABLE_LAZY_SINGLETON_DESTRUCTION
        ~TLazySingleton()
        {
            Reset();
        }
#endif

        T* TryGetValue()
        {
            return Ptr;
        }

        T& GetValue()
        {
            OLO_CORE_ASSERT(Ptr, "TLazySingleton::GetValue called after TearDown");
            return *Ptr;
        }

        void Reset()
        {
            if (Ptr)
            {
                Destruct(Ptr);
                Ptr = nullptr;
            }
        }
    };

} // namespace OloEngine
