// OloEngine Memory System
// Ported from Unreal Engine's HAL/UnrealMemory.cpp

#include "OloEngine/Memory/UnrealMemory.h"
#include "OloEngine/Memory/Memory.h"
#include "OloEngine/Memory/GenericPlatformMemory.h"
#include "OloEngine/HAL/MallocPoisonProxy.h"
#include "OloEngine/HAL/MallocPurgatoryProxy.h"
#include "OloEngine/HAL/MallocVerifyProxy.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional> // std::function used by GGameThreadMallocHook under MALLOC_GT_HOOKS
#include <vector>

namespace OloEngine
{

    /*-----------------------------------------------------------------------------
        GMalloc definition
    -----------------------------------------------------------------------------*/

    namespace Private
    {
        FMalloc* GMalloc = nullptr;
    }

    // Backwards compatibility reference to the namespaced GMalloc
    FMalloc* const& GMalloc = Private::GMalloc;

    // Memory allocator pointer location when PLATFORM_USES_FIXED_GMalloc_CLASS is true
    FMalloc** GFixedMallocLocationPtr = nullptr;

    /*-----------------------------------------------------------------------------
        Memory functions.
    -----------------------------------------------------------------------------*/

#if MALLOC_GT_HOOKS

    // This code is used to find memory allocations, you set up the lambda in the section of the code you are interested in and add a breakpoint to your lambda to see who is allocating memory

    std::function<void(i32)>* GGameThreadMallocHook = nullptr;

    void DoGamethreadHook(i32 Index)
    {
        if (GGameThreadMallocHook)
        {
            (*GGameThreadMallocHook)(Index);
        }
    }
#endif

#if TIME_MALLOC

    u64 FScopedMallocTimer::GTotalCycles[4] = { 0 };
    u64 FScopedMallocTimer::GTotalCount[4] = { 0 };
    u64 FScopedMallocTimer::GTotalMisses[4] = { 0 };

    void FScopedMallocTimer::Spew()
    {
        // Simplified version - would need platform time functions for full implementation
    }

#endif

    void FMemory::EnablePurgatoryTests()
    {
#if OLO_MALLOC_PURGATORY
        static bool bOnce = false;
        if (bOnce)
        {
            OLO_CORE_ERROR("Purgatory proxy was already turned on.");
            return;
        }
        bOnce = true;

        // Atomically swap GMalloc to point to the purgatory proxy
        while (true)
        {
            FMalloc* LocalGMalloc = Private::AtomicLoadGMalloc();
            FMalloc* Proxy = new FMallocPurgatoryProxy(LocalGMalloc);

            // Atomic compare-exchange to swap in the proxy
            FMalloc* Expected = LocalGMalloc;
            if (Private::AtomicCompareExchangeGMalloc(Expected, Proxy))
            {
                OLO_CORE_INFO("Purgatory proxy is now on - use-after-free detection enabled.");
                return;
            }
            delete Proxy;
        }
#else
        OLO_CORE_WARN("Purgatory proxy not compiled in (OLO_MALLOC_PURGATORY=0)");
#endif
    }

    void FMemory::EnablePoisonTests()
    {
        static bool bOnce = false;
        if (bOnce)
        {
            OLO_CORE_ERROR("Poison proxy was already turned on.");
            return;
        }
        bOnce = true;

        // Atomically swap GMalloc to point to the poison proxy
        while (true)
        {
            FMalloc* LocalGMalloc = Private::AtomicLoadGMalloc();
            FMalloc* Proxy = new FMallocPoisonProxy(LocalGMalloc);

            // Atomic compare-exchange to swap in the proxy
            FMalloc* Expected = LocalGMalloc;
            if (Private::AtomicCompareExchangeGMalloc(Expected, Proxy))
            {
                OLO_CORE_INFO("Poison proxy is now on - memory poisoning enabled (0xCD=new, 0xDD=freed).");
                return;
            }
            delete Proxy;
        }
    }

    // Helper function called on first allocation to create and initialize GMalloc
    static int FMemory_GCreateMalloc_ThreadUnsafe()
    {
        // Construct and fully initialise the allocator on a local pointer
        // FIRST, and only then publish it via an atomic release-store.
        // Publishing the pointer before `SetupMemoryPools` /
        // `OnMallocInitialized` would let a concurrent thread acquire-load
        // a non-null `GMalloc` and route an allocation through a
        // half-initialised allocator — the very race the atomic load was
        // added to close. The release-store pairs with the acquire-loads
        // in `AtomicLoadGMalloc` / `FMemory_AtomicLoadInlineGMalloc`,
        // guaranteeing the observer sees a fully-constructed allocator.
        FMalloc* const Allocator = FPlatformMemory::BaseAllocator();

        // Setup memory pools (safe to run against the local pointer; does
        // not depend on `Private::GMalloc` being visible to other threads).
        FPlatformMemory::SetupMemoryPools();

        if (Allocator)
        {
            Allocator->OnMallocInitialized();
        }

        // Publish only after the allocator is fully initialised.
        Private::AtomicStoreGMalloc(Allocator);

        return 0;
    }

    void FMemory::ExplicitInit(FMalloc& Allocator)
    {
        OLO_CORE_ASSERT(!Private::AtomicLoadGMalloc(), "ExplicitInit called but GMalloc already exists");
        Private::AtomicStoreGMalloc(&Allocator);
    }

    void FMemory::GCreateMalloc()
    {
        // Thread-safe creation using static initialization
        static int ThreadSafeCreationResult = FMemory_GCreateMalloc_ThreadUnsafe();
        (void)ThreadSafeCreationResult;
    }

    void* FMemory::MallocExternal(sizet Count, u32 Alignment)
    {
        FMalloc* Allocator = Private::AtomicLoadGMalloc();
        if (!Allocator)
        {
            GCreateMalloc();
            Allocator = Private::AtomicLoadGMalloc();
        }
        return Allocator->Malloc(Count, Alignment);
    }

    void* FMemory::ReallocExternal(void* Original, sizet Count, u32 Alignment)
    {
        FMalloc* Allocator = Private::AtomicLoadGMalloc();
        if (!Allocator)
        {
            GCreateMalloc();
            Allocator = Private::AtomicLoadGMalloc();
        }
        return Allocator->Realloc(Original, Count, Alignment);
    }

    void FMemory::FreeExternal(void* Original)
    {
        FMalloc* Allocator = Private::AtomicLoadGMalloc();
        if (!Allocator)
        {
            GCreateMalloc();
            Allocator = Private::AtomicLoadGMalloc();
        }
        if (Original)
        {
            Allocator->Free(Original);
        }
    }

    sizet FMemory::GetAllocSizeExternal(void* Original)
    {
        FMalloc* Allocator = Private::AtomicLoadGMalloc();
        if (!Allocator)
        {
            GCreateMalloc();
            Allocator = Private::AtomicLoadGMalloc();
        }
        sizet Size = 0;
        return Allocator->GetAllocationSize(Original, Size) ? Size : 0;
    }

    void* FMemory::MallocZeroedExternal(sizet Count, u32 Alignment)
    {
        FMalloc* Allocator = Private::AtomicLoadGMalloc();
        if (!Allocator)
        {
            GCreateMalloc();
            Allocator = Private::AtomicLoadGMalloc();
        }
        return Allocator->MallocZeroed(Count, Alignment);
    }

    sizet FMemory::QuantizeSizeExternal(sizet Count, u32 Alignment)
    {
        FMalloc* Allocator = Private::AtomicLoadGMalloc();
        if (!Allocator)
        {
            GCreateMalloc();
            Allocator = Private::AtomicLoadGMalloc();
        }
        return Allocator->QuantizeSize(Count, Alignment);
    }

    void FMemory::Trim(bool bTrimThreadCaches)
    {
        FMalloc* Allocator = Private::AtomicLoadGMalloc();
        if (!Allocator)
        {
            GCreateMalloc();
            Allocator = Private::AtomicLoadGMalloc();
        }
        Allocator->Trim(bTrimThreadCaches);
    }

    void FMemory::SetupTLSCachesOnCurrentThread()
    {
        FMalloc* Allocator = Private::AtomicLoadGMalloc();
        if (!Allocator)
        {
            GCreateMalloc();
            Allocator = Private::AtomicLoadGMalloc();
        }
        Allocator->SetupTLSCachesOnCurrentThread();
    }

    void FMemory::ClearAndDisableTLSCachesOnCurrentThread()
    {
        if (FMalloc* Allocator = Private::AtomicLoadGMalloc())
        {
            Allocator->ClearAndDisableTLSCachesOnCurrentThread();
        }
    }

    void FMemory::MarkTLSCachesAsUsedOnCurrentThread()
    {
        if (FMalloc* Allocator = Private::AtomicLoadGMalloc())
        {
            Allocator->MarkTLSCachesAsUsedOnCurrentThread();
        }
    }

    void FMemory::MarkTLSCachesAsUnusedOnCurrentThread()
    {
        if (FMalloc* Allocator = Private::AtomicLoadGMalloc())
        {
            Allocator->MarkTLSCachesAsUnusedOnCurrentThread();
        }
    }

    void FMemory::TestMemory()
    {
#if !defined(OLO_DIST)
        if (!Private::AtomicLoadGMalloc())
        {
            GCreateMalloc();
        }

        // Track the pointers to free next call to the function
        static std::vector<void*> LeakedPointers;
        std::vector<void*> SavedLeakedPointers = LeakedPointers;

        // Note that at the worst case, there will be NumFreedAllocations + 2 * NumLeakedAllocations allocations alive
        static const int NumFreedAllocations = 1000;
        static const int NumLeakedAllocations = 100;
        static const int MaxAllocationSize = 128 * 1024;

        std::vector<void*> FreedPointers;
        // Allocate pointers that will be freed later
        for (int Index = 0; Index < NumFreedAllocations; Index++)
        {
            FreedPointers.push_back(FMemory::Malloc(std::rand() % MaxAllocationSize));
        }

        // Allocate pointers that will be leaked until the next call
        LeakedPointers.clear();
        for (int Index = 0; Index < NumLeakedAllocations; Index++)
        {
            LeakedPointers.push_back(FMemory::Malloc(std::rand() % MaxAllocationSize));
        }

        // Free the leaked pointers from _last_ call to this function
        for (void* Ptr : SavedLeakedPointers)
        {
            FMemory::Free(Ptr);
        }

        // Free the non-leaked pointers from this call to this function
        for (void* Ptr : FreedPointers)
        {
            FMemory::Free(Ptr);
        }
#endif
    }

    void* FMemory::Malloc(sizet Count, u32 Alignment)
    {
        return FMemory_MallocInline(Count, Alignment);
    }

    void* FMemory::Realloc(void* Original, sizet Count, u32 Alignment)
    {
        return FMemory_ReallocInline(Original, Count, Alignment);
    }

    void FMemory::Free(void* Original)
    {
        return FMemory_FreeInline(Original);
    }

    sizet FMemory::GetAllocSize(void* Original)
    {
        return FMemory_GetAllocSizeInline(Original);
    }

    void* FMemory::MallocZeroed(sizet Count, u32 Alignment)
    {
        return FMemory_MallocZeroedInline(Count, Alignment);
    }

    sizet FMemory::QuantizeSize(sizet Count, u32 Alignment)
    {
        return FMemory_QuantizeSizeInline(Count, Alignment);
    }

    void* FUseSystemMallocForNew::operator new(sizet Size)
    {
        return FMemory::SystemMalloc(Size);
    }

    void FUseSystemMallocForNew::operator delete(void* Ptr)
    {
        FMemory::SystemFree(Ptr);
    }

    void* FUseSystemMallocForNew::operator new[](sizet Size)
    {
        return FMemory::SystemMalloc(Size);
    }

    void FUseSystemMallocForNew::operator delete[](void* Ptr)
    {
        FMemory::SystemFree(Ptr);
    }

} // namespace OloEngine
