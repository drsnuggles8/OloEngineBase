// OloEngine Memory System
// Ported from Unreal Engine's GenericPlatform/GenericPlatformMemory.h

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/Platform.h"

#include <cstring>
#include <string>
#include <vector>

#ifndef OLO_CHECK_LARGE_ALLOCATIONS
#define OLO_CHECK_LARGE_ALLOCATIONS 0
#endif

namespace OloEngine
{

#if OLO_CHECK_LARGE_ALLOCATIONS
    namespace Memory::Private
    {
        extern bool GEnableLargeAllocationChecks;
        extern i32 GLargeAllocationThreshold;
        // Note: Console variable support would be added here if/when OloEngine has a console variable system
    } // namespace Memory::Private
#endif

    // Forward declarations
    class FMalloc;
    class FOutputDevice;
    // Holds generic memory stats, internally implemented as a map.
    struct FGenericMemoryStats;

// Platform-dependent "bucket" for memory size, where Default is the normal, or possibly the largest.
// This is generally used for texture LOD settings for how to fit in smaller memory devices
#define PLATFORM_MEMORY_SIZE_BUCKET_LIST(XBUCKET)                                                                                 \
    /* not used with texture LODs (you can't use bigger textures than what is cooked out, which is what Default should map to) */ \
    XBUCKET(Largest)                                                                                                              \
    XBUCKET(Larger)                                                                                                               \
    /* these are used by texture LODs */                                                                                          \
    XBUCKET(Default)                                                                                                              \
    XBUCKET(Smaller)                                                                                                              \
    XBUCKET(Smallest)                                                                                                             \
    XBUCKET(Tiniest)

#define PLATFORM_MEMORY_SIZE_BUCKET_ENUM(Name) Name,
    enum class EPlatformMemorySizeBucket
    {
        PLATFORM_MEMORY_SIZE_BUCKET_LIST(PLATFORM_MEMORY_SIZE_BUCKET_ENUM)
    };
#undef PLATFORM_MEMORY_SIZE_BUCKET_ENUM

    inline const char* LexToString(EPlatformMemorySizeBucket Bucket)
    {
#define PLATFORM_MEMORY_SIZE_BUCKET_LEXTOSTRING(Name) \
    case EPlatformMemorySizeBucket::Name:             \
        return #Name;
        switch (Bucket)
        {
            PLATFORM_MEMORY_SIZE_BUCKET_LIST(PLATFORM_MEMORY_SIZE_BUCKET_LEXTOSTRING)
        }
#undef PLATFORM_MEMORY_SIZE_BUCKET_LEXTOSTRING

        return "Unknown";
    }

    enum class EMemcpyCachePolicy : u8
    {
        // Writes to destination memory are cache-visible (default).
        // This should be used if copy results are immediately accessed by CPU.
        StoreCached,

        // Writes to destination memory bypass cache (avoiding pollution).
        // Optimizes for large copies that aren't read from soon after.
        StoreUncached,
    };

    // @brief Struct used to hold common memory constants for all platforms.
    // These values don't change over the entire life of the executable.
    struct FGenericPlatformMemoryConstants
    {
        // The amount of actual physical memory, in bytes (needs to handle >4GB for 64-bit devices running 32-bit code).
        u64 TotalPhysical = 0;

        // The amount of virtual memory, in bytes.
        u64 TotalVirtual = 0;

        // The size of a physical page, in bytes. This is also the granularity for PageProtect(), commitment and properties (e.g. ability to access) of the physical RAM.
        sizet PageSize = 0;

        // Some platforms have advantages if memory is allocated in chunks larger than PageSize (e.g. VirtualAlloc() seems to have 64KB granularity as of now).
        // This value is the minimum allocation size that the system will use behind the scenes.
        sizet OsAllocationGranularity = 0;

        // The size of a "page" in Binned2 malloc terms, in bytes. Should be at least 64KB. BinnedMalloc expects memory returned from BinnedAllocFromOS() to be aligned on BinnedPageSize boundary.
        sizet BinnedPageSize = 0;

        // This is the "allocation granularity" in Binned malloc terms, i.e. BinnedMalloc will allocate the memory in increments of this value. If zero, Binned will use BinnedPageSize for this value.
        sizet BinnedAllocationGranularity = 0;

        // Starting address for the available virtual address space.
        // Can be used with AddressLimit to determine address space range for binned allocators
        u64 AddressStart = 0;

        // An estimate of the range of addresses expected to be returned by BinnedAllocFromOS(). Binned
        // Malloc will adjust its internal structures to make lookups for memory allocations O(1) for this range.
        // It is ok to go outside this range, lookups will just be a little slower
        u64 AddressLimit = static_cast<u64>(0xffffffff) + 1;

        // Approximate physical RAM in GB; 1 on everything except PC. Used for "coarse tuning", like FPlatformMisc::NumberOfCores().
        u32 TotalPhysicalGB = 1;
    };

    using FPlatformMemoryConstants = FGenericPlatformMemoryConstants;

    // @brief Struct used to hold common memory stats for all platforms.
    // These values may change over the entire life of the executable.
    struct FGenericPlatformMemoryStats : public FPlatformMemoryConstants
    {
        // The amount of physical memory currently available, in bytes.
        u64 AvailablePhysical;

        // The amount of virtual memory currently available, in bytes.
        u64 AvailableVirtual;

        // The amount of physical memory used by the process, in bytes.
        u64 UsedPhysical;

        // The peak amount of physical memory used by the process, in bytes.
        u64 PeakUsedPhysical;

        // Total amount of virtual memory used by the process.
        u64 UsedVirtual;

        // The peak amount of virtual memory used by the process.
        u64 PeakUsedVirtual;

        // Memory pressure states, useful for platforms in which the available memory estimate
        // may not take in to account memory reclaimable from closing inactive processes or resorting to swap.
        enum class EMemoryPressureStatus : u8
        {
            Unknown,
            Nominal,
            Warning,
            Critical, // high risk of OOM conditions
        };

        EMemoryPressureStatus GetMemoryPressureStatus() const;

        // Default constructor, clears all variables.
        FGenericPlatformMemoryStats();

        struct FPlatformSpecificStat
        {
            const char* Name;
            u64 Value;

            FPlatformSpecificStat(const char* InName, u64 InValue)
                : Name(InName), Value(InValue)
            {
            }
        };

        std::vector<FPlatformSpecificStat> GetPlatformSpecificStats() const;

        u64 GetAvailablePhysical(bool bExcludeExtraDevMemory) const;

        // Called by FCsvProfiler::EndFrame to set platform specific CSV stats.
        void SetEndFrameCsvStats() const {}
    };

    using FPlatformMemoryStats = FGenericPlatformMemoryStats;

    // @brief Contains shared/private information for a single page allocation from the kernel. A page
    // allocation may contain many pages.
    struct FForkedPageAllocation
    {
        // Start/End virtual address for the allocation.
        u64 PageStart;
        u64 PageEnd;

        // The amount of memory in this allocation range that is shared across the forked
        // child processes.
        u64 SharedCleanKiB;
        u64 SharedDirtyKiB;

        // The amount of memory in this allocation range that has been written to by the child
        // process, and as a result has been made unique to the process.
        u64 PrivateCleanKiB;
        u64 PrivateDirtyKiB;
    };

// @brief FMemory_Alloca/alloca implementation. This can't be a function, even FORCEINLINE'd because there's no guarantee that
// the memory returned in a function will stick around for the caller to use.
#ifdef OLO_PLATFORM_WINDOWS
#define __OloMemory_Alloca_Func _alloca
#else
#define __OloMemory_Alloca_Func alloca
#endif

#define OloMemory_Alloca(Size) ((Size == 0) ? 0 : (void*)(((uptr)__OloMemory_Alloca_Func(Size + 15) + 15) & ~15))

// Version that supports alignment requirement. However since the old alignment was always forced to be 16, this continues to enforce a min alignment of 16 but allows a larger value.
#define OloMemory_Alloca_Aligned(Size, Alignment) ((Size == 0) ? 0 : ((Alignment <= 16) ? OloMemory_Alloca(Size) : (void*)(((uptr)__OloMemory_Alloca_Func(Size + Alignment - 1) + Alignment - 1) & ~(Alignment - 1))))

    // Generic implementation for most platforms, these tend to be unused and unimplemented.
    struct FGenericPlatformMemory
    {
        // Set to true if we encounters out of memory.
        static bool bIsOOM;

        // Set to size of allocation that triggered out of memory, zero otherwise.
        static u64 OOMAllocationSize;

        // Set to alignment of allocation that triggered out of memory, zero otherwise.
        static u32 OOMAllocationAlignment;

        // Preallocated buffer to delete on out of memory. Used by OOM handling and crash reporting.
        static void* BackupOOMMemoryPool;

        // Size of BackupOOMMemoryPool in bytes.
        static u32 BackupOOMMemoryPoolSize;

        // Various memory regions that can be used with memory stats. The exact meaning of
        // the enums are relatively platform-dependent, although the general ones (Physical, GPU)
        // are straightforward. A platform can add more of these, and it won't affect other
        // platforms, other than a minuscule amount of memory for the StatManager to track the
        // max available memory for each region (uses an array FPlatformMemory::MCR_MAX big)
        enum EMemoryCounterRegion
        {
            MCR_Invalid,           // not memory
            MCR_Physical,          // main system memory
            MCR_GPU,               // memory directly a GPU (graphics card, etc)
            MCR_GPUSystem,         // system memory directly accessible by a GPU
            MCR_TexturePool,       // presized texture pools
            MCR_StreamingPool,     // amount of texture pool available for streaming.
            MCR_UsedStreamingPool, // amount of texture pool used for streaming.
            MCR_GPUDefragPool,     // presized pool of memory that can be defragmented.
            MCR_PhysicalLLM,       // total physical memory including CPU and GPU
            MCR_MAX
        };

        // Which allocator is being used
        enum EMemoryAllocatorToUse
        {
            Ansi,     // Default C allocator
            Stomp,    // Allocator to check for memory stomping
            TBB,      // Thread Building Blocks malloc
            Jemalloc, // Linux/FreeBSD malloc
            Binned,   // Older binned malloc
            Binned2,  // Newer binned malloc
            Binned3,  // Newer VM-based binned malloc, 64 bit only
            Platform, // Custom platform specific allocator
            Mimalloc, // mimalloc
            Libpas,   // libpas
        };

        // Current allocator
        static EMemoryAllocatorToUse AllocatorToUse;

        // Flags used for shared memory creation/open
        enum ESharedMemoryAccess
        {
            Read = (1 << 1),
            Write = (1 << 2)
        };

        // Generic representation of a shared memory region
        struct FSharedMemoryRegion
        {
            // Returns the name of the region
            const char* GetName() const
            {
                return Name;
            }

            // Returns the beginning of the region in process address space
            void* GetAddress()
            {
                return Address;
            }

            // Returns the beginning of the region in process address space
            const void* GetAddress() const
            {
                return Address;
            }

            // Returns size of the region in bytes
            sizet GetSize() const
            {
                return Size;
            }

            FSharedMemoryRegion(const std::string& InName, u32 InAccessMode, void* InAddress, sizet InSize);

          protected:
            enum Limits
            {
                MaxSharedMemoryName = 128
            };

            // Name of the region
            char Name[MaxSharedMemoryName];

            // Access mode for the region
            u32 AccessMode;

            // The actual buffer
            void* Address;

            // Size of the buffer
            sizet Size;
        };

        // @brief Initializes platform memory specific constants.
        static void Init();

        [[noreturn]] static void OnOutOfMemory(u64 Size, u32 Alignment);

        // @brief Initializes the memory pools, should be called by the init function.
        static void SetupMemoryPools();

        // @return how much memory the platform should pre-allocate for crash handling (this will be allocated ahead of time, and freed when system runs out of memory).
        static u32 GetBackMemoryPoolSize()
        {
            // by default, don't create a backup memory buffer
            return 0;
        }

        // @return the default allocator.
        static FMalloc* BaseAllocator();

        // @return platform specific current memory statistics. Note: On some platforms, unused allocator cached memory is taken into account in AvailablePhysical.
        static FPlatformMemoryStats GetStats();

        // @return platform specific raw stats.
        static FPlatformMemoryStats GetStatsRaw();

        // @return memory used for platforms that can do it quickly (without affecting stat unit much)
        static u64 GetMemoryUsedFast();

        // Writes all platform specific current memory statistics in the format usable by the malloc profiler.
        static void GetStatsForMallocProfiler(FGenericMemoryStats& OutStats);

        // @return platform specific memory constants.
        static const FPlatformMemoryConstants& GetConstants();

        // @return approximate physical RAM in GB.
        static u32 GetPhysicalGBRam();

        // Changes the protection on a region of committed pages in the virtual address space.
        //
        // @param Ptr Address to the starting page of the region of pages whose access protection attributes are to be changed.
        // @param Size The size of the region whose access protection attributes are to be changed, in bytes.
        // @param bCanRead Can the memory be read.
        // @param bCanWrite Can the memory be written to.
        // @return True if the specified pages' protection mode was changed.
        static bool PageProtect(void* const Ptr, const sizet Size, const bool bCanRead, const bool bCanWrite);

        // Allocates pages from the OS.
        //
        // @param Size Size to allocate, not necessarily aligned
        //
        // @return OS allocated pointer for use by binned allocator
        static void* BinnedAllocFromOS(sizet Size);

        // Returns pages allocated by BinnedAllocFromOS to the OS.
        //
        // @param Ptr A pointer previously returned from BinnedAllocFromOS
        // @param Size size of the allocation previously passed to BinnedAllocFromOS
        static void BinnedFreeToOS(void* Ptr, sizet Size);

        // Performs initial setup for MiMalloc.
        // This is a noop on platforms that do not support MiMalloc, or when MIMALLOC_ENABLED is not defined.
        static void MiMallocInit()
        {
        }

        // Performs initial setup for Nano malloc.
        // This is a noop on non-apple platforms
        static void NanoMallocInit()
        {
        }

        // Was this pointer allocated by the OS malloc?
        // Currently only Apple platforms implement this to detect small block allocations.
        //
        // @param Ptr The pointer to query
        // @return True if this pointer was allocated by the OS.
        static bool PtrIsOSMalloc(void* Ptr)
        {
            (void)Ptr;
            return false;
        }

        // Nano Malloc is Apple's tiny block allocator.
        // Does the Nano malloc zone exist?
        //
        // @return True if Nano malloc is enabled and available.
        static bool IsNanoMallocAvailable()
        {
            return false;
        }

        // Was this pointer allocated by in the Nano Malloc Zone?
        // Currently only Apple platforms implement this to detect small block allocations.
        //
        // @param Ptr The pointer to query
        // @return True if this pointer is in the Nano Malloc Region
        static bool PtrIsFromNanoMalloc(void* Ptr)
        {
            (void)Ptr;
            return false;
        }

        class FBasicVirtualMemoryBlock
        {
          protected:
            void* Ptr;
            u32 VMSizeDivVirtualSizeAlignment;

          public:
            FBasicVirtualMemoryBlock()
                : Ptr(nullptr), VMSizeDivVirtualSizeAlignment(0)
            {
            }

            FBasicVirtualMemoryBlock(void* InPtr, u32 InVMSizeDivVirtualSizeAlignment)
                : Ptr(InPtr), VMSizeDivVirtualSizeAlignment(InVMSizeDivVirtualSizeAlignment)
            {
            }

            FBasicVirtualMemoryBlock(const FBasicVirtualMemoryBlock& Other) = default;
            FBasicVirtualMemoryBlock& operator=(const FBasicVirtualMemoryBlock& Other) = default;

            OLO_FINLINE u32 GetActualSizeInPages() const
            {
                return VMSizeDivVirtualSizeAlignment;
            }

            OLO_FINLINE void* GetVirtualPointer() const
            {
                return Ptr;
            }

            // Platform-specific implementations would define these:
            // void Commit(sizet InOffset, sizet InSize);
            // void Decommit(sizet InOffset, sizet InSize);
            // void FreeVirtual();
            // static FPlatformVirtualMemoryBlock AllocateVirtual(sizet Size, sizet InAlignment);
            // static sizet GetCommitAlignment();
            // static sizet GetVirtualSizeAlignment();
        };

        // Some platforms may pool allocations of this size to reduce OS calls. This function
        // serves as a hint for BinnedMalloc's CachedOSPageAllocator so it does not cache these allocations additionally
        static bool BinnedPlatformHasMemoryPoolForThisSize(sizet Size)
        {
            (void)Size;
            return false;
        }

        // Dumps basic platform memory statistics into the specified output device.
        static void DumpStats(FOutputDevice& Ar);

        // Dumps basic platform memory statistics and allocator specific statistics into the specified output device.
        static void DumpPlatformAndAllocatorStats(FOutputDevice& Ar);

        // Return which "high level", per platform, memory bucket we are in
        static EPlatformMemorySizeBucket GetMemorySizeBucket();

        // @name Memory functions

        // Copies count bytes of characters from Src to Dest. If some regions of the source
        // area and the destination overlap, memmove ensures that the original source bytes
        // in the overlapping region are copied before being overwritten.  NOTE: make sure
        // that the destination buffer is the same size or larger than the source buffer!
        static OLO_FINLINE void* Memmove(void* Dest, const void* Src, sizet Count)
        {
            return memmove(Dest, Src, Count);
        }

        static OLO_FINLINE i32 Memcmp(const void* Buf1, const void* Buf2, sizet Count)
        {
            return memcmp(Buf1, Buf2, Count);
        }

        static OLO_FINLINE void* Memset(void* Dest, u8 Char, sizet Count)
        {
            return memset(Dest, Char, Count);
        }

        static OLO_FINLINE void* Memzero(void* Dest, sizet Count)
        {
            return memset(Dest, 0, Count);
        }

        static OLO_FINLINE void* Memcpy(void* Dest, const void* Src, sizet Count)
        {
            return memcpy(Dest, Src, Count);
        }

        // Memcpy optimized for big blocks.
        static OLO_FINLINE void* BigBlockMemcpy(void* Dest, const void* Src, sizet Count)
        {
            return memcpy(Dest, Src, Count);
        }

        // On some platforms memcpy optimized for big blocks that avoid L2 cache pollution are available
        static OLO_FINLINE void* StreamingMemcpy(void* Dest, const void* Src, sizet Count)
        {
            return memcpy(Dest, Src, Count);
        }

        // On some platforms memcpy can be distributed over multiple threads for throughput.
        static inline void* ParallelMemcpy(void* Dest, const void* Src, sizet Count, EMemcpyCachePolicy Policy = EMemcpyCachePolicy::StoreCached)
        {
            (void)Policy;
            return memcpy(Dest, Src, Count);
        }

      private:
        template<typename T>
        static inline void Valswap(T& A, T& B)
        {
            // Usually such an implementation would use move semantics, but
            // we're only ever going to call it on fundamental types and MoveTemp
            // is not necessarily in scope here anyway, so we don't want to
            // #include it if we don't need to.
            T Tmp = A;
            A = B;
            B = Tmp;
        }

        static void MemswapGreaterThan8(void* Ptr1, void* Ptr2, sizet Size);

      public:
        static inline void Memswap(void* Ptr1, void* Ptr2, sizet Size)
        {
            switch (Size)
            {
                case 0:
                    break;

                case 1:
                    Valswap(*(u8*)Ptr1, *(u8*)Ptr2);
                    break;

                case 2:
                    Valswap(*(u16*)Ptr1, *(u16*)Ptr2);
                    break;

                case 3:
                    Valswap(*((u16*&)Ptr1)++, *((u16*&)Ptr2)++);
                    Valswap(*(u8*)Ptr1, *(u8*)Ptr2);
                    break;

                case 4:
                    Valswap(*(u32*)Ptr1, *(u32*)Ptr2);
                    break;

                case 5:
                    Valswap(*((u32*&)Ptr1)++, *((u32*&)Ptr2)++);
                    Valswap(*(u8*)Ptr1, *(u8*)Ptr2);
                    break;

                case 6:
                    Valswap(*((u32*&)Ptr1)++, *((u32*&)Ptr2)++);
                    Valswap(*(u16*)Ptr1, *(u16*)Ptr2);
                    break;

                case 7:
                    Valswap(*((u32*&)Ptr1)++, *((u32*&)Ptr2)++);
                    Valswap(*((u16*&)Ptr1)++, *((u16*&)Ptr2)++);
                    Valswap(*(u8*)Ptr1, *(u8*)Ptr2);
                    break;

                case 8:
                    Valswap(*(u64*)Ptr1, *(u64*)Ptr2);
                    break;

                case 16:
                    Valswap(((u64*)Ptr1)[0], ((u64*)Ptr2)[0]);
                    Valswap(((u64*)Ptr1)[1], ((u64*)Ptr2)[1]);
                    break;

                default:
                    MemswapGreaterThan8(Ptr1, Ptr2, Size);
                    break;
            }
        }

        // Loads a simple POD type from unaligned memory.
        //
        // @param Ptr unaligned memory of at least size sizeof(T)
        // @return Value at Ptr
        template<typename T>
        static inline T ReadUnaligned(const void* Ptr)
        {
            T AlignedT;
            memcpy((void*)&AlignedT, Ptr, sizeof(T));
            return AlignedT;
        }

        // Stores a simple POD type to unaligned memory.
        //
        // @param Ptr unaligned memory of at least size sizeof(T)
        // @param InValue value to write at Ptr
        template<typename T>
        static OLO_FINLINE void WriteUnaligned(void* Ptr, const T& InValue)
        {
            memcpy(Ptr, &InValue, sizeof(T));
        }

        // Maps a named shared memory region into process address space (creates or opens it)
        //
        // @param Name unique name of the shared memory region (should not contain [back]slashes to remain cross-platform).
        // @param bCreate whether we're creating it or just opening existing (created by some other process).
        // @param AccessMode mode which we will be accessing it (use values from ESharedMemoryAccess)
        // @param Size size of the buffer (should be >0. Also, the real size is subject to platform limitations and may be increased to match page size)
        //
        // @return pointer to FSharedMemoryRegion (or its descendants) if successful, nullptr if not.
        static FSharedMemoryRegion* MapNamedSharedMemoryRegion(const std::string& Name, bool bCreate, u32 AccessMode, sizet Size);

        // Unmaps a name shared memory region
        //
        // @param MemoryRegion an object that encapsulates a shared memory region (will be destroyed even if function fails!)
        //
        // @return true if successful
        static bool UnmapNamedSharedMemoryRegion(FSharedMemoryRegion* MemoryRegion);

        // Gets whether this platform supports Fast VRAM memory
        //     Ie, whether TexCreate_FastVRAM flags actually mean something or not
        //
        // @return bool true if supported, false if not
        static OLO_FINLINE bool SupportsFastVRAMMemory()
        {
            return false;
        }

        // Returns true if debug memory has been assigned to the title for general use.
        // Only applies to platforms with fixed memory and no paging.
        static bool IsExtraDevelopmentMemoryAvailable();

        // Returns >0 if debug memory has been assigned to the title for general use.
        // Only applies to platforms with fixed memory and no paging.
        static u64 GetExtraDevelopmentMemorySize();

        // Returns initial program size or 0 if the platform doesn't track initial program size
        static u64 GetProgramSize()
        {
            return ProgramSize;
        }

        // Sets the initial program size
        static void SetProgramSize(u64 InProgramSize)
        {
            ProgramSize = InProgramSize;
        }

        // This function sets AllocFunction and FreeFunction and returns true, or just returns false.
        // These functions are the platform dependant low low low level functions that LLM uses to allocate memory.
        static bool GetLLMAllocFunctions(void* (*&OutAllocFunction)(sizet), void (*&OutFreeFunction)(void*, sizet), i32& OutAlignment);

        // Called for all default tracker LLM allocations and frees, when LLM is enabled.
        // Provides a single alloc/free hook that platforms can implement to support platform specific memory analysis tools.
        OLO_FINLINE static void OnLowLevelMemory_Alloc(void const* Pointer, u64 Size, u64 Tag)
        {
            (void)Pointer;
            (void)Size;
            (void)Tag;
        }
        OLO_FINLINE static void OnLowLevelMemory_Free(void const* Pointer, u64 Size, u64 Tag)
        {
            (void)Pointer;
            (void)Size;
            (void)Tag;
        }

        // Called once at LLM initialization time to let the platform add any custom tags
        static void RegisterCustomLLMTags() {}

        // Called once per frame when LLM is collating the data for the current frame.
        // Can be used to set platform-specific calculated tag data via SetTagAmountForTracker
        static void UpdateCustomLLMTags() {}

        // Indicates whether LLM allocations are already accounted for with tracking and in GetStats.
        // Returns true if LLM allocations come from a memory pool separate from the main engine's memory or
        // are already tagged with tracking by the platform memory system.
        // Returns false if LLM uses the regular memory shared with the engine and allocations are not tracked.
        // @see GetLLMAllocFunctions
        static bool TracksLLMAllocations()
        {
            return false;
        }

        // Returns true if Protecting the parent processes pages has been enabled
        // Only supported on platforms that support forking
        static bool HasForkPageProtectorEnabled()
        {
            return false;
        }

        // Return the page allocations from the operating system (/proc/self/smaps). This only means something on
        // platforms that can fork and have Copy On Write behavior.
        static bool GetForkedPageAllocationInfo(std::vector<FForkedPageAllocation>& OutPageAllocationInfos)
        {
            (void)OutPageAllocationInfos;
            return false; // Most platforms do not implement this.
        }

        // Returns a pretty-string for an amount of memory given in bytes.
        //
        // @param Memory amount in bytes
        // @return Memory in a pretty formatted string
        static std::string PrettyMemory(u64 Memory);

        // Return true if the platform can allocate a lot more virtual memory than physical memory
        // It's true for most platforms, but iOS needs a special entitlement and Linux a kernel config for this
        OLO_FINLINE static bool CanOverallocateVirtualMemory()
        {
            return true;
        }

        // This bit is always zero in user mode addresses and most likely won't be used by current or future
        // CPU features like ARM's PAC / Top-Byte Ignore or Intel's Linear Address Masking / 5-Level Paging
#if defined(__x86_64__) || defined(_M_X64)
        static constexpr u32 KernelAddressBit = 63;
#elif defined(__aarch64__) || defined(_M_ARM64)
        static constexpr u32 KernelAddressBit = 55;
#else
        static constexpr u32 KernelAddressBit = 63; // Default for unknown architectures
#endif

      protected:
        friend struct FGenericStatsUpdater;

        // @brief Updates platform specific stats. This method is called through FGenericStatsUpdater from the task graph thread.
        static void InternalUpdateStats(const FPlatformMemoryStats& MemoryStats);

        // @brief Program memory allocation in bytes.
        static u64 ProgramSize;
    };

    // Use GenericPlatformMemory as FPlatformMemory for now
    using FPlatformMemory = FGenericPlatformMemory;

} // namespace OloEngine
