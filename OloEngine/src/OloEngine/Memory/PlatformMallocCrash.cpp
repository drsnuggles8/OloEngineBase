// PlatformMallocCrash.cpp - Emergency allocator for crash handling
// Ported 1:1 from UE5.7 FPlatformMallocCrash

#include "OloEngine/Memory/PlatformMallocCrash.h"
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <new>

namespace OloEngine
{
    // Global state tracking - simple static bool to avoid TLS issues
    static std::atomic<bool> GIsMallocCrashActive{ false };

    //------------------------------------------------------------------------------
    // FMallocCrashPool Implementation
    //------------------------------------------------------------------------------

    void FMallocCrashPool::Initialize(const FPoolDesc& PoolDesc, FGenericPlatformMallocCrash& Outer)
    {
        AllocationSize = PoolDesc.Size;
        MaxNumAllocations = PoolDesc.NumAllocs;
        AllocationCount = PoolDesc.NumAllocs;

        // Allocate bookkeeping array from bookkeeping pool
        const sizet BookkeepingSize = sizeof(FPtrInfo) * AllocationCount;
        Allocations = reinterpret_cast<FPtrInfo*>(Outer.AllocateFromBookkeeping(BookkeepingSize));

        // Allocate pool memory from small pool
        const sizet PoolMemorySize = static_cast<sizet>(AllocationSize) * MaxNumAllocations;
        AllocBase = Outer.AllocateFromSmallPool(PoolMemorySize);

        AllocatedMemory = static_cast<u32>(BookkeepingSize + PoolMemorySize);

        // Initialize FPtrInfo entries with pointers into pool
        if (Allocations && AllocBase)
        {
            for (u32 i = 0; i < AllocationCount; ++i)
            {
                Allocations[i].Size = 0; // Mark as free
                Allocations[i].Ptr = AllocBase + (static_cast<sizet>(i) * AllocationSize);
            }
        }

        // Tag pool memory as uninitialized
        if (AllocBase)
        {
            std::memset(AllocBase, MEM_WIPETAG, PoolMemorySize);
        }

        NumUsed = 0;
        MaxUsedIndex = 0;
        MaxNumUsed = 0;
        TotalNumUsed = 0;
    }

    u8* FMallocCrashPool::AllocateFromPool(u32 InAllocationSize)
    {
        // Pool is exhausted
        if (NumUsed >= MaxNumAllocations)
        {
            return nullptr;
        }

        // Linear search for a free slot (Size == 0)
        // Start from MaxUsedIndex to avoid rescanning freed slots unnecessarily
        for (u32 i = 0; i < AllocationCount; ++i)
        {
            if (Allocations[i].Size == 0)
            {
                // Found a free slot
                Allocations[i].Size = InAllocationSize;
                NumUsed++;
                TotalNumUsed++;

                // Update tracking
                if (i > MaxUsedIndex)
                {
                    MaxUsedIndex = i;
                }
                if (NumUsed > MaxNumUsed)
                {
                    MaxNumUsed = NumUsed;
                }

                // Tag allocated memory
                u8* Result = Allocations[i].Ptr;
                std::memset(Result, MEM_TAG, AllocationSize);

                return Result;
            }
        }

        return nullptr;
    }

    bool FMallocCrashPool::TryFreeFromPool(u8* Ptr)
    {
        if (!ContainsPointer(Ptr))
        {
            return false;
        }

        // O(1) free using pointer arithmetic
        const sizet Offset = static_cast<sizet>(Ptr - AllocBase);
        const u32 Index = static_cast<u32>(Offset / AllocationSize);

        // Sanity check
        if (Index >= AllocationCount)
        {
            return false;
        }

        // Verify pointer alignment
        if (Allocations[Index].Ptr != Ptr)
        {
            return false;
        }

        // Mark as free
        if (Allocations[Index].Size != 0)
        {
            Allocations[Index].Size = 0;
            NumUsed--;

            // Wipe freed memory
            std::memset(Ptr, MEM_WIPETAG, AllocationSize);
        }

        return true;
    }

    u64 FMallocCrashPool::GetAllocationSize(u8* Ptr) const
    {
        if (!ContainsPointer(Ptr))
        {
            return 0;
        }

        // O(1) lookup using pointer arithmetic
        const sizet Offset = static_cast<sizet>(Ptr - AllocBase);
        const u32 Index = static_cast<u32>(Offset / AllocationSize);

        if (Index >= AllocationCount)
        {
            return 0;
        }

        return Allocations[Index].Size;
    }

    bool FMallocCrashPool::ContainsPointer(u8* Ptr) const
    {
        if (!AllocBase || !Ptr)
        {
            return false;
        }
        const u8* PoolEnd = AllocBase + (static_cast<sizet>(AllocationSize) * MaxNumAllocations);
        return Ptr >= AllocBase && Ptr < PoolEnd;
    }

    //------------------------------------------------------------------------------
    // FGenericPlatformMallocCrash Implementation
    //------------------------------------------------------------------------------

    u32 FGenericPlatformMallocCrash::CalculateSmallPoolTotalSize()
    {
        u32 TotalSize = 0;
        for (int i = 0; i < NUM_POOLS; ++i)
        {
            TotalSize += PoolDescs[i].Size * PoolDescs[i].NumAllocs;
        }
        return TotalSize;
    }

    u32 FGenericPlatformMallocCrash::CalculateBookkeepingPoolTotalSize()
    {
        u32 TotalSize = 0;
        for (int i = 0; i < NUM_POOLS; ++i)
        {
            TotalSize += static_cast<u32>(sizeof(FPtrInfo)) * PoolDescs[i].NumAllocs;
        }
        return TotalSize;
    }

    FGenericPlatformMallocCrash& FGenericPlatformMallocCrash::Get(void* /*MainMalloc*/)
    {
        // Static singleton - created once, never destroyed
        static FGenericPlatformMallocCrash CrashMalloc;
        return CrashMalloc;
    }

    bool FGenericPlatformMallocCrash::IsActive()
    {
        return GIsMallocCrashActive.load(std::memory_order_acquire);
    }

    FGenericPlatformMallocCrash::FGenericPlatformMallocCrash()
    {
        // Lazy initialization - pools are allocated when first needed
    }

    FGenericPlatformMallocCrash::~FGenericPlatformMallocCrash()
    {
        // Never free - crash malloc lives for entire process lifetime
    }

    u8* FGenericPlatformMallocCrash::AllocateFromSmallPool(sizet Size)
    {
        if (!SmallMemoryPool)
        {
            return nullptr;
        }

        // Align to 16 bytes
        u32 AlignedOffset = (SmallMemoryPoolOffset + REQUIRED_ALIGNMENT - 1) & ~(REQUIRED_ALIGNMENT - 1);
        if (AlignedOffset + Size > SmallMemoryPoolSize)
        {
            return nullptr;
        }

        u8* Result = SmallMemoryPool + AlignedOffset;
        SmallMemoryPoolOffset = static_cast<u32>(AlignedOffset + Size);
        return Result;
    }

    u8* FGenericPlatformMallocCrash::AllocateFromBookkeeping(sizet Size)
    {
        if (!BookkeepingPool)
        {
            return nullptr;
        }

        // Align to 16 bytes
        u32 AlignedOffset = (BookkeepingPoolOffset + REQUIRED_ALIGNMENT - 1) & ~(REQUIRED_ALIGNMENT - 1);
        if (AlignedOffset + Size > BookkeepingPoolSize)
        {
            return nullptr;
        }

        u8* Result = BookkeepingPool + AlignedOffset;
        BookkeepingPoolOffset = static_cast<u32>(AlignedOffset + Size);
        return Result;
    }

    void FGenericPlatformMallocCrash::InitializeSmallPools()
    {
        // Construct each pool in-place and initialize
        for (int i = 0; i < NUM_POOLS; ++i)
        {
            FMallocCrashPool* Pool = new (PoolStorage[i].GetTypedPtr()) FMallocCrashPool();
            Pool->Initialize(PoolDescs[i], *this);
        }
    }

    void FGenericPlatformMallocCrash::Initialize()
    {
        if (bIsInitialized)
        {
            return;
        }

        // Calculate pool sizes
        SmallMemoryPoolSize = CalculateSmallPoolTotalSize();
        BookkeepingPoolSize = CalculateBookkeepingPoolTotalSize();

        // Allocate all pools using platform malloc (bypasses GMalloc)
        LargeMemoryPool = static_cast<u8*>(std::malloc(LARGE_MEMORYPOOL_SIZE));
        SmallMemoryPool = static_cast<u8*>(std::malloc(SmallMemoryPoolSize));
        BookkeepingPool = static_cast<u8*>(std::malloc(BookkeepingPoolSize));

        // Initialize memory with tag bytes
        if (LargeMemoryPool)
        {
            std::memset(LargeMemoryPool, MEM_WIPETAG, LARGE_MEMORYPOOL_SIZE);
        }
        if (SmallMemoryPool)
        {
            std::memset(SmallMemoryPool, MEM_WIPETAG, SmallMemoryPoolSize);
        }
        if (BookkeepingPool)
        {
            std::memset(BookkeepingPool, 0, BookkeepingPoolSize);
        }

        // Reset offsets
        SmallMemoryPoolOffset = 0;
        BookkeepingPoolOffset = 0;
        LargeMemoryPoolOffset = 0;

        // Initialize the fixed-size pools
        InitializeSmallPools();

        bIsInitialized = true;
    }

    void FGenericPlatformMallocCrash::SetAsGMalloc()
    {
        // Lock to the crashed thread - this lock is never released
        InternalLock.lock();

        // Initialize pools if not done yet
        Initialize();

        // Record the crashed thread
        CrashedThreadId = std::this_thread::get_id();

        // Mark crash malloc as active
        GIsMallocCrashActive.store(true, std::memory_order_release);
    }

    bool FGenericPlatformMallocCrash::CheckThreadForAllocation() const
    {
        // Only the crashed thread can allocate
        if (CrashedThreadId == std::this_thread::get_id())
        {
            return true;
        }

        // Non-crashed threads should not allocate during crash
        // In a real implementation, we would suspend them
        return false;
    }

    bool FGenericPlatformMallocCrash::CheckThreadForFree() const
    {
        // Only the crashed thread can free
        if (CrashedThreadId == std::this_thread::get_id())
        {
            return true;
        }

        // Non-crashed threads skip frees silently
        return false;
    }

    FMallocCrashPool* FGenericPlatformMallocCrash::ChoosePoolForSize(u32 AllocationSize)
    {
        // Linear search for first pool that can fit the allocation
        // Skip exhausted pools
        for (int i = 0; i < NUM_POOLS; ++i)
        {
            FMallocCrashPool& Pool = GetPool(i);
            if (Pool.AllocationSize >= AllocationSize && Pool.NumUsed < Pool.MaxNumAllocations)
            {
                return &Pool;
            }
        }
        return nullptr;
    }

    FMallocCrashPool* FGenericPlatformMallocCrash::FindPoolForAlloc(void* Ptr)
    {
        if (!Ptr)
        {
            return nullptr;
        }

        u8* BytePtr = static_cast<u8*>(Ptr);

        // Search pools for the one containing this pointer
        for (int i = 0; i < NUM_POOLS; ++i)
        {
            FMallocCrashPool& Pool = GetPool(i);
            if (Pool.ContainsPointer(BytePtr))
            {
                return &Pool;
            }
        }
        return nullptr;
    }

    void* FGenericPlatformMallocCrash::AllocateFromLargePool(sizet Size, u32 Alignment)
    {
        if (!LargeMemoryPool)
        {
            return nullptr;
        }

        // Align the offset
        u32 AlignedOffset = (LargeMemoryPoolOffset + Alignment - 1) & ~(Alignment - 1);

        if (AlignedOffset + Size > LARGE_MEMORYPOOL_SIZE)
        {
            // Out of memory in large pool
            return nullptr;
        }

        void* Result = LargeMemoryPool + AlignedOffset;
        LargeMemoryPoolOffset = static_cast<u32>(AlignedOffset + Size);

        // Tag allocated memory
        std::memset(Result, MEM_TAG, Size);

        return Result;
    }

    bool FGenericPlatformMallocCrash::IsPtrInSmallPool(void* Ptr) const
    {
        if (!SmallMemoryPool || !Ptr)
        {
            return false;
        }
        u8* BytePtr = static_cast<u8*>(Ptr);
        return BytePtr >= SmallMemoryPool && BytePtr < SmallMemoryPool + SmallMemoryPoolSize;
    }

    bool FGenericPlatformMallocCrash::IsPtrInLargePool(void* Ptr) const
    {
        if (!LargeMemoryPool || !Ptr)
        {
            return false;
        }
        u8* BytePtr = static_cast<u8*>(Ptr);
        return BytePtr >= LargeMemoryPool && BytePtr < LargeMemoryPool + LARGE_MEMORYPOOL_SIZE;
    }

    bool FGenericPlatformMallocCrash::IsOwnedPointer(void* Ptr) const
    {
        return IsPtrInSmallPool(Ptr) || IsPtrInLargePool(Ptr);
    }

    bool FGenericPlatformMallocCrash::IsOnCrashedThread() const
    {
        return CrashedThreadId == std::this_thread::get_id();
    }

    void FGenericPlatformMallocCrash::PrintPoolsUsage() const
    {
#ifdef _DEBUG
        OLO_CORE_INFO("FPoolDesc used:");
        for (u32 Index = 0; Index < NUM_POOLS; ++Index)
        {
            const FMallocCrashPool& CrashPool = GetPool(Index);
            OLO_CORE_INFO("  FPoolDesc({:5},{:4}),", CrashPool.AllocationSize, CrashPool.MaxUsedIndex);
        }

        OLO_CORE_INFO("FPoolDesc tweaked:");
        for (u32 Index = 0; Index < NUM_POOLS; ++Index)
        {
            const FMallocCrashPool& CrashPool = GetPool(Index);
            // Align to 16 for suggested pool sizes
            u32 TweakedSize = ((CrashPool.MaxUsedIndex * 2 + 16 + 15) / 16) * 16;
            OLO_CORE_INFO("  FPoolDesc({:5},{:4}),", CrashPool.AllocationSize, TweakedSize);
        }
        OLO_CORE_INFO("LargeMemoryPoolOffset={}", LargeMemoryPoolOffset);
#endif // _DEBUG
    }

    sizet FGenericPlatformMallocCrash::GetAllocationSize(void* Ptr) const
    {
        if (!Ptr)
        {
            return 0;
        }

        u8* BytePtr = static_cast<u8*>(Ptr);

        // Check small pools
        for (int i = 0; i < NUM_POOLS; ++i)
        {
            const FMallocCrashPool& Pool = GetPool(i);
            if (Pool.ContainsPointer(BytePtr))
            {
                return static_cast<sizet>(Pool.GetAllocationSize(BytePtr));
            }
        }

        // Large pool doesn't track sizes - return 0
        return 0;
    }

    void* FGenericPlatformMallocCrash::Malloc(sizet Size, u32 Alignment)
    {
        if (!CheckThreadForAllocation())
        {
            return nullptr;
        }

        if (Size == 0)
        {
            return nullptr;
        }

        // Ensure minimum alignment
        Alignment = std::max(Alignment, static_cast<u32>(REQUIRED_ALIGNMENT));

        // Try small pools first
        if (Size <= PoolDescs[NUM_POOLS - 1].Size)
        {
            FMallocCrashPool* Pool = ChoosePoolForSize(static_cast<u32>(Size));
            if (Pool)
            {
                u8* Result = Pool->AllocateFromPool(static_cast<u32>(Size));
                if (Result)
                {
                    return Result;
                }
            }
        }

        // Fall back to large pool (bump allocator)
        return AllocateFromLargePool(Size, Alignment);
    }

    void FGenericPlatformMallocCrash::Free(void* Ptr)
    {
        if (!CheckThreadForFree())
        {
            return;
        }

        if (!Ptr)
        {
            return;
        }

        // Try to free from small pools
        FMallocCrashPool* Pool = FindPoolForAlloc(Ptr);
        if (Pool)
        {
            Pool->TryFreeFromPool(static_cast<u8*>(Ptr));
            return;
        }

        // Large pool uses bump allocator - individual frees not supported
        // Just ignore (memory will be reclaimed when crash handling completes)
    }

    void* FGenericPlatformMallocCrash::Realloc(void* Ptr, sizet NewSize, u32 Alignment)
    {
        if (!CheckThreadForAllocation())
        {
            return nullptr;
        }

        if (!Ptr)
        {
            return Malloc(NewSize, Alignment);
        }

        if (NewSize == 0)
        {
            Free(Ptr);
            return nullptr;
        }

        // Get old size for proper copy
        sizet OldSize = GetAllocationSize(Ptr);

        // Allocate new memory
        void* NewPtr = Malloc(NewSize, Alignment);
        if (!NewPtr)
        {
            return nullptr;
        }

        // Copy old data
        sizet CopySize = (OldSize > 0) ? std::min(OldSize, NewSize) : NewSize;
        std::memcpy(NewPtr, Ptr, CopySize);

        // Free old allocation
        Free(Ptr);

        return NewPtr;
    }

} // namespace OloEngine
