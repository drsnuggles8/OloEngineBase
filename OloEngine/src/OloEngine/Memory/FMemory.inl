// OloEngine Memory System
// Ported from Unreal Engine's HAL/FMemory.inl

#if !defined(FMEMORY_INLINE_GMalloc)
#	error "FMEMORY_INLINE_GMalloc should be defined before including this file. Possibly FMemory.inl is included directly instead of including Memory.h"
#endif

#include "OloEngine/Memory/MemoryBase.h"
#include "OloEngine/Memory/UnrealMemory.h"

namespace OloEngine
{

struct FMemory;
struct FScopedMallocTimer;

void FMemory_FreeInline(void* Original);
sizet FMemory_GetAllocSizeInline(void* Original);


OLO_FINLINE void* FMemory_MallocInline(sizet Count, u32 Alignment)
{
    void* Ptr = nullptr;
    if (OLO_UNLIKELY(!FMEMORY_INLINE_GMalloc))
    {
        Ptr = FMemory::MallocExternal(Count, Alignment);
    }
    else
    {
        DoGamethreadHook(0);
        FScopedMallocTimer Timer(0);
        Ptr = FMEMORY_INLINE_GMalloc->Malloc(Count, Alignment);
    }

    return Ptr;
}

OLO_FINLINE void* FMemory_ReallocInline(void* Original, sizet Count, u32 Alignment)
{
    void* Ptr = nullptr;
    if (OLO_UNLIKELY(!FMEMORY_INLINE_GMalloc))
    {
        Ptr = FMemory::ReallocExternal(Original, Count, Alignment);
    }
    else
    {
        DoGamethreadHook(1);
        FScopedMallocTimer Timer(1);
        Ptr = FMEMORY_INLINE_GMalloc->Realloc(Original, Count, Alignment);
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

    if (OLO_UNLIKELY(!FMEMORY_INLINE_GMalloc))
    {
        FMemory::FreeExternal(Original);
        return;
    }
    DoGamethreadHook(2);
    FScopedMallocTimer Timer(2);
    FMEMORY_INLINE_GMalloc->Free(Original);
}

OLO_FINLINE sizet FMemory_GetAllocSizeInline(void* Original)
{
    sizet Result = 0;
    if (OLO_UNLIKELY(!FMEMORY_INLINE_GMalloc))
    {
        Result = FMemory::GetAllocSizeExternal(Original);
    }
    else
    {
        sizet Size = 0;
        const bool bGotSize = FMEMORY_INLINE_GMalloc->GetAllocationSize(Original, Size);
        Result = bGotSize ? Size : 0;
    }

    return Result;
}

OLO_FINLINE void* FMemory_MallocZeroedInline(sizet Count, u32 Alignment)
{
    void* Ptr = nullptr;
    if (OLO_UNLIKELY(!FMEMORY_INLINE_GMalloc))
    {
        Ptr = FMemory::MallocZeroedExternal(Count, Alignment);
    }
    else
    {
        DoGamethreadHook(0);
        FScopedMallocTimer Timer(0);
        Ptr = FMEMORY_INLINE_GMalloc->MallocZeroed(Count, Alignment);
    }

    return Ptr;
}

OLO_FINLINE sizet FMemory_QuantizeSizeInline(sizet Count, u32 Alignment)
{
    sizet Result = 0;
    if (OLO_UNLIKELY(!FMEMORY_INLINE_GMalloc))
    {
        Result = Count;
    }
    else
    {
        Result = FMEMORY_INLINE_GMalloc->QuantizeSize(Count, Alignment);
    }

    return Result;
}

} // namespace OloEngine
