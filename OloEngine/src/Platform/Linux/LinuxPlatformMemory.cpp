// Linux implementation of PlatformMemoryBackend.

#include "OloEnginePCH.h"
#include "OloEngine/Memory/PlatformMemoryBackend.h"

#ifdef OLO_PLATFORM_LINUX

#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <string>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

namespace OloEngine::PlatformMemoryBackend
{
    namespace
    {
        u64 ReadMemInfoValue(const char* key)
        {
            std::ifstream meminfo("/proc/meminfo");
            std::string line;
            const sizet keyLen = std::strlen(key);
            while (std::getline(meminfo, line))
            {
                if (line.compare(0, keyLen, key) == 0)
                {
                    // Format: "Key: <value> kB"
                    auto colon = line.find(':');
                    if (colon == std::string::npos)
                    {
                        return 0;
                    }
                    u64 kb = 0;
                    try
                    {
                        kb = std::stoull(line.substr(colon + 1));
                    }
                    catch (...)
                    {
                        return 0;
                    }
                    return kb * 1024;
                }
            }
            return 0;
        }
    } // namespace

    bool QueryStats(BackendStats& outStats)
    {
        outStats.TotalPhysical = ReadMemInfoValue("MemTotal");
        outStats.AvailablePhysical = ReadMemInfoValue("MemAvailable");

        const long pageSize = sysconf(_SC_PAGESIZE);
        const long pages = sysconf(_SC_PHYS_PAGES);
        if (outStats.TotalPhysical == 0 && pageSize > 0 && pages > 0)
        {
            outStats.TotalPhysical = static_cast<u64>(pageSize) * static_cast<u64>(pages);
        }

        // Process resident set size via /proc/self/statm
        std::ifstream statm("/proc/self/statm");
        if (statm.is_open())
        {
            u64 sizePages = 0, rssPages = 0;
            statm >> sizePages >> rssPages;
            if (pageSize > 0)
            {
                outStats.UsedVirtual = sizePages * static_cast<u64>(pageSize);
                outStats.UsedPhysical = rssPages * static_cast<u64>(pageSize);
            }
        }

        rusage ru{};
        if (getrusage(RUSAGE_SELF, &ru) == 0)
        {
            // ru_maxrss is in kilobytes on Linux
            outStats.PeakUsedPhysical = static_cast<u64>(ru.ru_maxrss) * 1024;
        }

        return outStats.TotalPhysical != 0;
    }

    void QueryConstants(BackendConstants& outConstants)
    {
        const long pageSize = sysconf(_SC_PAGESIZE);
        if (pageSize > 0)
        {
            outConstants.PageSize = static_cast<u32>(pageSize);
        }
        outConstants.OsAllocationGranularity = 65536;

        outConstants.TotalPhysical = ReadMemInfoValue("MemTotal");
        const long pages = sysconf(_SC_PHYS_PAGES);
        if (outConstants.TotalPhysical == 0 && pageSize > 0 && pages > 0)
        {
            outConstants.TotalPhysical = static_cast<u64>(pageSize) * static_cast<u64>(pages);
        }
        // Virtual memory limit isn't directly queryable; leave as 0.
    }

    void* AllocateFromOS(sizet size)
    {
        void* ptr = nullptr;
        if (posix_memalign(&ptr, 65536, size) != 0)
        {
            return nullptr;
        }
        return ptr;
    }

    void FreeToOS(void* ptr, sizet /*size*/)
    {
        std::free(ptr);
    }

    bool PageProtect(void* ptr, sizet size, bool canRead, bool canWrite)
    {
        int prot = PROT_NONE;
        if (canRead)
            prot |= PROT_READ;
        if (canWrite)
            prot |= PROT_WRITE;
        return mprotect(ptr, size, prot) == 0;
    }

} // namespace OloEngine::PlatformMemoryBackend

#endif // OLO_PLATFORM_LINUX
