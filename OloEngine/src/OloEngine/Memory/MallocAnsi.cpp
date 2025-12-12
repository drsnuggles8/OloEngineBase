// OloEngine Memory System
// Ported from Unreal Engine's HAL/MallocAnsi.cpp

#include "OloEngine/Memory/MallocAnsi.h"
#include "OloEngine/Memory/GenericPlatformMemory.h"
#include "OloEngine/Memory/AlignmentTemplates.h"

#include <cstdlib>
#include <cstring>
#include <algorithm>

#ifdef OLO_PLATFORM_WINDOWS
    #include <windows.h>
    #include <malloc.h>
    #define PLATFORM_USES__ALIGNED_MALLOC 1
#else
    #define PLATFORM_USES__ALIGNED_MALLOC 0
#endif

#if defined(OLO_PLATFORM_LINUX) || defined(OLO_PLATFORM_MACOS)
    #include <malloc.h>
    #define PLATFORM_USE_ANSI_POSIX_MALLOC 1
#else
    #define PLATFORM_USE_ANSI_POSIX_MALLOC 0
#endif

// Some platforms (e.g., Android) have memalign but not posix_memalign
#ifndef PLATFORM_USE_ANSI_MEMALIGN
    #define PLATFORM_USE_ANSI_MEMALIGN 0
#endif

// Whether the platform's ANSI malloc is thread-safe
#ifndef PLATFORM_IS_ANSI_MALLOC_THREADSAFE
    #if defined(OLO_PLATFORM_WINDOWS) || defined(OLO_PLATFORM_LINUX) || defined(OLO_PLATFORM_MACOS)
        #define PLATFORM_IS_ANSI_MALLOC_THREADSAFE 1
    #else
        #define PLATFORM_IS_ANSI_MALLOC_THREADSAFE 0
    #endif
#endif

#define MALLOC_ANSI_USES__ALIGNED_MALLOC PLATFORM_USES__ALIGNED_MALLOC

namespace OloEngine
{

void* AnsiMalloc(sizet Size, u32 Alignment)
{
#if MALLOC_ANSI_USES__ALIGNED_MALLOC
    void* Result = _aligned_malloc(Size, Alignment);
#elif PLATFORM_USE_ANSI_POSIX_MALLOC
    void* Result;
    if (OLO_UNLIKELY(posix_memalign(&Result, Alignment, Size) != 0))
    {
        Result = nullptr;
    }
#elif PLATFORM_USE_ANSI_MEMALIGN
    void* Result = memalign(Alignment, Size);
#else
    void* Ptr = std::malloc(Size + Alignment + sizeof(void*) + sizeof(sizet));
    void* Result = nullptr;
    if (Ptr)
    {
        Result = Align(reinterpret_cast<u8*>(Ptr) + sizeof(void*) + sizeof(sizet), static_cast<sizet>(Alignment));
        *reinterpret_cast<void**>(reinterpret_cast<u8*>(Result) - sizeof(void*)) = Ptr;
        *reinterpret_cast<sizet*>(reinterpret_cast<u8*>(Result) - sizeof(void*) - sizeof(sizet)) = Size;
    }
#endif

    return Result;
}

static sizet AnsiGetAllocationSize(void* Original)
{
#if MALLOC_ANSI_USES__ALIGNED_MALLOC
    return _aligned_msize(Original, 16, 0); // TODO: incorrectly assumes alignment of 16
#elif PLATFORM_USE_ANSI_POSIX_MALLOC || PLATFORM_USE_ANSI_MEMALIGN
    return malloc_usable_size(Original);
#else
    return *reinterpret_cast<sizet*>(reinterpret_cast<u8*>(Original) - sizeof(void*) - sizeof(sizet));
#endif
}

void* AnsiRealloc(void* Ptr, sizet NewSize, u32 Alignment)
{
    void* Result;

#if MALLOC_ANSI_USES__ALIGNED_MALLOC
    if (Ptr && NewSize)
    {
        Result = _aligned_realloc(Ptr, NewSize, Alignment);
    }
    else if (Ptr == nullptr)
    {
        Result = _aligned_malloc(NewSize, Alignment);
    }
    else
    {
        _aligned_free(Ptr);
        Result = nullptr;
    }
#elif PLATFORM_USE_ANSI_POSIX_MALLOC
    if (Ptr && NewSize)
    {
        sizet UsableSize = malloc_usable_size(Ptr);
        if (OLO_UNLIKELY(posix_memalign(&Result, Alignment, NewSize) != 0))
        {
            Result = nullptr;
        }
        else if (OLO_LIKELY(UsableSize))
        {
            FMemory::Memcpy(Result, Ptr, std::min(NewSize, UsableSize));
        }
        std::free(Ptr);
    }
    else if (Ptr == nullptr)
    {
        if (OLO_UNLIKELY(posix_memalign(&Result, Alignment, NewSize) != 0))
        {
            Result = nullptr;
        }
    }
    else
    {
        std::free(Ptr);
        Result = nullptr;
    }
#elif PLATFORM_USE_ANSI_MEMALIGN
    Result = reallocalign(Ptr, NewSize, Alignment);
#else
    if (Ptr && NewSize)
    {
        // Can't use realloc as it might screw with alignment.
        Result = AnsiMalloc(NewSize, Alignment);
        sizet PtrSize = AnsiGetAllocationSize(Ptr);
        FMemory::Memcpy(Result, Ptr, std::min(NewSize, PtrSize));
        AnsiFree(Ptr);
    }
    else if (Ptr == nullptr)
    {
        Result = AnsiMalloc(NewSize, Alignment);
    }
    else
    {
        std::free(*reinterpret_cast<void**>(reinterpret_cast<u8*>(Ptr) - sizeof(void*)));
        Result = nullptr;
    }
#endif

    return Result;
}

void AnsiFree(void* Ptr)
{
#if MALLOC_ANSI_USES__ALIGNED_MALLOC
    _aligned_free(Ptr);
#elif PLATFORM_USE_ANSI_POSIX_MALLOC || PLATFORM_USE_ANSI_MEMALIGN
    std::free(Ptr);
#else
    if (Ptr)
    {
        std::free(*reinterpret_cast<void**>(reinterpret_cast<u8*>(Ptr) - sizeof(void*)));
    }
#endif
}

FMallocAnsi::FMallocAnsi()
{
#ifdef OLO_PLATFORM_WINDOWS
    // Enable low fragmentation heap - http://msdn2.microsoft.com/en-US/library/aa366750.aspx
    intptr_t CrtHeapHandle = _get_heap_handle();
    ULONG EnableLFH = 2;
    HeapSetInformation(reinterpret_cast<void*>(CrtHeapHandle), HeapCompatibilityInformation, &EnableLFH, sizeof(EnableLFH));
#endif
}

void* FMallocAnsi::TryMalloc(sizet Size, u32 Alignment)
{
#if !defined(OLO_DIST)
    u64 LocalMaxSingleAlloc = MaxSingleAlloc.load(std::memory_order_relaxed);
    if (LocalMaxSingleAlloc != 0 && Size > LocalMaxSingleAlloc)
    {
        return nullptr;
    }
#endif

    Alignment = std::max(Size >= 16 ? 16u : 8u, Alignment);

    void* Result = AnsiMalloc(Size, Alignment);

    return Result;
}

void* FMallocAnsi::Malloc(sizet Size, u32 Alignment)
{
    void* Result = TryMalloc(Size, Alignment);

    if (Result == nullptr && Size)
    {
        FPlatformMemory::OnOutOfMemory(Size, Alignment);
    }

    return Result;
}

void* FMallocAnsi::TryRealloc(void* Ptr, sizet NewSize, u32 Alignment)
{
#if !defined(OLO_DIST)
    u64 LocalMaxSingleAlloc = MaxSingleAlloc.load(std::memory_order_relaxed);
    if (LocalMaxSingleAlloc != 0 && NewSize > LocalMaxSingleAlloc)
    {
        return nullptr;
    }
#endif

    Alignment = std::max(NewSize >= 16 ? 16u : 8u, Alignment);

    void* Result = AnsiRealloc(Ptr, NewSize, Alignment);

    return Result;
}

void* FMallocAnsi::Realloc(void* Ptr, sizet NewSize, u32 Alignment)
{
    void* Result = TryRealloc(Ptr, NewSize, Alignment);

    if (Result == nullptr && NewSize != 0)
    {
        FPlatformMemory::OnOutOfMemory(NewSize, Alignment);
    }

    return Result;
}

void FMallocAnsi::Free(void* Ptr)
{
    AnsiFree(Ptr);
}

bool FMallocAnsi::GetAllocationSize(void* Original, sizet& SizeOut)
{
    if (!Original)
    {
        return false;
    }

#if MALLOC_ANSI_USES__ALIGNED_MALLOC
    // _aligned_msize doesn't give reliable results for our use case
    return false;
#else
    SizeOut = AnsiGetAllocationSize(Original);
    return true;
#endif
}

bool FMallocAnsi::IsInternallyThreadSafe() const
{
#if PLATFORM_IS_ANSI_MALLOC_THREADSAFE
    return true;
#else
    return false;
#endif
}

bool FMallocAnsi::ValidateHeap()
{
#ifdef OLO_PLATFORM_WINDOWS
    i32 Result = _heapchk();
    OLO_CORE_ASSERT(Result != _HEAPBADBEGIN, "Heap validation failed: _HEAPBADBEGIN");
    OLO_CORE_ASSERT(Result != _HEAPBADNODE, "Heap validation failed: _HEAPBADNODE");
    OLO_CORE_ASSERT(Result != _HEAPBADPTR, "Heap validation failed: _HEAPBADPTR");
    OLO_CORE_ASSERT(Result != _HEAPEMPTY, "Heap validation failed: _HEAPEMPTY");
    OLO_CORE_ASSERT(Result == _HEAPOK, "Heap validation failed");
#endif
    return true;
}

} // namespace OloEngine
