// OloEngine Memory System
// Ported from Unreal Engine's HAL/UnrealMemory.cpp

#include "OloEngine/Memory/UnrealMemory.h"
#include "OloEngine/Memory/Memory.h"
#include "OloEngine/Memory/GenericPlatformMemory.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

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
    // Simplified - would need malloc proxy infrastructure
    OLO_CORE_WARN("Purgatory proxy not implemented");
}

void FMemory::EnablePoisonTests()
{
    // Simplified - would need malloc proxy infrastructure
    OLO_CORE_WARN("Poison proxy not implemented");
}

/** Helper function called on first allocation to create and initialize GMalloc */
static int FMemory_GCreateMalloc_ThreadUnsafe()
{
    Private::GMalloc = FPlatformMemory::BaseAllocator();

    // Setup memory pools
    FPlatformMemory::SetupMemoryPools();

    if (Private::GMalloc)
    {
        Private::GMalloc->OnMallocInitialized();
    }

    return 0;
}

void FMemory::ExplicitInit(FMalloc& Allocator)
{
    OLO_CORE_ASSERT(!Private::GMalloc, "ExplicitInit called but GMalloc already exists");
    Private::GMalloc = &Allocator;
}

void FMemory::GCreateMalloc()
{
    // Thread-safe creation using static initialization
    static int ThreadSafeCreationResult = FMemory_GCreateMalloc_ThreadUnsafe();
    (void)ThreadSafeCreationResult;
}

void* FMemory::MallocExternal(sizet Count, u32 Alignment)
{
    if (!Private::GMalloc)
    {
        GCreateMalloc();
    }
    return Private::GMalloc->Malloc(Count, Alignment);
}

void* FMemory::ReallocExternal(void* Original, sizet Count, u32 Alignment)
{
    if (!Private::GMalloc)
    {
        GCreateMalloc();
    }
    return Private::GMalloc->Realloc(Original, Count, Alignment);
}

void FMemory::FreeExternal(void* Original)
{
    if (!Private::GMalloc)
    {
        GCreateMalloc();
    }
    if (Original)
    {
        Private::GMalloc->Free(Original);
    }
}

sizet FMemory::GetAllocSizeExternal(void* Original)
{
    if (!Private::GMalloc)
    {
        GCreateMalloc();
    }
    sizet Size = 0;
    return Private::GMalloc->GetAllocationSize(Original, Size) ? Size : 0;
}

void* FMemory::MallocZeroedExternal(sizet Count, u32 Alignment)
{
    if (!Private::GMalloc)
    {
        GCreateMalloc();
    }
    return Private::GMalloc->MallocZeroed(Count, Alignment);
}

sizet FMemory::QuantizeSizeExternal(sizet Count, u32 Alignment)
{
    if (!Private::GMalloc)
    {
        GCreateMalloc();
    }
    return Private::GMalloc->QuantizeSize(Count, Alignment);
}

void FMemory::Trim(bool bTrimThreadCaches)
{
    if (!Private::GMalloc)
    {
        GCreateMalloc();
    }
    Private::GMalloc->Trim(bTrimThreadCaches);
}

void FMemory::SetupTLSCachesOnCurrentThread()
{
    if (!Private::GMalloc)
    {
        GCreateMalloc();
    }
    Private::GMalloc->SetupTLSCachesOnCurrentThread();
}

void FMemory::ClearAndDisableTLSCachesOnCurrentThread()
{
    if (Private::GMalloc)
    {
        Private::GMalloc->ClearAndDisableTLSCachesOnCurrentThread();
    }
}

void FMemory::MarkTLSCachesAsUsedOnCurrentThread()
{
    if (Private::GMalloc)
    {
        Private::GMalloc->MarkTLSCachesAsUsedOnCurrentThread();
    }
}

void FMemory::MarkTLSCachesAsUnusedOnCurrentThread()
{
    if (Private::GMalloc)
    {
        Private::GMalloc->MarkTLSCachesAsUnusedOnCurrentThread();
    }
}

void FMemory::TestMemory()
{
#if !defined(OLO_DIST)
    if (!Private::GMalloc)
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
