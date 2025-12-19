// OloEngine Memory System
// Ported from Unreal Engine's GenericPlatform/GenericPlatformMemory.cpp

#include "OloEngine/Memory/GenericPlatformMemory.h"
#include "OloEngine/Memory/MemoryBase.h"
#include "OloEngine/Memory/MallocAnsi.h"

#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <sstream>
#include <iomanip>

#ifdef OLO_PLATFORM_WINDOWS
#include <windows.h>
#include <psapi.h>
#elif defined(OLO_PLATFORM_LINUX) || defined(OLO_PLATFORM_MACOS)
#include <sys/resource.h>
#include <unistd.h>
#ifdef OLO_PLATFORM_MACOS
#include <mach/mach.h>
#endif
#endif

namespace OloEngine
{

    /*-----------------------------------------------------------------------------
        OLO_CHECK_LARGE_ALLOCATIONS support
    -----------------------------------------------------------------------------*/

#if OLO_CHECK_LARGE_ALLOCATIONS
    namespace Memory::Private
    {
        bool GEnableLargeAllocationChecks = false;
        i32 GLargeAllocationThreshold = 1024 * 1024 * 1024; // 1 GiB default threshold
        // Note: Console variable support would be added here if/when OloEngine has a console variable system
    } // namespace Memory::Private
#endif

    /*-----------------------------------------------------------------------------
        Static member definitions
    -----------------------------------------------------------------------------*/

    bool FGenericPlatformMemory::bIsOOM = false;
    u64 FGenericPlatformMemory::OOMAllocationSize = 0;
    u32 FGenericPlatformMemory::OOMAllocationAlignment = 0;
    FGenericPlatformMemory::EMemoryAllocatorToUse FGenericPlatformMemory::AllocatorToUse = FGenericPlatformMemory::Platform;
    void* FGenericPlatformMemory::BackupOOMMemoryPool = nullptr;
    u32 FGenericPlatformMemory::BackupOOMMemoryPoolSize = 0;
    u64 FGenericPlatformMemory::ProgramSize = 0;

    /*-----------------------------------------------------------------------------
        FGenericPlatformMemoryStats
    -----------------------------------------------------------------------------*/

    FGenericPlatformMemoryStats::FGenericPlatformMemoryStats()
        : FGenericPlatformMemoryConstants(FPlatformMemory::GetConstants()), AvailablePhysical(0), AvailableVirtual(0), UsedPhysical(0), PeakUsedPhysical(0), UsedVirtual(0), PeakUsedVirtual(0)
    {
    }

    FGenericPlatformMemoryStats::EMemoryPressureStatus FGenericPlatformMemoryStats::GetMemoryPressureStatus() const
    {
        // Default implementation - platforms can override
        return EMemoryPressureStatus::Unknown;
    }

    std::vector<FGenericPlatformMemoryStats::FPlatformSpecificStat> FGenericPlatformMemoryStats::GetPlatformSpecificStats() const
    {
        return std::vector<FPlatformSpecificStat>();
    }

    u64 FGenericPlatformMemoryStats::GetAvailablePhysical(bool bExcludeExtraDevMemory) const
    {
        u64 BytesAvailable = AvailablePhysical;

#if !defined(OLO_DIST)
        if (bExcludeExtraDevMemory)
        {
            // Clamp at zero when ExtraDevelopmentMemory > AvailablePhysical
            BytesAvailable -= std::min(FPlatformMemory::GetExtraDevelopmentMemorySize(), BytesAvailable);
        }
#else
        (void)bExcludeExtraDevMemory;
#endif

        return BytesAvailable;
    }

    /*-----------------------------------------------------------------------------
        FSharedMemoryRegion
    -----------------------------------------------------------------------------*/

    FGenericPlatformMemory::FSharedMemoryRegion::FSharedMemoryRegion(const std::string& InName, u32 InAccessMode, void* InAddress, sizet InSize)
        : AccessMode(InAccessMode), Address(InAddress), Size(InSize)
    {
        // Copy name with bounds checking
        sizet CopyLen = std::min(InName.length(), static_cast<sizet>(MaxSharedMemoryName - 1));
        std::memcpy(Name, InName.c_str(), CopyLen);
        Name[CopyLen] = '\0';
    }

    /*-----------------------------------------------------------------------------
        FGenericPlatformMemory
    -----------------------------------------------------------------------------*/

    void FGenericPlatformMemory::Init()
    {
        SetupMemoryPools();
    }

    void FGenericPlatformMemory::SetupMemoryPools()
    {
        // If the platform chooses to have a BackupOOM pool, create it now
        if (FPlatformMemory::GetBackMemoryPoolSize() > 0)
        {
            BackupOOMMemoryPool = FPlatformMemory::BinnedAllocFromOS(FPlatformMemory::GetBackMemoryPoolSize());
            BackupOOMMemoryPoolSize = FPlatformMemory::GetBackMemoryPoolSize();
        }
    }

    FMalloc* FGenericPlatformMemory::BaseAllocator()
    {
        static FMalloc* Instance = nullptr;
        if (Instance != nullptr)
        {
            return Instance;
        }

        Instance = new FMallocAnsi();

        return Instance;
    }

    FPlatformMemoryStats FGenericPlatformMemory::GetStats()
    {
        FPlatformMemoryStats Stats;

#ifdef OLO_PLATFORM_WINDOWS
        MEMORYSTATUSEX MemoryStatus;
        MemoryStatus.dwLength = sizeof(MemoryStatus);
        if (GlobalMemoryStatusEx(&MemoryStatus))
        {
            Stats.TotalPhysical = MemoryStatus.ullTotalPhys;
            Stats.TotalVirtual = MemoryStatus.ullTotalVirtual;
            Stats.AvailablePhysical = MemoryStatus.ullAvailPhys;
            Stats.AvailableVirtual = MemoryStatus.ullAvailVirtual;
            Stats.TotalPhysicalGB = static_cast<u32>((Stats.TotalPhysical + 1024 * 1024 * 1024 - 1) / (1024 * 1024 * 1024));
        }

        PROCESS_MEMORY_COUNTERS ProcessMemoryCounters;
        if (GetProcessMemoryInfo(GetCurrentProcess(), &ProcessMemoryCounters, sizeof(ProcessMemoryCounters)))
        {
            Stats.UsedPhysical = ProcessMemoryCounters.WorkingSetSize;
            Stats.PeakUsedPhysical = ProcessMemoryCounters.PeakWorkingSetSize;
            Stats.UsedVirtual = ProcessMemoryCounters.PagefileUsage;
            Stats.PeakUsedVirtual = ProcessMemoryCounters.PeakPagefileUsage;
        }
#elif defined(OLO_PLATFORM_LINUX)
        // Linux implementation would read from /proc/meminfo and /proc/self/status
        OLO_CORE_WARN("FGenericPlatformMemory::GetStats not fully implemented on this platform");
#elif defined(OLO_PLATFORM_MACOS)
        // macOS implementation would use mach_task_basic_info
        OLO_CORE_WARN("FGenericPlatformMemory::GetStats not fully implemented on this platform");
#else
        OLO_CORE_WARN("FGenericPlatformMemory::GetStats not implemented on this platform");
#endif

        return Stats;
    }

    FPlatformMemoryStats FGenericPlatformMemory::GetStatsRaw()
    {
        return FPlatformMemory::GetStats();
    }

    u64 FGenericPlatformMemory::GetMemoryUsedFast()
    {
        return FPlatformMemory::GetStats().UsedPhysical;
    }

    void FGenericPlatformMemory::GetStatsForMallocProfiler(FGenericMemoryStats& OutStats)
    {
        (void)OutStats;
        // Would populate OutStats with detailed memory statistics
        // Simplified for now - requires stats system integration
    }

    const FPlatformMemoryConstants& FGenericPlatformMemory::GetConstants()
    {
        static FPlatformMemoryConstants Constants;
        static bool bInitialized = false;

        if (!bInitialized)
        {
#ifdef OLO_PLATFORM_WINDOWS
            SYSTEM_INFO SystemInfo;
            GetSystemInfo(&SystemInfo);
            Constants.PageSize = SystemInfo.dwPageSize;
            Constants.OsAllocationGranularity = SystemInfo.dwAllocationGranularity;

            MEMORYSTATUSEX MemoryStatus;
            MemoryStatus.dwLength = sizeof(MemoryStatus);
            if (GlobalMemoryStatusEx(&MemoryStatus))
            {
                Constants.TotalPhysical = MemoryStatus.ullTotalPhys;
                Constants.TotalVirtual = MemoryStatus.ullTotalVirtual;
                Constants.TotalPhysicalGB = static_cast<u32>((Constants.TotalPhysical + 1024 * 1024 * 1024 - 1) / (1024 * 1024 * 1024));
            }
#else
            // Default values for other platforms
            Constants.PageSize = 4096;
            Constants.OsAllocationGranularity = 65536;
            Constants.TotalPhysical = 0;
            Constants.TotalVirtual = 0;
            Constants.TotalPhysicalGB = 1;
#endif
            Constants.BinnedPageSize = 65536;
            Constants.BinnedAllocationGranularity = 0;
            Constants.AddressStart = 0;
            Constants.AddressLimit = static_cast<u64>(0xffffffff) + 1;

            bInitialized = true;
        }

        return Constants;
    }

    u32 FGenericPlatformMemory::GetPhysicalGBRam()
    {
        return FPlatformMemory::GetConstants().TotalPhysicalGB;
    }

    void* FGenericPlatformMemory::BinnedAllocFromOS(sizet Size)
    {
#ifdef OLO_PLATFORM_WINDOWS
        return VirtualAlloc(nullptr, Size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
        void* Ptr = nullptr;
        if (posix_memalign(&Ptr, 65536, Size) != 0)
        {
            return nullptr;
        }
        return Ptr;
#endif
    }

    void FGenericPlatformMemory::BinnedFreeToOS(void* Ptr, sizet Size)
    {
        (void)Size;
#ifdef OLO_PLATFORM_WINDOWS
        VirtualFree(Ptr, 0, MEM_RELEASE);
#else
        std::free(Ptr);
#endif
    }

    bool FGenericPlatformMemory::PageProtect(void* const Ptr, const sizet Size, const bool bCanRead, const bool bCanWrite)
    {
#ifdef OLO_PLATFORM_WINDOWS
        DWORD NewProtect = PAGE_NOACCESS;
        if (bCanRead && bCanWrite)
        {
            NewProtect = PAGE_READWRITE;
        }
        else if (bCanRead)
        {
            NewProtect = PAGE_READONLY;
        }
        else if (bCanWrite)
        {
            NewProtect = PAGE_READWRITE; // Write-only not supported, use read-write
        }

        DWORD OldProtect;
        return VirtualProtect(Ptr, Size, NewProtect, &OldProtect) != 0;
#else
        (void)Ptr;
        (void)Size;
        (void)bCanRead;
        (void)bCanWrite;
        OLO_CORE_WARN("FGenericPlatformMemory::PageProtect not implemented on this platform");
        return false;
#endif
    }

    void FGenericPlatformMemory::DumpStats(FOutputDevice& Ar)
    {
        (void)Ar;
        const float InvMB = 1.0f / 1024.0f / 1024.0f;
        FPlatformMemoryStats MemoryStats = FPlatformMemory::GetStats();

        OLO_CORE_INFO("Platform Memory Stats:");
        OLO_CORE_INFO("Process Physical Memory: {:.2f} MB used, {:.2f} MB peak",
                      static_cast<float>(MemoryStats.UsedPhysical) * InvMB,
                      static_cast<float>(MemoryStats.PeakUsedPhysical) * InvMB);
        OLO_CORE_INFO("Process Virtual Memory: {:.2f} MB used, {:.2f} MB peak",
                      static_cast<float>(MemoryStats.UsedVirtual) * InvMB,
                      static_cast<float>(MemoryStats.PeakUsedVirtual) * InvMB);
        OLO_CORE_INFO("Physical Memory: {:.2f} MB used, {:.2f} MB free, {:.2f} MB total",
                      static_cast<float>(MemoryStats.TotalPhysical - MemoryStats.AvailablePhysical) * InvMB,
                      static_cast<float>(MemoryStats.AvailablePhysical) * InvMB,
                      static_cast<float>(MemoryStats.TotalPhysical) * InvMB);
        OLO_CORE_INFO("Virtual Memory: {:.2f} MB used, {:.2f} MB free, {:.2f} MB total",
                      static_cast<float>(MemoryStats.TotalVirtual - MemoryStats.AvailableVirtual) * InvMB,
                      static_cast<float>(MemoryStats.AvailableVirtual) * InvMB,
                      static_cast<float>(MemoryStats.TotalVirtual) * InvMB);
    }

    void FGenericPlatformMemory::DumpPlatformAndAllocatorStats(FOutputDevice& Ar)
    {
        FPlatformMemory::DumpStats(Ar);
        if (GMalloc)
        {
            GMalloc->DumpAllocatorStats(Ar);
        }
    }

    void FGenericPlatformMemory::OnOutOfMemory(u64 Size, u32 Alignment)
    {
        // Update memory stats before we enter the crash handler.
        OOMAllocationSize = Size;
        OOMAllocationAlignment = Alignment;
        bIsOOM = true;

        FPlatformMemoryStats PlatformMemoryStats = FPlatformMemory::GetStats();

        if (BackupOOMMemoryPool)
        {
            const u32 BackupPoolSize = FPlatformMemory::GetBackMemoryPoolSize();
            FPlatformMemory::BinnedFreeToOS(BackupOOMMemoryPool, BackupPoolSize);
            BackupOOMMemoryPool = nullptr;
            OLO_CORE_WARN("Freeing {} bytes ({:.1f} MiB) from backup pool to handle out of memory.",
                          BackupPoolSize, static_cast<double>(BackupPoolSize) / (1024 * 1024));
        }

        OLO_CORE_WARN("MemoryStats:");
        OLO_CORE_WARN("  AvailablePhysical {} ({:.2f} GiB)",
                      PlatformMemoryStats.AvailablePhysical,
                      static_cast<double>(PlatformMemoryStats.AvailablePhysical) / (1024 * 1024 * 1024));
        OLO_CORE_WARN("   AvailableVirtual {} ({:.2f} GiB)",
                      PlatformMemoryStats.AvailableVirtual,
                      static_cast<double>(PlatformMemoryStats.AvailableVirtual) / (1024 * 1024 * 1024));
        OLO_CORE_WARN("       UsedPhysical {} ({:.2f} GiB)",
                      PlatformMemoryStats.UsedPhysical,
                      static_cast<double>(PlatformMemoryStats.UsedPhysical) / (1024 * 1024 * 1024));
        OLO_CORE_WARN("   PeakUsedPhysical {} ({:.2f} GiB)",
                      PlatformMemoryStats.PeakUsedPhysical,
                      static_cast<double>(PlatformMemoryStats.PeakUsedPhysical) / (1024 * 1024 * 1024));
        OLO_CORE_WARN("        UsedVirtual {} ({:.2f} GiB)",
                      PlatformMemoryStats.UsedVirtual,
                      static_cast<double>(PlatformMemoryStats.UsedVirtual) / (1024 * 1024 * 1024));
        OLO_CORE_WARN("    PeakUsedVirtual {} ({:.2f} GiB)",
                      PlatformMemoryStats.PeakUsedVirtual,
                      static_cast<double>(PlatformMemoryStats.PeakUsedVirtual) / (1024 * 1024 * 1024));

        OLO_CORE_CRITICAL("Ran out of memory allocating {} ({:.1f} MiB) bytes with alignment {}.",
                          Size, static_cast<double>(Size) / (1024 * 1024), Alignment);

        std::abort();
    }

    EPlatformMemorySizeBucket FGenericPlatformMemory::GetMemorySizeBucket()
    {
        static bool bCalculatedBucket = false;
        static EPlatformMemorySizeBucket Bucket = EPlatformMemorySizeBucket::Default;

        // get bucket one time
        if (!bCalculatedBucket)
        {
            bCalculatedBucket = true;

            // Default thresholds (would be config-driven in full implementation)
            i32 LargestMemoryGB = 32;
            i32 LargerMemoryGB = 16;
            i32 DefaultMemoryGB = 8;
            i32 SmallerMemoryGB = 4;
            i32 SmallestMemoryGB = 2;
            i32 TiniestMemoryGB = 1;

            FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
            u32 TotalPhysicalGB = static_cast<u32>((Stats.TotalPhysical + 1024 * 1024 * 1024 - 1) / 1024 / 1024 / 1024);
            u32 AddressLimitGB = static_cast<u32>((Stats.AddressLimit + 1024 * 1024 * 1024 - 1) / 1024 / 1024 / 1024);
            i32 CurMemoryGB = static_cast<i32>(std::min(TotalPhysicalGB, AddressLimitGB));

            // if at least Smaller is specified, we can set the Bucket
            if (SmallerMemoryGB > 0)
            {
                if (CurMemoryGB >= SmallerMemoryGB)
                {
                    Bucket = EPlatformMemorySizeBucket::Smaller;
                }
                else if (CurMemoryGB >= SmallestMemoryGB)
                {
                    Bucket = EPlatformMemorySizeBucket::Smallest;
                }
                else
                {
                    Bucket = EPlatformMemorySizeBucket::Tiniest;
                }
            }
            if (DefaultMemoryGB > 0 && CurMemoryGB >= DefaultMemoryGB)
            {
                Bucket = EPlatformMemorySizeBucket::Default;
            }
            if (LargerMemoryGB > 0 && CurMemoryGB >= LargerMemoryGB)
            {
                Bucket = EPlatformMemorySizeBucket::Larger;
            }
            if (LargestMemoryGB > 0 && CurMemoryGB >= LargestMemoryGB)
            {
                Bucket = EPlatformMemorySizeBucket::Largest;
            }

            OLO_CORE_INFO("Platform has ~ {} GB [{} / {} / {}], which maps to {} [LargestMinGB={}, LargerMinGB={}, DefaultMinGB={}, SmallerMinGB={}, SmallestMinGB={})",
                          CurMemoryGB, Stats.TotalPhysical, Stats.AddressLimit, Stats.TotalPhysicalGB, LexToString(Bucket),
                          LargestMemoryGB, LargerMemoryGB, DefaultMemoryGB, SmallerMemoryGB, SmallestMemoryGB);
        }

        return Bucket;
    }

    void FGenericPlatformMemory::MemswapGreaterThan8(void* Ptr1, void* Ptr2, sizet Size)
    {
        union PtrUnion
        {
            void* PtrVoid;
            u8* Ptr8;
            u16* Ptr16;
            u32* Ptr32;
            u64* Ptr64;
            uptr PtrUint;
        };

        PtrUnion Union1 = { Ptr1 };
        PtrUnion Union2 = { Ptr2 };

        OLO_CORE_ASSERT(Union1.PtrVoid && Union2.PtrVoid, "Pointers must be non-null: {}, {}", Union1.PtrVoid, Union2.PtrVoid);

        // We may skip up to 7 bytes below, so better make sure that we're swapping more than that
        // (8 is a common case that we also want to inline before we this call, so skip that too)
        OLO_CORE_ASSERT(Size > 8, "Size must be > 8");

        if (Union1.PtrUint & 1)
        {
            Valswap(*Union1.Ptr8++, *Union2.Ptr8++);
            Size -= 1;
        }
        if (Union1.PtrUint & 2)
        {
            Valswap(*Union1.Ptr16++, *Union2.Ptr16++);
            Size -= 2;
        }
        if (Union1.PtrUint & 4)
        {
            Valswap(*Union1.Ptr32++, *Union2.Ptr32++);
            Size -= 4;
        }

        // Count trailing zeros to find common alignment
        u32 CommonAlignment = 3u; // Default to 8-byte alignment
        uptr AlignmentDiff = Union1.PtrUint - Union2.PtrUint;
        if (AlignmentDiff != 0)
        {
            u32 TrailingZeros = 0;
            while ((AlignmentDiff & 1) == 0 && TrailingZeros < 3)
            {
                AlignmentDiff >>= 1;
                ++TrailingZeros;
            }
            CommonAlignment = std::min(TrailingZeros, 3u);
        }

        switch (CommonAlignment)
        {
            default:
                for (; Size >= 8; Size -= 8)
                {
                    Valswap(*Union1.Ptr64++, *Union2.Ptr64++);
                }
                [[fallthrough]];

            case 2:
                for (; Size >= 4; Size -= 4)
                {
                    Valswap(*Union1.Ptr32++, *Union2.Ptr32++);
                }
                [[fallthrough]];

            case 1:
                for (; Size >= 2; Size -= 2)
                {
                    Valswap(*Union1.Ptr16++, *Union2.Ptr16++);
                }
                [[fallthrough]];

            case 0:
                for (; Size >= 1; Size -= 1)
                {
                    Valswap(*Union1.Ptr8++, *Union2.Ptr8++);
                }
        }
    }

    FGenericPlatformMemory::FSharedMemoryRegion* FGenericPlatformMemory::MapNamedSharedMemoryRegion(const std::string& Name, bool bCreate, u32 AccessMode, sizet Size)
    {
        (void)Name;
        (void)bCreate;
        (void)AccessMode;
        (void)Size;
        OLO_CORE_ERROR("FGenericPlatformMemory::MapNamedSharedMemoryRegion not implemented on this platform");
        return nullptr;
    }

    bool FGenericPlatformMemory::UnmapNamedSharedMemoryRegion(FSharedMemoryRegion* MemoryRegion)
    {
        (void)MemoryRegion;
        OLO_CORE_ERROR("FGenericPlatformMemory::UnmapNamedSharedMemoryRegion not implemented on this platform");
        return false;
    }

    void FGenericPlatformMemory::InternalUpdateStats(const FPlatformMemoryStats& MemoryStats)
    {
        (void)MemoryStats;
        // Generic method is empty. Implement at platform level.
    }

    bool FGenericPlatformMemory::IsExtraDevelopmentMemoryAvailable()
    {
        return false;
    }

    u64 FGenericPlatformMemory::GetExtraDevelopmentMemorySize()
    {
        return 0;
    }

    bool FGenericPlatformMemory::GetLLMAllocFunctions(void* (*&OutAllocFunction)(sizet), void (*&OutFreeFunction)(void*, sizet), i32& OutAlignment)
    {
        (void)OutAllocFunction;
        (void)OutFreeFunction;
        (void)OutAlignment;
        return false;
    }

    std::string FGenericPlatformMemory::PrettyMemory(u64 Memory)
    {
        static const char* Units[] = { "B", "KB", "MB", "GB", "TB", "PB", "EB" };

        u64 UnitIndex = 0;
        u64 Remainder = 0;

        while ((Memory > 1024) && (UnitIndex++ < (sizeof(Units) / sizeof(Units[0]) - 1)))
        {
            Remainder = Memory & 1023;
            Memory >>= 10llu;
        }

        // Convert remainder to percentage
        const u64 RemainderPerc = (Remainder * 100) >> 10;

        std::ostringstream oss;
        if (RemainderPerc)
        {
            if (RemainderPerc % 10)
            {
                oss << Memory << "." << std::setfill('0') << std::setw(2) << RemainderPerc << Units[UnitIndex];
            }
            else
            {
                oss << Memory << "." << (RemainderPerc / 10) << Units[UnitIndex];
            }
        }
        else
        {
            oss << Memory << Units[UnitIndex];
        }

        return oss.str();
    }

} // namespace OloEngine
