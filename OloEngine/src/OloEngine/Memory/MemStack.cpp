/**
 * @file MemStack.cpp
 * @brief Implementation of FMemStackBase
 * 
 * Ported from Unreal Engine's MemStack.cpp
 */

#include "OloEngine/Memory/MemStack.h"

namespace OloEngine
{
    // ========================================================================
    // FMemStackBase Implementation
    // ========================================================================

    FMemStackBase::FMemStackBase(EPageSize InPageSize)
        : m_PageSize(InPageSize)
    {
        // By fetching the FPageAllocator singleton's address here we can guarantee
        // that its lifetime spans that of all FMemStackBase objects.
        FPageAllocator::Get();
    }

    i32 FMemStackBase::GetByteCount() const
    {
        i32 Count = 0;
        for (FTaggedMemory* Chunk = m_TopChunk; Chunk; Chunk = Chunk->Next)
        {
            if (Chunk != m_TopChunk)
            {
                Count += Chunk->DataSize;
            }
            else
            {
                Count += static_cast<i32>(m_Top - Chunk->Data());
            }
        }
        return Count;
    }

    void FMemStackBase::AllocateNewChunk(i32 MinSize)
    {
        FTaggedMemory* Chunk = nullptr;
        
        // Create new chunk
        i32 TotalSize = MinSize + static_cast<i32>(sizeof(FTaggedMemory));
        i32 AllocSize;
        
        if (m_TopChunk || TotalSize > FPageAllocator::SmallPageSize || m_PageSize == EPageSize::Large)
        {
            AllocSize = AlignArbitrary<i32>(TotalSize, FPageAllocator::PageSize);
            if (AllocSize == FPageAllocator::PageSize)
            {
                Chunk = static_cast<FTaggedMemory*>(FPageAllocator::Get().Alloc());
            }
            else
            {
                Chunk = static_cast<FTaggedMemory*>(FMemory::Malloc(AllocSize));
                // Note: UE tracks this with STAT_MemStackLargeBLock - we skip stats for now
            }
            OLO_CORE_ASSERT(AllocSize != FPageAllocator::SmallPageSize, "Unexpected allocation size!");
        }
        else
        {
            AllocSize = FPageAllocator::SmallPageSize;
            Chunk = static_cast<FTaggedMemory*>(FPageAllocator::Get().AllocSmall());
        }
        
        Chunk->DataSize = AllocSize - static_cast<i32>(sizeof(FTaggedMemory));
        Chunk->Next = m_TopChunk;
        m_TopChunk = Chunk;
        m_Top = Chunk->Data();
        m_End = m_Top + Chunk->DataSize;
    }

    void FMemStackBase::FreeChunks(FTaggedMemory* NewTopChunk)
    {
        while (m_TopChunk != NewTopChunk)
        {
            FTaggedMemory* RemoveChunk = m_TopChunk;
            m_TopChunk = m_TopChunk->Next;
            
            i32 ChunkTotalSize = RemoveChunk->DataSize + static_cast<i32>(sizeof(FTaggedMemory));
            
            if (ChunkTotalSize == FPageAllocator::PageSize)
            {
                FPageAllocator::Get().Free(RemoveChunk);
            }
            else if (ChunkTotalSize == FPageAllocator::SmallPageSize)
            {
                FPageAllocator::Get().FreeSmall(RemoveChunk);
            }
            else
            {
                // Large block allocated directly
                FMemory::Free(RemoveChunk);
            }
        }
        
        m_Top = nullptr;
        m_End = nullptr;
        
        if (m_TopChunk)
        {
            m_Top = m_TopChunk->Data();
            m_End = m_Top + m_TopChunk->DataSize;
        }
    }

    bool FMemStackBase::ContainsPointer(const void* Pointer) const
    {
        const u8* Ptr = static_cast<const u8*>(Pointer);
        
        for (const FTaggedMemory* Chunk = m_TopChunk; Chunk; Chunk = Chunk->Next)
        {
            if (Ptr >= Chunk->Data() && Ptr < Chunk->Data() + Chunk->DataSize)
            {
                return true;
            }
        }
        
        return false;
    }

} // namespace OloEngine
