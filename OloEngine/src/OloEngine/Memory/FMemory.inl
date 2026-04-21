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
    // Going through `std::atomic_ref` directly on the macro target works
    // whether the macro points at the default `Private::GMalloc` or at a
    // project-overridden concrete static allocator pointer, as long as
    // that pointer is naturally aligned (required by the macro contract).
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
