// OloEngine Memory System
// Ported from Unreal Engine's HAL/UnrealMemory.h

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/GenericPlatformMemory.h"
#include "OloEngine/Memory/MemoryBase.h"

#ifndef OLO_USE_VERYLARGEPAGEALLOCATOR
#define OLO_USE_VERYLARGEPAGEALLOCATOR 0
#endif

#ifndef OLO_ALLOW_OSMEMORYLOCKFREE
#define OLO_ALLOW_OSMEMORYLOCKFREE 0
#endif

// STATS is typically defined in build configuration
// In OloEngine, we enable stats in non-dist builds
#ifndef STATS
    #if !defined(OLO_DIST)
        #define STATS 1
    #else
        #define STATS 0
    #endif
#endif

namespace OloEngine
{

// Sizes.

#if STATS
#define MALLOC_GT_HOOKS 1
#else
#define MALLOC_GT_HOOKS 0
#endif

#if MALLOC_GT_HOOKS
    void DoGamethreadHook(i32 Index);
#else
    OLO_FINLINE void DoGamethreadHook(i32 Index)
    {
        (void)Index;
    }
#endif

#define TIME_MALLOC (0)

#if TIME_MALLOC

struct FScopedMallocTimer
{
    static u64 GTotalCycles[4];
    static u64 GTotalCount[4];
    static u64 GTotalMisses[4];

    i32 Index;
    u64 Cycles;

    inline FScopedMallocTimer(i32 InIndex)
        : Index(InIndex)
        , Cycles(0) // Would use FPlatformTime::Cycles64()
    {
    }
    inline ~FScopedMallocTimer()
    {
    }
    static OLO_FINLINE void Miss(i32 InIndex)
    {
        (void)InIndex;
    }
};

#else

struct FScopedMallocTimer
{
    OLO_FINLINE FScopedMallocTimer(i32 InIndex)
    {
        (void)InIndex;
    }
    OLO_FINLINE ~FScopedMallocTimer()
    {
    }
    OLO_FINLINE void Hit(i32 InIndex)
    {
        (void)InIndex;
    }
};

#endif

/*-----------------------------------------------------------------------------
    FMemory.
-----------------------------------------------------------------------------*/

struct FMemory
{
    /** Some allocators can be given hints to treat allocations differently depending on how the memory is used, it's lifetime etc. */
    enum AllocationHints
    {
        None = -1,
        Default,
        Temporary,
        SmallPool,

        Max
    };

    /** @name Memory functions (wrapper for FPlatformMemory) */

    static OLO_FINLINE void* Memmove(void* Dest, const void* Src, sizet Count)
    {
        return FPlatformMemory::Memmove(Dest, Src, Count);
    }

    static OLO_FINLINE i32 Memcmp(const void* Buf1, const void* Buf2, sizet Count)
    {
        return FPlatformMemory::Memcmp(Buf1, Buf2, Count);
    }

    static OLO_FINLINE void* Memset(void* Dest, u8 Char, sizet Count)
    {
        return FPlatformMemory::Memset(Dest, Char, Count);
    }

    template<class T>
    static inline void Memset(T& Src, u8 ValueToSet)
    {
        static_assert(!std::is_pointer_v<T>, "For pointers use the three parameters function");
        Memset(&Src, ValueToSet, sizeof(T));
    }

    static OLO_FINLINE void* Memzero(void* Dest, sizet Count)
    {
        return FPlatformMemory::Memzero(Dest, Count);
    }

    /** Returns true if memory is zeroes, false otherwise. */
    static inline bool MemIsZero(const void* Ptr, sizet Count)
    {
        // first pass implementation
        u8* Start = (u8*)Ptr;
        u8* End = Start + Count;
        while (Start < End)
        {
            if ((*Start++) != 0)
            {
                return false;
            }
        }

        return true;
    }

    template<class T>
    static inline void Memzero(T& Src)
    {
        static_assert(!std::is_pointer_v<T>, "For pointers use the two parameters function");
        Memzero(&Src, sizeof(T));
    }

    static OLO_FINLINE void* Memcpy(void* Dest, const void* Src, sizet Count)
    {
        return FPlatformMemory::Memcpy(Dest, Src, Count);
    }

    template<class T>
    static inline void Memcpy(T& Dest, const T& Src)
    {
        static_assert(!std::is_pointer_v<T>, "For pointers use the three parameters function");
        Memcpy(&Dest, &Src, sizeof(T));
    }

    static OLO_FINLINE void* BigBlockMemcpy(void* Dest, const void* Src, sizet Count)
    {
        return FPlatformMemory::BigBlockMemcpy(Dest, Src, Count);
    }

    static OLO_FINLINE void* StreamingMemcpy(void* Dest, const void* Src, sizet Count)
    {
        return FPlatformMemory::StreamingMemcpy(Dest, Src, Count);
    }

    static OLO_FINLINE void* ParallelMemcpy(void* Dest, const void* Src, sizet Count, EMemcpyCachePolicy Policy = EMemcpyCachePolicy::StoreCached)
    {
        return FPlatformMemory::ParallelMemcpy(Dest, Src, Count, Policy);
    }

    static OLO_FINLINE void Memswap(void* Ptr1, void* Ptr2, sizet Size)
    {
        FPlatformMemory::Memswap(Ptr1, Ptr2, Size);
    }

    //
    // C style memory allocation stubs that fall back to C runtime
    //
    [[nodiscard]] static inline void* SystemMalloc(sizet Size)
    {
        return ::malloc(Size);
    }

    static inline void SystemFree(void* Ptr)
    {
        ::free(Ptr);
    }

    //
    // C style memory allocation stubs.
    //

    [[nodiscard]] static OLO_NOINLINE void* Malloc(sizet Count, u32 Alignment = DEFAULT_ALIGNMENT);
    [[nodiscard]] static OLO_NOINLINE void* Realloc(void* Original, sizet Count, u32 Alignment = DEFAULT_ALIGNMENT);
    static OLO_NOINLINE void Free(void* Original);
    static OLO_NOINLINE sizet GetAllocSize(void* Original);

    [[nodiscard]] static OLO_NOINLINE void* MallocZeroed(sizet Count, u32 Alignment = DEFAULT_ALIGNMENT);

    /**
     * For some allocators this will return the actual size that should be requested to eliminate
     * internal fragmentation. The return value will always be >= Count. This can be used to grow
     * and shrink containers to optimal sizes.
     * This call is always fast and threadsafe with no locking.
     */
    static OLO_NOINLINE sizet QuantizeSize(sizet Count, u32 Alignment = DEFAULT_ALIGNMENT);

    /**
     * Releases as much memory as possible. Must be called from the main thread.
     */
    static void Trim(bool bTrimThreadCaches = true);

    /**
     * Set up TLS caches on the current thread. These are the threads that we can trim.
     */
    static void SetupTLSCachesOnCurrentThread();

    /**
     * Clears the TLS caches on the current thread and disables any future caching.
     */
    static void ClearAndDisableTLSCachesOnCurrentThread();

    /**
     * Mark TLS caches for the current thread as used. Thread has woken up to do some processing and needs its TLS caches back.
     */
    static void MarkTLSCachesAsUsedOnCurrentThread();

    /**
     * Mark TLS caches for current thread as unused. Typically before going to sleep. These are the threads that we can trim without waking them up.
     */
    static void MarkTLSCachesAsUnusedOnCurrentThread();

    /**
     * A helper function that will perform a series of random heap allocations to test
     * the internal validity of the heap. Note, this function will "leak" memory, but another call
     * will clean up previously allocated blocks before returning. This will help to A/B testing
     * where you call it in a good state, do something to corrupt memory, then call this again
     * and hopefully freeing some pointers will trigger a crash.
     */
    static void TestMemory();

    /**
     * Called once main is started and we have -purgatorymallocproxy.
     * This uses the purgatory malloc proxy to check if things are writing to stale pointers.
     */
    static void EnablePurgatoryTests();

    /**
     * Called once main is started and we have -poisonmallocproxy.
     */
    static void EnablePoisonTests();

    /**
     * Set global allocator instead of creating it lazily on first allocation.
     * Must only be called once and only if lazy init is disabled via a macro.
     */
    static void ExplicitInit(FMalloc& Allocator);

    // These versions are called either at startup or in the event of a crash
    [[nodiscard]] static void* MallocExternal(sizet Count, u32 Alignment = DEFAULT_ALIGNMENT);
    [[nodiscard]] static void* ReallocExternal(void* Original, sizet Count, u32 Alignment = DEFAULT_ALIGNMENT);
    static void FreeExternal(void* Original);
    static sizet GetAllocSizeExternal(void* Original);
    [[nodiscard]] static void* MallocZeroedExternal(sizet Count, u32 Alignment = DEFAULT_ALIGNMENT);
    static sizet QuantizeSizeExternal(sizet Count, u32 Alignment = DEFAULT_ALIGNMENT);

private:
    static void GCreateMalloc();
};

} // namespace OloEngine
