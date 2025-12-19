// PlatformMallocCrash.h - Emergency allocator for crash handling
// Ported 1:1 from UE5.7 FPlatformMallocCrash

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/Memory.h"
#include "OloEngine/Templates/UnrealTypeTraits.h"
#include <atomic>
#include <mutex>
#include <thread>

namespace OloEngine
{
    // Forward declaration
    class FGenericPlatformMallocCrash;

    // @brief Pool configuration descriptor
    struct FPoolDesc
    {
        constexpr FPoolDesc(u32 InSize, u32 InNumAllocs)
            : Size(InSize), NumAllocs(InNumAllocs) {}

        const u32 Size;      // Allocation size for this pool
        const u32 NumAllocs; // Maximum number of allocations
    };

    // @brief Allocation descriptor - tracks size and pointer for each allocation in a pool
    struct FPtrInfo
    {
        u64 Size; // Size of the allocation (0 means free)
        u8* Ptr;  // Address of the allocation

#if INTPTR_MAX == INT32_MAX
        u8 Padding[4]; // Explicit padding for 32-bit builds
#endif

        FPtrInfo() : Size(0), Ptr(nullptr) {}
        explicit FPtrInfo(void* NewPtr) : Size(0), Ptr(static_cast<u8*>(NewPtr)) {}
    };

    // @brief Fixed-size memory pool for crash allocator.
    // Uses linear search for allocation, O(1) free via pointer arithmetic.
    struct FMallocCrashPool
    {
        u32 NumUsed = 0;      // Current number of used allocations
        u32 MaxUsedIndex = 0; // Highest index ever used (optimization for search)
        u32 MaxNumUsed = 0;   // Peak usage (for debugging)
        u32 TotalNumUsed = 0; // Total allocations made (including freed)

        u32 AllocationSize = 0;          // Fixed allocation size for this pool
        FPtrInfo* Allocations = nullptr; // Bookkeeping array
        u32 AllocationCount = 0;         // Number of allocations array entries
        u8* AllocBase = nullptr;         // Start of pool memory
        u32 MaxNumAllocations = 0;       // Capacity
        u32 AllocatedMemory = 0;         // Total memory (pool + bookkeeping)

        // Memory tags
        static constexpr u8 MEM_TAG = 0xfe;
        static constexpr u8 MEM_WIPETAG = 0xcd;

        FMallocCrashPool() = default;

        // Initialize the pool with the given descriptor
        void Initialize(const FPoolDesc& PoolDesc, FGenericPlatformMallocCrash& Outer);

        // Allocate from pool - linear search for free slot (Size == 0)
        // @return Allocated pointer or nullptr if pool is exhausted
        u8* AllocateFromPool(u32 InAllocationSize);

        // Free - O(1) using pointer arithmetic: Index = (Ptr - AllocBase) / AllocationSize
        // @return true if freed, false if pointer not from this pool
        bool TryFreeFromPool(u8* Ptr);

        // Get allocation size for a pointer
        // @return Size stored in FPtrInfo for this allocation
        u64 GetAllocationSize(u8* Ptr) const;

        // @brief Check if pointer belongs to this pool
        bool ContainsPointer(u8* Ptr) const;

        // Debug verification (no-op in release)
        void DebugVerify() const {}
    };

    // Pool-based allocator used during crash handling.
    // Provides thread-safe memory allocation that doesn't rely on the main allocator
    // which may be in a corrupted state during a crash.
    //
    // Key features:
    // - Preallocated memory pools (no OS calls during crash)
    // - Locks to the crashed thread
    // - 14 fixed-size pools for small allocations (64 bytes to 32KB)
    // - Bump allocator for large allocations (>32KB)
    // - FPtrInfo bookkeeping for O(1) free and size queries
    class FGenericPlatformMallocCrash
    {
        friend struct FMallocCrashPool;

      public:
        enum
        {
            LARGE_MEMORYPOOL_SIZE = 2 * 1024 * 1024, // 2MB
            REQUIRED_ALIGNMENT = 16,
            NUM_POOLS = 14,
            MAX_NUM_ALLOCS_IN_POOL = 2048,
            MEM_TAG = 0xfe,
            MEM_WIPETAG = 0xcd,
        };

        // Gets the crash malloc singleton instance.
        // @param MainMalloc The main allocator to fall back to (only used on first call)
        static FGenericPlatformMallocCrash& Get(void* MainMalloc = nullptr);

        // Checks if crash malloc is currently active.
        // Used to skip certain operations (like TLS cleanup) during crash handling.
        static bool IsActive();

        // Activates crash malloc and sets it as the global allocator.
        // After this call, all memory operations go through the crash malloc.
        void SetAsGMalloc();

        // Allocates memory from the crash pool.
        // First tries small pools, then falls back to large pool.
        // @param Size Size of allocation in bytes
        // @param Alignment Required alignment (max 16)
        // @return Pointer to allocated memory, or nullptr if failed
        void* Malloc(sizet Size, u32 Alignment = REQUIRED_ALIGNMENT);

        // Frees memory back to the crash pool.
        // Returns allocation to appropriate pool for reuse.
        // @param Ptr Pointer to free
        void Free(void* Ptr);

        // Reallocates memory.
        // Uses GetAllocationSize() to copy old data correctly.
        // @param Ptr Pointer to existing allocation
        // @param NewSize New size in bytes
        // @param Alignment Required alignment
        // @return Pointer to reallocated memory
        void* Realloc(void* Ptr, sizet NewSize, u32 Alignment = REQUIRED_ALIGNMENT);

        // @brief Get the size of an allocation.
        sizet GetAllocationSize(void* Ptr) const;

        // @return true if Ptr is within our managed pools
        bool IsOwnedPointer(void* Ptr) const;

        // @return true if this is the crashed thread
        bool IsOnCrashedThread() const;

        // Print pool usage statistics (debug only).
        // Outputs current pool usage to help tune pool sizes.
        void PrintPoolsUsage() const;

        // Pool memory allocation (used during initialization)
        u8* AllocateFromSmallPool(sizet Size);
        u8* AllocateFromBookkeeping(sizet Size);

      protected:
        FGenericPlatformMallocCrash();
        ~FGenericPlatformMallocCrash();

        // Non-copyable
        FGenericPlatformMallocCrash(const FGenericPlatformMallocCrash&) = delete;
        FGenericPlatformMallocCrash& operator=(const FGenericPlatformMallocCrash&) = delete;

        // Initializes all memory pools.
        // Called on first access to Get().
        void Initialize();

        // Initialize the small pools (FMallocCrashPool instances).
        void InitializeSmallPools();

        // Checks if the current thread is allowed to allocate.
        // Non-crashed threads will be blocked.
        bool CheckThreadForAllocation() const;

        // Checks if the current thread is allowed to free.
        // Non-crashed threads may skip or block.
        bool CheckThreadForFree() const;

        // Allocates from the large memory pool (bump allocator).
        void* AllocateFromLargePool(sizet Size, u32 Alignment);

        // Choose the appropriate pool for the given size.
        // Tries pools in order, skipping exhausted ones.
        FMallocCrashPool* ChoosePoolForSize(u32 AllocationSize);

        // Find the pool that contains the given pointer.
        // Uses binary search by AllocBase.
        FMallocCrashPool* FindPoolForAlloc(void* Ptr);

        // Checks if pointer is in the small pool range.
        bool IsPtrInSmallPool(void* Ptr) const;

        // Checks if pointer is in the large pool range.
        bool IsPtrInLargePool(void* Ptr) const;

        // Calculate total size needed for small pools.
        static u32 CalculateSmallPoolTotalSize();

        // Calculate total size needed for bookkeeping.
        static u32 CalculateBookkeepingPoolTotalSize();

      protected:
        std::recursive_mutex InternalLock; // Locks crash malloc to one thread
        std::thread::id CrashedThreadId{}; // ID of crashed thread
        u8* LargeMemoryPool = nullptr;     // Preallocated large pool (bump allocator for >32KB)
        u32 LargeMemoryPoolOffset = 0;     // Current offset in large pool
        u8* SmallMemoryPool = nullptr;     // Preallocated small pool memory
        u32 SmallMemoryPoolOffset = 0;
        u32 SmallMemoryPoolSize = 0;
        u8* BookkeepingPool = nullptr; // Preallocated bookkeeping (FPtrInfo arrays)
        u32 BookkeepingPoolOffset = 0;
        u32 BookkeepingPoolSize = 0;
        bool bIsInitialized = false;

        // Storage for pool objects - placement new to avoid constructor calls
        TTypeCompatibleBytes<FMallocCrashPool> PoolStorage[NUM_POOLS];

        // Pool descriptors - matches UE5.7 configuration
        static constexpr FPoolDesc PoolDescs[NUM_POOLS] = {
            { 64, 224 },
            { 96, 144 },
            { 128, 80 },
            { 192, 560 },
            { 256, 384 },
            { 384, 208 },
            { 512, 48 },
            { 768, 32 },
            { 1024, 32 },
            { 2048, 32 },
            { 4096, 32 },
            { 8192, 32 },
            { 16384, 16 },
            { 32768, 16 },
        };

        FMallocCrashPool& GetPool(i32 Index)
        {
            return *PoolStorage[Index].GetTypedPtr();
        }
        const FMallocCrashPool& GetPool(i32 Index) const
        {
            return *PoolStorage[Index].GetTypedPtr();
        }
    };

    // Platform-specific typedef
    using FPlatformMallocCrash = FGenericPlatformMallocCrash;

} // namespace OloEngine
