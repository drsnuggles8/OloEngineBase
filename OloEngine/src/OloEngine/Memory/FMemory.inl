// OloEngine Memory System
// Ported from Unreal Engine's HAL/FMemory.inl

#if !defined(FMEMORY_INLINE_GMalloc)
#error "FMEMORY_INLINE_GMalloc should be defined before including this file. Possibly FMemory.inl is included directly instead of including Memory.h"
#endif

#include "OloEngine/Memory/MemoryBase.h"
#include "OloEngine/Memory/UnrealMemory.h"

#include <atomic>

namespace OloEngine
{

    struct FMemory;
    struct FScopedMallocTimer;

    void FMemory_FreeInline(void* Original);
    sizet FMemory_GetAllocSizeInline(void* Original);

    // @brief Acquire-load of the inline GMalloc pointer.
    //
    // See `Private::AtomicLoadGMalloc` for the rationale — naked reads of
    // the allocator pointer race with lazy initialisation under TSan.
    //
    // Contract of `FMEMORY_INLINE_GMalloc`: the macro MUST expand to an
    // lvalue naming an addressable `FMalloc*` storage object — typically
    // `Private::GMalloc` (the default) or a project-supplied static
    // `FMalloc*` variable. It MUST NOT expand to a concrete allocator
    // instance, temporary, or member-of-class-template reference that
    // cannot have its address taken, because `std::atomic_ref<FMalloc*>`
    // requires a real, naturally-aligned pointer object to bind to.
    //
    // Example of a valid override in Memory.h:
    //     // extern FMalloc* gMyEngineMalloc; // addressable pointer storage
    //     // #define FMEMORY_INLINE_GMalloc gMyEngineMalloc
    //
    // An expression like `FMallocBinned2::MallocBinned2` only works if it
    // names a static `FMalloc*` member variable; using a concrete
    // allocator object or an rvalue there is ill-formed.
    OLO_FINLINE FMalloc* FMemory_AtomicLoadInlineGMalloc() noexcept
    {
        return std::atomic_ref<FMalloc*>(FMEMORY_INLINE_GMalloc).load(std::memory_order_acquire);
    }

    OLO_FINLINE void* FMemory_MallocInline(sizet Count, u32 Alignment)
    {
        void* Ptr = nullptr;
        FMalloc* const Allocator = FMemory_AtomicLoadInlineGMalloc();
        if (OLO_UNLIKELY(!Allocator))
        {
            Ptr = FMemory::MallocExternal(Count, Alignment);
        }
        else
        {
            DoGamethreadHook(0);
            FScopedMallocTimer Timer(0);
            Ptr = Allocator->Malloc(Count, Alignment);
        }

        return Ptr;
    }

    OLO_FINLINE void* FMemory_ReallocInline(void* Original, sizet Count, u32 Alignment)
    {
        void* Ptr = nullptr;
        FMalloc* const Allocator = FMemory_AtomicLoadInlineGMalloc();
        if (OLO_UNLIKELY(!Allocator))
        {
            Ptr = FMemory::ReallocExternal(Original, Count, Alignment);
        }
        else
        {
            DoGamethreadHook(1);
            FScopedMallocTimer Timer(1);
            Ptr = Allocator->Realloc(Original, Count, Alignment);
        }

        return Ptr;
    }

    OLO_FINLINE void FMemory_FreeInline(void* Original)
    {
        if (!Original)
        {
            FScopedMallocTimer Timer(3);
            return;
        }

        FMalloc* const Allocator = FMemory_AtomicLoadInlineGMalloc();
        if (OLO_UNLIKELY(!Allocator))
        {
            FMemory::FreeExternal(Original);
            return;
        }
        DoGamethreadHook(2);
        FScopedMallocTimer Timer(2);
        Allocator->Free(Original);
    }

    OLO_FINLINE sizet FMemory_GetAllocSizeInline(void* Original)
    {
        sizet Result = 0;
        FMalloc* const Allocator = FMemory_AtomicLoadInlineGMalloc();
        if (OLO_UNLIKELY(!Allocator))
        {
            Result = FMemory::GetAllocSizeExternal(Original);
        }
        else
        {
            sizet Size = 0;
            const bool bGotSize = Allocator->GetAllocationSize(Original, Size);
            Result = bGotSize ? Size : 0;
        }

        return Result;
    }

    OLO_FINLINE void* FMemory_MallocZeroedInline(sizet Count, u32 Alignment)
    {
        void* Ptr = nullptr;
        FMalloc* const Allocator = FMemory_AtomicLoadInlineGMalloc();
        if (OLO_UNLIKELY(!Allocator))
        {
            Ptr = FMemory::MallocZeroedExternal(Count, Alignment);
        }
        else
        {
            DoGamethreadHook(0);
            FScopedMallocTimer Timer(0);
            Ptr = Allocator->MallocZeroed(Count, Alignment);
        }

        return Ptr;
    }

    OLO_FINLINE sizet FMemory_QuantizeSizeInline(sizet Count, u32 Alignment)
    {
        sizet Result = 0;
        FMalloc* const Allocator = FMemory_AtomicLoadInlineGMalloc();
        if (OLO_UNLIKELY(!Allocator))
        {
            Result = Count;
        }
        else
        {
            Result = Allocator->QuantizeSize(Count, Alignment);
        }

        return Result;
    }

} // namespace OloEngine
