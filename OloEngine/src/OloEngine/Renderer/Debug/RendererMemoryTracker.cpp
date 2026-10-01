#include "OloEnginePCH.h"
#include "RendererMemoryTracker.h"
#include "DebugUtils.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Threading/UniqueLock.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <chrono>
#include <cmath>

namespace OloEngine
{
    namespace
    {
        constexpr std::string_view kUnattributedOwner = "Unattributed";

        // Owner scopes nest per thread. 32 is far deeper than any real nesting (pass ->
        // pool -> resource); overflowing it is a missing scope pop, asserted below.
        constexpr u32 kMaxOwnerScopeDepth = 32;
        thread_local std::array<RendererMemoryOwnerScope::Frame, kMaxOwnerScopeDepth> t_OwnerStack{};
        thread_local u32 t_OwnerDepth = 0;
    } // namespace

    RendererMemoryOwnerScope::RendererMemoryOwnerScope(const std::string_view owner, const MemoryLifetime lifetime)
    {
        OLO_CORE_ASSERT(t_OwnerDepth < kMaxOwnerScopeDepth, "RendererMemoryOwnerScope nested too deeply — a scope was never popped");
        if (t_OwnerDepth < kMaxOwnerScopeDepth)
        {
            t_OwnerStack[t_OwnerDepth] = Frame{ owner, lifetime };
            ++t_OwnerDepth;
            m_Pushed = true;
        }
    }

    RendererMemoryOwnerScope::~RendererMemoryOwnerScope()
    {
        if (m_Pushed && t_OwnerDepth > 0)
        {
            --t_OwnerDepth;
        }
    }

    RendererMemoryOwnerScope::Frame RendererMemoryOwnerScope::Current()
    {
        return t_OwnerDepth > 0 ? t_OwnerStack[t_OwnerDepth - 1] : Frame{};
    }

    RendererMemoryReporterHandle::RendererMemoryReporterHandle(MemoryCapacityReporter reporter)
        : m_Handle(RendererMemoryTracker::GetInstance().RegisterCapacityReporter(std::move(reporter)))
    {
    }

    RendererMemoryReporterHandle::~RendererMemoryReporterHandle()
    {
        Reset();
    }

    RendererMemoryReporterHandle::RendererMemoryReporterHandle(RendererMemoryReporterHandle&& other) noexcept
        : m_Handle(std::exchange(other.m_Handle, 0))
    {
    }

    RendererMemoryReporterHandle& RendererMemoryReporterHandle::operator=(RendererMemoryReporterHandle&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_Handle = std::exchange(other.m_Handle, 0);
        }
        return *this;
    }

    void RendererMemoryReporterHandle::Reset()
    {
        if (m_Handle != 0)
        {
            RendererMemoryTracker::GetInstance().UnregisterCapacityReporter(m_Handle);
            m_Handle = 0;
        }
    }

    RendererMemoryTracker& RendererMemoryTracker::GetInstance()
    {
        // Deliberately leaked (never destroyed) — issue #1088's shape. Every
        // OpenGL resource destructor calls OLO_TRACK_DEALLOC, which lands here,
        // and those destructors run during STATIC DESTRUCTION for anything the
        // namespace-scope ShaderLibrary statics still own. A lazily-created
        // Meyers singleton is registered for destruction later than they are
        // and so is torn down first.
        //
        // Leaking also keeps the teardown REPORT trustworthy: Shutdown() is
        // what enumerates surviving allocations, and it is an explicit call,
        // not the destructor (which is `= default`).
        static auto* s_Instance = new RendererMemoryTracker();
        return *s_Instance;
    }

    void RendererMemoryTracker::Initialize()
    {
        OLO_PROFILE_FUNCTION();

        // Prevent double initialization
        if (m_IsInitialized.load())
        {
            OLO_CORE_WARN("RendererMemoryTracker: Already initialized, skipping re-initialization");
            return;
        }

        TUniqueLock<FMutex> lock(m_Mutex);

        // Double-check after acquiring lock
        if (m_IsInitialized.load())
        {
            OLO_CORE_WARN("RendererMemoryTracker: Already initialized (double check), skipping re-initialization");
            return;
        }

        // Initialize history arrays
        m_MemoryHistory.Init(0.0f, OLO_HISTORY_SIZE);
        m_AllocationHistory.Init(0.0f, OLO_HISTORY_SIZE);
        m_GPUMemoryHistory.Init(0.0f, OLO_HISTORY_SIZE);
        m_CPUMemoryHistory.Init(0.0f, OLO_HISTORY_SIZE);
        m_TypeUsage.fill(0);
        m_TypeCounts.fill(0);
        if (m_OwnerNames.IsEmpty())
        {
            InternOwnerUnlocked(kUnattributedOwner);
        }

        m_LastUpdateTime = std::chrono::duration<f64>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();

        // Clear the shutdown latch. Shutdown() sets it and nothing used to clear it, so after
        // the first Renderer::Shutdown() the tracker was permanently half-dead: TrackDeallocation
        // early-returns on m_IsShutdown while TrackAllocation keeps recording, which turns EVERY
        // resource type into a phantom leaker across an Init/Shutdown/Init cycle (the test binary
        // and the editor's project-reload both do exactly that).
        m_IsShutdown = false;

        // Set initialization flag
        m_IsInitialized.store(true);

        OLO_CORE_INFO("Renderer Memory Tracker initialized");
    }

    void RendererMemoryTracker::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        m_IsShutdown = true; // Set shutdown flag first

        TUniqueLock<FMutex> lock(m_Mutex);

        // Anything still tracked here outlived the renderer, which is the entire
        // lazy-static-release bug class (#814, #839). This used to clear() the map
        // without looking at it — so the ONE instrument that is backend-independent, and
        // that already knew every survivor's type, size, name and creation site, threw
        // that away at exactly the moment it had the answer. That is why "GL is quiet"
        // read as "GL is clean" for the whole life of the backend while Vulkan aborted at
        // vmaDestroyAllocator over the same objects.
        //
        // Reported at ERROR because it always indicates a real defect: by the time
        // Renderer::Shutdown() reaches here the layers are detached and Project::Unload()
        // has dropped every loaded asset, so a live GPU allocation has no legitimate
        // owner left. Vulkan allocations are booked too since #1342, but VMA-owned
        // survivors are still reported by the #794 teardown forensics at
        // vmaDestroyAllocator; this line covers what exists when the RENDERER shuts down.
        // See docs/agent-rules/lazy-static-release-ownership.md.
        {
            // Deliberately terse: ONE error line with the totals and a per-type breakdown,
            // then a handful of examples at warning level. The first run of this reporter
            // found 115 survivors on OpenGL, and a two-dozen-line dump on every editor exit
            // — and at the end of every test run, since OloEngineTest calls
            // Renderer::Shutdown() — is noise nobody reads. The count and the type tell you
            // whether to care; the examples tell you where to start looking.
            constexpr u32 kMaxSurvivorExamples = 6;
            sizet gpuBytes = 0;
            u32 gpuCount = 0;
            std::array<u32, static_cast<sizet>(std::to_underlying(ResourceType::COUNT))> byType{};
            for (const auto& [address, info] : m_Allocations)
            {
                if (!info.m_IsGPU || info.IsAlias())
                    continue;
                ++gpuCount;
                gpuBytes += info.m_Size;
                ++byType[static_cast<sizet>(std::to_underlying(info.m_Type))];
            }
            if (gpuCount > 0)
            {
                std::string breakdown;
                for (u32 i = 0; i < static_cast<u32>(std::to_underlying(ResourceType::COUNT)); ++i)
                {
                    if (byType[i] == 0)
                        continue;
                    if (!breakdown.empty())
                        breakdown += ", ";
                    breakdown += std::to_string(byType[i]) + " " + GetResourceTypeName(static_cast<ResourceType>(i));
                }
                OLO_CORE_ERROR("[Teardown] {} GPU allocation(s) ({} bytes) survived the renderer — {}. Each is a "
                               "process static, or a Ref that outlived Renderer::Shutdown(). See "
                               "docs/agent-rules/lazy-static-release-ownership.md",
                               gpuCount, gpuBytes, breakdown);
                u32 listed = 0;
                for (const auto& [address, info] : m_Allocations)
                {
                    if (!info.m_IsGPU || info.IsAlias() || listed == kMaxSurvivorExamples)
                        continue;
                    ++listed;
                    OLO_CORE_WARN("[Teardown]   e.g. {} '{}' ({} bytes) created at {}:{}",
                                  GetResourceTypeName(info.m_Type), info.m_Name.ToView(), info.m_Size,
                                  info.m_File.ToView(), info.m_Line);
                }
            }
        }

        m_Allocations.clear();
        m_Retiring.clear();
        m_BackingByHandle.clear();
        m_ViewTokens.clear();
        m_TypeUsage.fill(0);
        m_TypeCounts.fill(0);
        m_GpuLiveBytes = 0;
        m_GpuRetiringBytes = 0;
        m_CpuLiveBytes = 0;
        m_CpuRetiringBytes = 0;

        // Reset initialization flag
        m_IsInitialized.store(false);

        OLO_CORE_INFO("Renderer Memory Tracker shutdown");
    }

    void RendererMemoryTracker::Reset()
    {
        OLO_PROFILE_FUNCTION();

        TUniqueLock<FMutex> lock(m_Mutex);
        // Clear all tracking data
        m_Allocations.clear();
        m_Retiring.clear();
        m_BackingByHandle.clear();
        m_ViewTokens.clear();
        m_TypeUsage.fill(0);
        m_TypeCounts.fill(0);

        // Reset statistics
        m_GpuLiveBytes = 0;
        m_GpuRetiringBytes = 0;
        m_CpuLiveBytes = 0;
        m_CpuRetiringBytes = 0;
        m_PeakGpuResidentBytes = 0;
        m_WindowPeakGpuResidentBytes = 0;
        m_PeakCpuResidentBytes = 0;
        m_WindowPeakCpuResidentBytes = 0;
        m_PeakMemoryUsage = 0;
        m_TotalAllocations = 0;
        m_TotalDeallocations = 0;

        // Reset history arrays
        std::ranges::fill(m_MemoryHistory, 0.0f);
        std::ranges::fill(m_AllocationHistory, 0.0f);
        std::ranges::fill(m_GPUMemoryHistory, 0.0f);
        std::ranges::fill(m_CPUMemoryHistory, 0.0f);

        m_HistoryIndex = 0;
        m_LastUpdateTime = DebugUtils::GetCurrentTimeSeconds();

        // Reset initialization flag so Initialize() can be called again, and clear the shutdown
        // latch with it — a Reset() that left the tracker refusing every deallocation would be
        // worse than useless (see Initialize()).
        m_IsShutdown = false;
        m_IsInitialized.store(false);

        OLO_CORE_INFO("Renderer Memory Tracker reset");
    }

    u16 RendererMemoryTracker::InternOwnerUnlocked(const std::string_view owner)
    {
        if (m_OwnerNames.IsEmpty() && owner != kUnattributedOwner)
        {
            // Index 0 is always "Unattributed", even when an owner is booked before Initialize().
            m_OwnerNames.Add(FString(kUnattributedOwner));
            m_OwnerIds.emplace(std::string(kUnattributedOwner), u16{ 0 });
        }
        if (const auto it = m_OwnerIds.find(std::string(owner)); it != m_OwnerIds.end())
        {
            return it->second;
        }
        if (m_OwnerNames.Num() >= std::numeric_limits<u16>::max())
        {
            // 65 535 distinct owner names means owner strings are being minted per object
            // (a pointer in the name, say). Attribute to Unattributed rather than wrap.
            return 0;
        }
        const auto id = static_cast<u16>(m_OwnerNames.Num());
        m_OwnerNames.Add(FString(owner));
        m_OwnerIds.emplace(std::string(owner), id);
        return id;
    }

    void RendererMemoryTracker::StampOwnerUnlocked(AllocationInfo& info)
    {
        const auto frame = RendererMemoryOwnerScope::Current();
        if (frame.Owner.empty())
        {
            info.m_OwnerId = InternOwnerUnlocked(kUnattributedOwner);
            info.m_Lifetime = MemoryLifetime::Unattributed;
            return;
        }
        info.m_OwnerId = InternOwnerUnlocked(frame.Owner);
        info.m_Lifetime = frame.Lifetime;
    }

    void RendererMemoryTracker::AddPhysicalUnlocked(const AllocationInfo& info, const bool retiring)
    {
        if (info.IsAlias())
            return;
        u64& bucket = info.m_IsGPU ? (retiring ? m_GpuRetiringBytes : m_GpuLiveBytes)
                                   : (retiring ? m_CpuRetiringBytes : m_CpuLiveBytes);
        bucket += info.m_Size;
        if (!retiring)
        {
            const auto type = static_cast<sizet>(std::to_underlying(info.m_Type));
            m_TypeUsage[type] += info.m_Size;
            ++m_TypeCounts[type];
        }
    }

    void RendererMemoryTracker::RemovePhysicalUnlocked(const AllocationInfo& info, const bool retiring)
    {
        if (info.IsAlias())
            return;
        u64& bucket = info.m_IsGPU ? (retiring ? m_GpuRetiringBytes : m_GpuLiveBytes)
                                   : (retiring ? m_CpuRetiringBytes : m_CpuLiveBytes);
        bucket -= std::min<u64>(bucket, info.m_Size);
        if (!retiring)
        {
            const auto type = static_cast<sizet>(std::to_underlying(info.m_Type));
            m_TypeUsage[type] -= std::min(m_TypeUsage[type], info.m_Size);
            if (m_TypeCounts[type] > 0)
            {
                --m_TypeCounts[type];
            }
        }
    }

    void RendererMemoryTracker::NotePeakUnlocked()
    {
        const u64 gpuResident = m_GpuLiveBytes + m_GpuRetiringBytes;
        const u64 cpuResident = m_CpuLiveBytes + m_CpuRetiringBytes;
        m_PeakGpuResidentBytes = std::max(m_PeakGpuResidentBytes, gpuResident);
        m_WindowPeakGpuResidentBytes = std::max(m_WindowPeakGpuResidentBytes, gpuResident);
        m_PeakCpuResidentBytes = std::max(m_PeakCpuResidentBytes, cpuResident);
        m_WindowPeakCpuResidentBytes = std::max(m_WindowPeakCpuResidentBytes, cpuResident);
        m_PeakMemoryUsage = std::max<sizet>(m_PeakMemoryUsage, GetTotalMemoryUsageUnlocked());
    }

    void RendererMemoryTracker::InsertBackingUnlocked(AllocationInfo info)
    {
        // A live entry already sitting at this address means the previous owner was destroyed
        // WITHOUT untracking (the map is keyed on the CPU heap address, so the allocator
        // handing the block straight back to a same-size-class object is the normal case, not
        // a rare one). Retire the corpse's size/count before overwriting it: the old code just
        // clobbered the entry, so its bytes were added again and never subtracted, and the
        // per-type totals in the Statistics panel drifted upward for the rest of the session.
        //
        // The warning names the leaker's OWN file:line — that is the constructor whose
        // destructor is missing its OLO_TRACK_DEALLOC, which is the actual thing to go fix.
        if (const auto stale = m_Allocations.find(info.m_Address); stale != m_Allocations.end())
        {
            const AllocationInfo& old = stale->second;
            OLO_CORE_WARN("Double allocation at address {0}: '{1}' ({2} bytes, tracked at {3}:{4}) was never "
                          "untracked before '{5}' reused the address — its destructor is missing OLO_TRACK_DEALLOC",
                          info.m_Address, old.m_Name.ToView(), old.m_Size, old.m_File.ToView(), old.m_Line,
                          info.m_Name.ToView());
            RemovePhysicalUnlocked(old, false);
            ForgetHandleUnlocked(old);
        }

        AddPhysicalUnlocked(info, false);
        void* const address = info.m_Address;
        m_Allocations[address] = std::move(info);
        ++m_TotalAllocations;
        NotePeakUnlocked();
    }

    void RendererMemoryTracker::TrackAllocation(void* address, sizet size, ResourceType type,
                                                const std::string& name, bool isGPU,
                                                const char* file, u32 line)
    {
        AllocationDesc desc;
        desc.Address = address;
        desc.Size = size;
        desc.Type = type;
        desc.Name = name;
        desc.IsGPU = isGPU;
        desc.Backend = MemoryBackend::OpenGL;
        desc.SizeSource = MemorySizeSource::FormatEstimate;
        desc.File = file;
        desc.Line = line;
        TrackAllocation(desc);
    }

    void RendererMemoryTracker::TrackAllocation(const AllocationDesc& desc)
    {
        if (!desc.Address || desc.Size == 0)
        {
            OLO_CORE_WARN("RendererMemoryTracker: Invalid allocation - address={}, size={}", desc.Address, desc.Size);
            return;
        }

        TUniqueLock<FMutex> lock(m_Mutex);

        AllocationInfo info;
        info.m_Address = desc.Address;
        info.m_Size = desc.Size;
        info.m_Type = desc.Type;
        info.m_Name = FString(desc.Name);
        info.m_File = desc.File ? desc.File : "Unknown";
        info.m_Line = desc.Line;
        info.m_Timestamp = DebugUtils::GetCurrentTimeSeconds();
        info.m_IsGPU = desc.IsGPU;
        info.m_Backend = desc.Backend;
        info.m_SizeSource = desc.SizeSource;
        StampOwnerUnlocked(info);
        InsertBackingUnlocked(std::move(info));
    }

    void RendererMemoryTracker::TrackAlias(void* aliasAddress, void* backingAddress, sizet logicalBytes,
                                           ResourceType type, std::string_view name, const char* file, u32 line)
    {
        if (!aliasAddress || !backingAddress || aliasAddress == backingAddress)
        {
            OLO_CORE_WARN("RendererMemoryTracker: Invalid alias - alias={}, backing={}", aliasAddress, backingAddress);
            return;
        }

        TUniqueLock<FMutex> lock(m_Mutex);
        TrackAliasUnlocked(aliasAddress, backingAddress, logicalBytes, type, name, file, line);
    }

    void RendererMemoryTracker::TrackAliasUnlocked(void* aliasAddress, void* backingAddress, sizet logicalBytes,
                                                   ResourceType type, std::string_view name, const char* file, u32 line)
    {
        AllocationInfo info;
        info.m_Address = aliasAddress;
        info.m_Size = logicalBytes;
        info.m_Type = type;
        info.m_Name = FString(name);
        info.m_File = file ? file : "Unknown";
        info.m_Line = line;
        info.m_Timestamp = DebugUtils::GetCurrentTimeSeconds();
        info.m_IsGPU = true;
        if (const auto backing = m_Allocations.find(backingAddress); backing != m_Allocations.end())
        {
            info.m_Backend = backing->second.m_Backend;
            info.m_SizeSource = backing->second.m_SizeSource;
            info.m_IsGPU = backing->second.m_IsGPU;
        }
        StampOwnerUnlocked(info);

        if (Levers::FaultCountAliasAsBacking())
        {
            // THE #1342 NEGATIVE CONTROL: book the view as its own backing allocation. Every
            // physical total and the backend reconciliation must now disagree with reality.
            if (info.m_Size == 0)
                return;
            InsertBackingUnlocked(std::move(info));
            return;
        }

        info.m_BackingAddress = backingAddress;
        if (const auto stale = m_Allocations.find(aliasAddress); stale != m_Allocations.end())
        {
            RemovePhysicalUnlocked(stale->second, false);
        }
        m_Allocations[aliasAddress] = std::move(info);
    }

    void RendererMemoryTracker::RenameAllocation(void* address, const std::string_view name)
    {
        if (!address)
            return;
        TUniqueLock<FMutex> lock(m_Mutex);
        if (const auto it = m_Allocations.find(address); it != m_Allocations.end())
        {
            it->second.m_Name = FString(name);
        }
    }

    void RendererMemoryTracker::ForgetHandleUnlocked(const AllocationInfo& info)
    {
        if (info.m_HandleKey == 0)
            return;
        if (const auto it = m_BackingByHandle.find(info.m_HandleKey); it != m_BackingByHandle.end() && it->second == info.m_Address)
        {
            m_BackingByHandle.erase(it);
        }
    }

    void RendererMemoryTracker::BindResourceHandle(void* address, const u64 handleKey)
    {
        if (!address || handleKey == 0)
            return;
        TUniqueLock<FMutex> lock(m_Mutex);
        const auto it = m_Allocations.find(address);
        if (it == m_Allocations.end() || it->second.IsAlias())
            return;
        it->second.m_HandleKey = handleKey;
        m_BackingByHandle[handleKey] = address;
    }

    void RendererMemoryTracker::TrackAliasOfHandle(const u64 aliasHandleKey, const u64 backingHandleKey, const ResourceType type,
                                                   const std::string_view name, const char* file, const u32 line)
    {
        if (aliasHandleKey == 0 || m_IsShutdown)
            return;
        TUniqueLock<FMutex> lock(m_Mutex);

        auto [token, inserted] = m_ViewTokens.try_emplace(aliasHandleKey, u8{ 0 });
        void* const aliasAddress = &token->second;
        if (!inserted)
        {
            // The same view handle booked twice: replace, never stack.
            if (const auto stale = m_Allocations.find(aliasAddress); stale != m_Allocations.end())
            {
                RemovePhysicalUnlocked(stale->second, false);
                m_Allocations.erase(stale);
            }
        }

        const auto backing = m_BackingByHandle.find(backingHandleKey);
        if (backing == m_BackingByHandle.end())
        {
            // No bound backing: still booked, as an ORPHAN alias (its backing address is the
            // token map itself, which is never an entry). The report counts orphans, so a
            // view of an untracked resource is a visible hole rather than a silent zero.
            TrackAliasUnlocked(aliasAddress, &m_ViewTokens, 0, type, name, file, line);
            return;
        }
        const auto backingEntry = m_Allocations.find(backing->second);
        const sizet logicalBytes = backingEntry != m_Allocations.end() ? backingEntry->second.m_Size : 0;
        TrackAliasUnlocked(aliasAddress, backing->second, logicalBytes, type, name, file, line);
    }

    void RendererMemoryTracker::UntrackAliasOfHandle(const u64 aliasHandleKey)
    {
        if (aliasHandleKey == 0 || m_IsShutdown)
            return;
        TUniqueLock<FMutex> lock(m_Mutex);
        const auto token = m_ViewTokens.find(aliasHandleKey);
        if (token == m_ViewTokens.end())
            return;
        if (const auto it = m_Allocations.find(&token->second); it != m_Allocations.end())
        {
            // Normally an alias (no physical bytes). Under FaultCountAliasAsBacking it was
            // booked as backing, and its planted bytes leave with it.
            RemovePhysicalUnlocked(it->second, false);
            m_Allocations.erase(it);
        }
        m_ViewTokens.erase(token);
    }

    namespace RendererMemory
    {
        void BindBackingResourceHandle(void* backingAddress, const u64 handleKey)
        {
            RendererMemoryTracker::GetInstance().BindResourceHandle(backingAddress, handleKey);
        }

        void TrackResourceView(const u64 viewHandleKey, const u64 sourceHandleKey, const std::string_view name)
        {
            RendererMemoryTracker::GetInstance().TrackAliasOfHandle(viewHandleKey, sourceHandleKey,
                                                                    RendererMemoryTracker::ResourceType::Texture2D, name,
                                                                    __FILE__, __LINE__);
        }

        void UntrackResourceView(const u64 viewHandleKey)
        {
            RendererMemoryTracker::GetInstance().UntrackAliasOfHandle(viewHandleKey);
        }
    } // namespace RendererMemory

    void RendererMemoryTracker::TrackDeallocation(void* address, const char* file, u32 line)
    {
        if (!address || m_IsShutdown)
            return;

        // A blocking lock. This used to be TryLock(), which DROPPED the deallocation — with only a
        // warning — whenever another thread happened to hold the mutex (a UI tab, the MCP report,
        // an allocation on a loader thread). The bytes then stayed booked forever: a leak the
        // tracker invented under ordinary contention. Nothing in this class calls back into a
        // resource destructor while holding the mutex, so there is no deadlock to avoid.
        TUniqueLock<FMutex> lock(m_Mutex);

        // Double-check shutdown state after acquiring lock
        if (m_IsShutdown)
            return;

        const auto it = m_Allocations.find(address);
        if (it == m_Allocations.end())
        {
            OLO_CORE_WARN("Attempted to deallocate untracked memory at address {0} (from {1}:{2})", address, file, line);
            return;
        }
        RemovePhysicalUnlocked(it->second, false);
        ForgetHandleUnlocked(it->second);
        if (!it->second.IsAlias())
        {
            ++m_TotalDeallocations;
        }
        m_Allocations.erase(it);
    }

    u64 RendererMemoryTracker::RetireAllocation(void* address)
    {
        if (!address || m_IsShutdown)
            return 0;

        TUniqueLock<FMutex> lock(m_Mutex);
        if (m_IsShutdown)
            return 0;

        const auto it = m_Allocations.find(address);
        if (it == m_Allocations.end())
            return 0;

        if (it->second.IsAlias())
        {
            // A view owns no backing, so there is nothing to wait for.
            m_Allocations.erase(it);
            return 0;
        }

        RemovePhysicalUnlocked(it->second, false);
        AddPhysicalUnlocked(it->second, true);
        // The owner released it: nothing new may be made a view of it (#1342).
        ForgetHandleUnlocked(it->second);
        const u64 ticket = m_NextRetireTicket++;
        m_Retiring.emplace(ticket, std::move(it->second));
        m_Allocations.erase(it);
        return ticket;
    }

    void RendererMemoryTracker::ReleaseRetired(const u64 ticket)
    {
        if (ticket == 0 || m_IsShutdown)
            return;

        TUniqueLock<FMutex> lock(m_Mutex);
        if (m_IsShutdown)
            return;

        const auto it = m_Retiring.find(ticket);
        if (it == m_Retiring.end())
        {
            // Reset() or Shutdown() cleared the set while the deletion was queued: the bytes
            // already left every total, so there is nothing to subtract.
            return;
        }
        RemovePhysicalUnlocked(it->second, true);
        ++m_TotalDeallocations;
        m_Retiring.erase(it);
    }

    void RendererMemoryTracker::BeginPeakWindow()
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        m_WindowPeakGpuResidentBytes = m_GpuLiveBytes + m_GpuRetiringBytes;
        m_WindowPeakCpuResidentBytes = m_CpuLiveBytes + m_CpuRetiringBytes;
    }

    u64 RendererMemoryTracker::GetGpuResidentBytes() const
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        return m_GpuLiveBytes + m_GpuRetiringBytes;
    }

    u64 RendererMemoryTracker::GetCpuResidentBytes() const
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        return m_CpuLiveBytes + m_CpuRetiringBytes;
    }

    u64 RendererMemoryTracker::RegisterCapacityReporter(CapacityReporter reporter)
    {
        TUniqueLock<FMutex> lock(m_ReporterMutex);
        const u64 handle = m_NextReporterHandle++;
        m_CapacityReporters.emplace(handle, std::move(reporter));
        return handle;
    }

    void RendererMemoryTracker::UnregisterCapacityReporter(const u64 handle)
    {
        TUniqueLock<FMutex> lock(m_ReporterMutex);
        m_CapacityReporters.erase(handle);
    }

    bool RendererMemoryTracker::ObserveActiveRendererAPI(BackendMemoryObservation& out)
    {
        const RendererAPI* const api = RenderCommand::TryGetRendererAPI();
        return api != nullptr && api->ObserveDeviceMemory(out);
    }

    void RendererMemoryTracker::SetBackendObserver(const BackendObserver observer)
    {
        TUniqueLock<FMutex> lock(m_ReporterMutex);
        m_BackendObserver = observer;
    }

    void RendererMemoryTracker::BeginExternalMutation()
    {
        m_ExternalMutationsInFlight.fetch_add(1, std::memory_order_acq_rel);
        m_ExternalMutationEpoch.fetch_add(1, std::memory_order_acq_rel);
    }

    void RendererMemoryTracker::EndExternalMutation()
    {
        m_ExternalMutationEpoch.fetch_add(1, std::memory_order_acq_rel);
        m_ExternalMutationsInFlight.fetch_sub(1, std::memory_order_acq_rel);
    }

    RendererMemoryReport RendererMemoryTracker::BuildReport() const
    {
        OLO_PROFILE_FUNCTION();

        RendererMemoryReport report;

        BackendObserver observer = nullptr;
        {
            TUniqueLock<FMutex> lock(m_ReporterMutex);
            observer = m_BackendObserver;
        }

        // Read the allocator and the table between two epoch samples. If a bracketed
        // allocation or free overlapped the reads, the two sides describe different instants;
        // retry a few times, then publish Racing rather than a false difference.
        constexpr u32 kMaxAttempts = 4;
        bool racing = true;
        bool observed = false;
        u64 trackedCommitted = 0;
        for (u32 attempt = 0; attempt < kMaxAttempts && racing; ++attempt)
        {
            const u64 epochBefore = m_ExternalMutationEpoch.load(std::memory_order_acquire);
            const bool quietBefore = m_ExternalMutationsInFlight.load(std::memory_order_acquire) == 0;

            report.Observation = BackendMemoryObservation{};
            observed = observer && observer(report.Observation);
            const MemoryBackend observedBackend = observed ? report.Observation.Backend : MemoryBackend::Unknown;

            {
                TUniqueLock<FMutex> lock(m_Mutex);
                report.Gpu = MemoryTotals{};
                report.Cpu = MemoryTotals{};
                report.AliasLogicalBytes = 0;
                report.AliasCount = 0;
                report.OrphanAliasCount = 0;
                trackedCommitted = 0;

                std::map<std::pair<u16, MemoryLifetime>, MemoryOwnerRow> owners;
                const auto book = [&](const AllocationInfo& info, const bool retiring)
                {
                    MemoryTotals& totals = info.m_IsGPU ? report.Gpu : report.Cpu;
                    (retiring ? totals.RetiringBytes : totals.LiveBytes) += info.m_Size;
                    ++(retiring ? totals.RetiringCount : totals.LiveCount);
                    (info.m_SizeSource == MemorySizeSource::Committed ? totals.CommittedBytes : totals.EstimatedBytes) += info.m_Size;
                    if (info.m_SizeSource == MemorySizeSource::Committed && info.m_Backend == observedBackend)
                    {
                        trackedCommitted += info.m_Size;
                    }

                    MemoryOwnerRow& row = owners[{ info.m_OwnerId, info.m_Lifetime }];
                    if (info.m_IsGPU)
                        (retiring ? row.GpuRetiringBytes : row.GpuLiveBytes) += info.m_Size;
                    else if (!retiring)
                        row.CpuLiveBytes += info.m_Size;
                    ++row.AllocationCount;
                    (info.m_SizeSource == MemorySizeSource::Committed ? row.CommittedBytes : row.EstimatedBytes) += info.m_Size;
                };

                for (const auto& [address, info] : m_Allocations)
                {
                    if (info.IsAlias())
                    {
                        report.AliasLogicalBytes += info.m_Size;
                        ++report.AliasCount;
                        const auto backing = m_Allocations.find(info.m_BackingAddress);
                        if (backing == m_Allocations.end() || backing->second.IsAlias())
                        {
                            ++report.OrphanAliasCount;
                        }
                        continue;
                    }
                    book(info, false);
                }
                for (const auto& [ticket, info] : m_Retiring)
                {
                    book(info, true);
                }

                report.Gpu.PeakBytes = m_PeakGpuResidentBytes;
                report.Gpu.WindowPeakBytes = m_WindowPeakGpuResidentBytes;
                report.Cpu.PeakBytes = m_PeakCpuResidentBytes;
                report.Cpu.WindowPeakBytes = m_WindowPeakCpuResidentBytes;

                report.Owners.Reset();
                for (auto& [key, row] : owners)
                {
                    const u16 ownerId = key.first;
                    row.Owner = ownerId < m_OwnerNames.Num() ? m_OwnerNames[ownerId] : FString(kUnattributedOwner);
                    row.Lifetime = key.second;
                    report.Owners.Add(std::move(row));
                }
            }

            const u64 epochAfter = m_ExternalMutationEpoch.load(std::memory_order_acquire);
            const bool quietAfter = m_ExternalMutationsInFlight.load(std::memory_order_acquire) == 0;
            racing = !(quietBefore && quietAfter && epochBefore == epochAfter);
        }

        std::ranges::sort(report.Owners, [](const MemoryOwnerRow& a, const MemoryOwnerRow& b)
                          { return (a.GpuLiveBytes + a.GpuRetiringBytes) > (b.GpuLiveBytes + b.GpuRetiringBytes); });

        report.Reconciliation = ReconcileCommittedBytes(trackedCommitted, observed ? &report.Observation : nullptr, racing);

        {
            TUniqueLock<FMutex> lock(m_ReporterMutex);
            for (const auto& [handle, reporter] : m_CapacityReporters)
            {
                reporter(report.Capacity);
            }
        }
        return report;
    }

    void RendererMemoryTracker::UpdateStats()
    {
        OLO_PROFILE_FUNCTION();
        f64 currentTime = DebugUtils::GetCurrentTimeSeconds();
        if (currentTime - m_LastUpdateTime < m_RefreshInterval)
            return;

        TUniqueLock<FMutex> lock(m_Mutex);
        if (m_MemoryHistory.IsEmpty())
            return;

        m_MemoryHistory[m_HistoryIndex] = static_cast<f32>(GetTotalMemoryUsageUnlocked());
        m_AllocationHistory[m_HistoryIndex] = static_cast<f32>(m_Allocations.size());
        m_GPUMemoryHistory[m_HistoryIndex] = static_cast<f32>(m_GpuLiveBytes + m_GpuRetiringBytes);
        m_CPUMemoryHistory[m_HistoryIndex] = static_cast<f32>(m_CpuLiveBytes + m_CpuRetiringBytes);

        m_HistoryIndex = (m_HistoryIndex + 1) % OLO_HISTORY_SIZE;
        m_LastUpdateTime = currentTime;
    }

    void RendererMemoryTracker::RenderUI(bool* open)
    {
        OLO_PROFILE_FUNCTION();

        if (!open || *open)
        {
            ImGui::Begin("Renderer Memory Tracker", open, ImGuiWindowFlags_MenuBar);

            // Menu bar
            if (ImGui::BeginMenuBar())
            {
                if (ImGui::BeginMenu("Options"))
                {
                    ImGui::MenuItem("Show System Memory", nullptr, &m_ShowSystemMemory);
                    ImGui::MenuItem("Detailed View", nullptr, &m_ShowDetailedView);
                    ImGui::MenuItem("Enable Leak Detection", nullptr, &m_EnableLeakDetection);

                    ImGui::Separator();
                    ImGui::SliderFloat("Refresh Rate", &m_RefreshInterval, 1.0f / 120.0f, 1.0f, "%.3f s");

                    ImGui::Separator();
                    if (ImGui::Button("Export Report"))
                    {
                        ExportReport("memory_report.txt");
                    }
                    if (ImGui::Button("Begin Peak Window"))
                    {
                        BeginPeakWindow();
                    }

                    ImGui::EndMenu();
                }
                ImGui::EndMenuBar();
            }

            // Tab bar
            if (ImGui::BeginTabBar("MemoryTabs"))
            {
                if (ImGui::BeginTabItem("Overview"))
                {
                    RenderOverviewTab();
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Physical Report"))
                {
                    RenderPhysicalReportTab();
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Detailed"))
                {
                    RenderDetailedTab();
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Leak Detection"))
                {
                    RenderLeakDetectionTab();
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Capacity"))
                {
                    RenderPoolStatsTab();
                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }

            ImGui::End();
        }
    }

    void RendererMemoryTracker::RenderOverviewTab()
    {
        TUniqueLock<FMutex> lock(m_Mutex);

        // The two lines that open this tab used to sit on the END of a `// Summary statistics`
        // and a `// Memory by type` comment, so neither was ever drawn — and in their place the
        // tab logged an OLO_CORE_INFO line on every frame it was open.
        ImGui::Text("GPU resident (live + retiring): %s", DebugUtils::FormatMemorySize(m_GpuLiveBytes + m_GpuRetiringBytes).c_str());
        ImGui::Text("  live %s, retiring %s", DebugUtils::FormatMemorySize(m_GpuLiveBytes).c_str(),
                    DebugUtils::FormatMemorySize(m_GpuRetiringBytes).c_str());
        ImGui::Text("GPU peak: %s (window %s)", DebugUtils::FormatMemorySize(m_PeakGpuResidentBytes).c_str(),
                    DebugUtils::FormatMemorySize(m_WindowPeakGpuResidentBytes).c_str());
        ImGui::Text("CPU tracked (live + retiring): %s", DebugUtils::FormatMemorySize(m_CpuLiveBytes + m_CpuRetiringBytes).c_str());
        ImGui::Text("Tracked entries: %zu live, %zu retiring", m_Allocations.size(), m_Retiring.size());
        ImGui::Text("Total Allocations: %zu", m_TotalAllocations);
        ImGui::Text("Total Deallocations: %zu", m_TotalDeallocations);

        ImGui::Separator();
        ImGui::Text("Live physical bytes by type:");
        for (u32 i = 0; i < static_cast<u32>(std::to_underlying(ResourceType::COUNT)); ++i)
        {
            const auto type = static_cast<ResourceType>(i);
            const sizet usage = m_TypeUsage[i];
            const u32 count = m_TypeCounts[i];
            if (usage > 0)
            {
                ImVec4 color = GetResourceTypeColor(type);
                ImGui::TextColored(color, "%s: %s (%u allocations)",
                                   GetResourceTypeName(type).c_str(),
                                   DebugUtils::FormatMemorySize(usage).c_str(), count);
            }
        }

        ImGui::Separator();
        RenderHistoryGraphs();
    }

    void RendererMemoryTracker::RenderPhysicalReportTab()
    {
        const RendererMemoryReport report = BuildReport();

        const auto bytes = [](u64 value)
        { return DebugUtils::FormatMemorySize(static_cast<sizet>(value)); };

        ImGui::Text("GPU: live %s, retiring %s, peak %s, window peak %s", bytes(report.Gpu.LiveBytes).c_str(),
                    bytes(report.Gpu.RetiringBytes).c_str(), bytes(report.Gpu.PeakBytes).c_str(),
                    bytes(report.Gpu.WindowPeakBytes).c_str());
        ImGui::Text("     %s committed, %s format estimate", bytes(report.Gpu.CommittedBytes).c_str(),
                    bytes(report.Gpu.EstimatedBytes).c_str());
        ImGui::Text("CPU: live %s, retiring %s", bytes(report.Cpu.LiveBytes).c_str(), bytes(report.Cpu.RetiringBytes).c_str());
        ImGui::Text("Aliases: %u (%s logical, not counted as backing), %u orphaned", report.AliasCount,
                    bytes(report.AliasLogicalBytes).c_str(), report.OrphanAliasCount);

        ImGui::Separator();
        ImGui::Text("Backend: %s", ToString(report.Observation.Backend));
        ImGui::Text("Reconciliation: %s", ToString(report.Reconciliation.Status));
        if (report.Reconciliation.ObservedAllocationBytes)
        {
            ImGui::Text("  allocator %s vs tracked %s", bytes(*report.Reconciliation.ObservedAllocationBytes).c_str(),
                        bytes(report.Reconciliation.TrackedCommittedBytes.value_or(0)).c_str());
        }
        ImGui::Text("Residency: %s", ToString(report.Observation.Residency));
        for (const auto& heap : report.Observation.Heaps)
        {
            ImGui::Text("  heap %u%s: usage %s / budget %s (heap %s)", heap.Index, heap.DeviceLocal ? " (device-local)" : "",
                        bytes(heap.UsageBytes).c_str(), bytes(heap.BudgetBytes).c_str(), bytes(heap.HeapSizeBytes).c_str());
        }

        ImGui::Separator();
        if (ImGui::BeginTable("Owners", 5, ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
        {
            ImGui::TableSetupColumn("Owner");
            ImGui::TableSetupColumn("Lifetime");
            ImGui::TableSetupColumn("GPU live");
            ImGui::TableSetupColumn("GPU retiring");
            ImGui::TableSetupColumn("Count");
            ImGui::TableHeadersRow();
            for (const auto& row : report.Owners)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(row.Owner.GetData());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(ToString(row.Lifetime));
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(bytes(row.GpuLiveBytes).c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(bytes(row.GpuRetiringBytes).c_str());
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%u", row.AllocationCount);
            }
            ImGui::EndTable();
        }
    }

    void RendererMemoryTracker::RenderDetailedTab()
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        // Filter controls
        static i32 s_TypeFilter = -1; // -1 means show all
        static bool s_ShowGPUOnly = false;
        static bool s_ShowCPUOnly = false;
        static void* s_SelectedAllocation = nullptr; // Track selected allocation
        ImGui::Text("Filters:");
        // Built from the enum, not a hand-written "\0"-separated list: that list had no
        // "Storage Buffer" entry, so every filter from "Texture 2D" down selected the type
        // one row above the one it named.
        const std::string preview = s_TypeFilter < 0 ? std::string("All") : GetResourceTypeName(static_cast<ResourceType>(s_TypeFilter));
        if (ImGui::BeginCombo("Resource Type", preview.c_str()))
        {
            if (ImGui::Selectable("All", s_TypeFilter < 0))
                s_TypeFilter = -1;
            for (i32 i = 0; i < static_cast<i32>(std::to_underlying(ResourceType::COUNT)); ++i)
            {
                if (ImGui::Selectable(GetResourceTypeName(static_cast<ResourceType>(i)).c_str(), s_TypeFilter == i))
                    s_TypeFilter = i;
            }
            ImGui::EndCombo();
        }
        ImGui::Checkbox("GPU Only", &s_ShowGPUOnly);
        ImGui::SameLine();
        ImGui::Checkbox("CPU Only", &s_ShowCPUOnly);

        ImGui::Separator(); // Allocation table
        if (ImGui::BeginTable("Allocations", 8, ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthFixed, 150.0f);
            ImGui::TableSetupColumn("Age", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableHeadersRow();

            f64 currentTime = DebugUtils::GetCurrentTimeSeconds();
            for (const auto& [address, info] : m_Allocations)
            {
                if (s_TypeFilter >= 0 && s_TypeFilter != static_cast<i32>(std::to_underlying(info.m_Type)))
                    continue;
                if (s_ShowGPUOnly && !info.m_IsGPU)
                    continue;
                if (s_ShowCPUOnly && info.m_IsGPU)
                    continue;

                ImGui::TableNextRow();

                // Check if this row is clicked for selection
                bool isSelected = (s_SelectedAllocation == address);
                if (isSelected)
                {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(100, 100, 150, 100));
                }
                ImGui::TableSetColumnIndex(0);
                // Create unique ID for each selectable
                char selectableId[64];
                snprintf(selectableId, sizeof(selectableId), "##selectable_%p", address);
                if (ImGui::Selectable(selectableId, isSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                {
                    s_SelectedAllocation = (s_SelectedAllocation == address) ? nullptr : address;
                }
                ImGui::SameLine();
                ImGui::Text("0x%p", address);

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", DebugUtils::FormatMemorySize(info.m_Size).c_str());

                ImGui::TableSetColumnIndex(2);
                ImVec4 typeColor = GetResourceTypeColor(info.m_Type);
                ImGui::TextColored(typeColor, "%s", GetResourceTypeName(info.m_Type).c_str());

                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%s", info.m_IsGPU ? "GPU" : "CPU");

                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%s", info.IsAlias() ? "alias" : ToString(info.m_SizeSource));

                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%s", info.m_Name.GetData());

                ImGui::TableSetColumnIndex(6);
                // Extract filename from full path
                std::string filename = info.m_File.ToStdString();
                if (sizet lastSlash = filename.find_last_of("/\\"); lastSlash != std::string::npos)
                    filename = filename.substr(lastSlash + 1);
                ImGui::Text("%s:%u", filename.c_str(), info.m_Line);
                ImGui::TableSetColumnIndex(7);
                f64 age = currentTime - info.m_Timestamp;
                ImGui::Text("%.1fs", age);
            }

            ImGui::EndTable();
        }

        // Show detailed information about selected allocation
        if (s_SelectedAllocation)
        {
            auto it = m_Allocations.find(s_SelectedAllocation);
            if (it != m_Allocations.end())
            {
                ImGui::Separator();
                ImGui::Text("Selected Allocation Details:");
                const AllocationInfo& info = it->second;

                ImGui::Text("Address: 0x%p", s_SelectedAllocation);
                ImGui::Text("Size: %s (%zu bytes)", DebugUtils::FormatMemorySize(info.m_Size).c_str(), info.m_Size);
                ImGui::Text("Type: %s", GetResourceTypeName(info.m_Type).c_str());
                ImGui::Text("Location: %s", info.m_IsGPU ? "GPU" : "CPU");
                ImGui::Text("Backend: %s, size source: %s", ToString(info.m_Backend), ToString(info.m_SizeSource));
                if (info.IsAlias())
                {
                    ImGui::Text("Alias of: 0x%p (logical bytes only)", info.m_BackingAddress);
                }
                ImGui::Text("Owner: %s (%s)", info.m_OwnerId < m_OwnerNames.Num() ? m_OwnerNames[info.m_OwnerId].GetData() : "?",
                            ToString(info.m_Lifetime));
                ImGui::Text("Name: %s", info.m_Name.GetData());
                ImGui::Text("Source: %s:%u", info.m_File.GetData(), info.m_Line);

                f64 currentTime2 = DebugUtils::GetCurrentTimeSeconds();
                f64 age = currentTime2 - info.m_Timestamp;
                ImGui::Text("Age: %.2f seconds", age);
                ImGui::Text("Allocated at: %.6f", info.m_Timestamp);
                if (ImGui::Button("Copy Address to Clipboard"))
                {
                    char addressStr[32];
                    snprintf(addressStr, sizeof(addressStr), "0x%p", s_SelectedAllocation);
                    ImGui::SetClipboardText(addressStr);
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear Selection"))
                {
                    s_SelectedAllocation = nullptr;
                }
            }
            else
            {
                // Selected allocation no longer exists
                s_SelectedAllocation = nullptr;
            }
        }
    }

    void RendererMemoryTracker::RenderLeakDetectionTab()
    {
        TUniqueLock<FMutex> lock(m_Mutex);

        ImGui::Text("Leak Detection Settings:");
        if (f32 threshold = static_cast<f32>(m_LeakDetectionThreshold); ImGui::SliderFloat("Detection Threshold", &threshold, 1.0f, 300.0f, "%.1f seconds"))
        {
            m_LeakDetectionThreshold = static_cast<f64>(threshold);
        }
        if (ImGui::Button("Scan for Leaks"))
        {
            m_LastLeakCheck = DebugUtils::GetCurrentTimeSeconds();
        }
        ImGui::Separator();

        // Detect and display potential leaks inline (avoid double locking)
        TArray<LeakInfo> leaks;
        f64 currentTime = DebugUtils::GetCurrentTimeSeconds();

        for (const auto& [address, info] : m_Allocations)
        {
            f64 age = currentTime - info.m_Timestamp;
            if (age > m_LeakDetectionThreshold)
            {
                LeakInfo leak;
                leak.m_Allocation = info;
                leak.m_AgeSeconds = age;
                leak.m_IsSuspicious = (age > m_LeakDetectionThreshold * 2.0); // Very old allocations are suspicious
                leaks.Add(leak);
            }
        }

        if (leaks.IsEmpty())
        {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "No potential memory leaks detected!");
        }
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Potential memory leaks detected: %d", leaks.Num());

            if (ImGui::BeginTable("Leaks", 6, ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableSetupColumn("Age", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Suspicious", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableHeadersRow();

                for (const auto& leak : leaks)
                {
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("0x%p", leak.m_Allocation.m_Address);

                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s", DebugUtils::FormatMemorySize(leak.m_Allocation.m_Size).c_str());

                    ImGui::TableSetColumnIndex(2);
                    ImVec4 typeColor = GetResourceTypeColor(leak.m_Allocation.m_Type);
                    ImGui::TextColored(typeColor, "%s", GetResourceTypeName(leak.m_Allocation.m_Type).c_str());

                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%.1fs", leak.m_AgeSeconds);

                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%s", leak.m_Allocation.m_Name.GetData());

                    ImGui::TableSetColumnIndex(5);
                    if (leak.m_IsSuspicious)
                    {
                        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Yes");
                    }
                    else
                    {
                        ImGui::Text("No");
                    }
                }

                ImGui::EndTable();
            }
        }
    }

    void RendererMemoryTracker::RenderPoolStatsTab()
    {
        // The capacity-versus-demand rows the owners publish. This tab used to invent
        // "Pool Utilization" and "Fragmentation" percentages from the spread of allocation
        // sizes per resource type — numbers with no pool behind them.
        const RendererMemoryReport report = BuildReport();
        const auto optionalBytes = [](const std::optional<u64>& value) -> std::string
        { return value ? DebugUtils::FormatMemorySize(static_cast<sizet>(*value)) : std::string("unknown"); };

        if (report.Capacity.IsEmpty())
        {
            ImGui::TextUnformatted("No owner publishes capacity rows right now.");
            return;
        }
        if (ImGui::BeginTable("Capacity", 6, ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
        {
            ImGui::TableSetupColumn("Owner");
            ImGui::TableSetupColumn("Category");
            ImGui::TableSetupColumn("Capacity");
            ImGui::TableSetupColumn("Active demand");
            ImGui::TableSetupColumn("Alias savings");
            ImGui::TableSetupColumn("Source / why unknown");
            ImGui::TableHeadersRow();
            for (const auto& row : report.Capacity)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(row.Owner.GetData());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s%s", row.Category.GetData(), row.IsGpu ? "" : " (CPU)");
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(optionalBytes(row.CapacityBytes).c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(optionalBytes(row.ActiveDemandBytes).c_str());
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(optionalBytes(row.AliasSavingsBytes).c_str());
                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%s%s%s", ToString(row.Source), row.UnknownReason.IsEmpty() ? "" : " — ",
                            row.UnknownReason.GetData());
            }
            ImGui::EndTable();
        }
    }

    void RendererMemoryTracker::RenderHistoryGraphs()
    {
        // Memory usage over time
        if (!m_MemoryHistory.IsEmpty())
        {
            ImGui::Text("Memory Usage History:");
            ImGui::PlotLines("Total Memory", m_MemoryHistory.GetData(),
                             (i32)m_MemoryHistory.Num(), m_HistoryIndex,
                             nullptr, 0.0f, FLT_MAX, ImVec2(0, 80));

            ImGui::PlotLines("GPU Memory", m_GPUMemoryHistory.GetData(),
                             (i32)m_GPUMemoryHistory.Num(), m_HistoryIndex,
                             nullptr, 0.0f, FLT_MAX, ImVec2(0, 60));

            ImGui::PlotLines("CPU Memory", m_CPUMemoryHistory.GetData(),
                             (i32)m_CPUMemoryHistory.Num(), m_HistoryIndex,
                             nullptr, 0.0f, FLT_MAX, ImVec2(0, 60));

            ImGui::PlotLines("Allocation Count", m_AllocationHistory.GetData(),
                             (i32)m_AllocationHistory.Num(), m_HistoryIndex,
                             nullptr, 0.0f, FLT_MAX, ImVec2(0, 60));
        }
    }

    sizet RendererMemoryTracker::GetMemoryUsage(ResourceType type) const
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        return m_TypeUsage[static_cast<sizet>(std::to_underlying(type))];
    }

    sizet RendererMemoryTracker::GetTotalMemoryUsage() const
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        return GetTotalMemoryUsageUnlocked();
    }

    sizet RendererMemoryTracker::GetTotalMemoryUsageUnlocked() const
    {
        sizet total = 0;
        for (sizet i = 0; i < static_cast<sizet>(std::to_underlying(ResourceType::COUNT)); ++i)
        {
            total += m_TypeUsage[i];
        }
        return total;
    }

    u32 RendererMemoryTracker::GetAllocationCount(ResourceType type) const
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        return m_TypeCounts[static_cast<sizet>(std::to_underlying(type))];
    }

    TArray<RendererMemoryTracker::LeakInfo> RendererMemoryTracker::DetectLeaks() const
    {
        TUniqueLock<FMutex> lock(m_Mutex);
        TArray<LeakInfo> leaks;
        f64 currentTime = DebugUtils::GetCurrentTimeSeconds();

        for (const auto& [address, info] : m_Allocations)
        {
            f64 age = currentTime - info.m_Timestamp;
            if (age > m_LeakDetectionThreshold)
            {
                LeakInfo leak;
                leak.m_Allocation = info;
                leak.m_AgeSeconds = age;
                leak.m_IsSuspicious = (age > m_LeakDetectionThreshold * 2.0); // Very old allocations are suspicious
                leaks.Add(leak);
            }
        }

        return leaks;
    }

    std::string RendererMemoryTracker::GetResourceTypeName(ResourceType type) const
    {
        switch (type)
        {
            case ResourceType::VertexBuffer:
                return "Vertex Buffer";
            case ResourceType::IndexBuffer:
                return "Index Buffer";
            case ResourceType::UniformBuffer:
                return "Uniform Buffer";
            case ResourceType::StorageBuffer:
                return "Storage Buffer";
            case ResourceType::Texture2D:
                return "Texture 2D";
            case ResourceType::TextureCubemap:
                return "Texture Cubemap";
            case ResourceType::Framebuffer:
                return "Framebuffer";
            case ResourceType::Shader:
                return "Shader";
            case ResourceType::RenderTarget:
                return "Render Target";
            case ResourceType::CommandBuffer:
                return "Command Buffer";
            case ResourceType::AccelerationStructure:
                return "Acceleration Structure";
            case ResourceType::Other:
                return "Other";
            default:
                return "Unknown";
        }
    }

    ImVec4 RendererMemoryTracker::GetResourceTypeColor(ResourceType type) const
    {
        switch (type)
        {
            case ResourceType::VertexBuffer:
                return ImVec4(0.2f, 0.8f, 0.2f, 1.0f); // Green
            case ResourceType::IndexBuffer:
                return ImVec4(0.2f, 0.6f, 0.8f, 1.0f); // Blue
            case ResourceType::UniformBuffer:
                return ImVec4(0.8f, 0.6f, 0.2f, 1.0f); // Orange
            case ResourceType::StorageBuffer:
                return ImVec4(0.9f, 0.4f, 0.1f, 1.0f); // Dark Orange
            case ResourceType::Texture2D:
                return ImVec4(0.8f, 0.2f, 0.8f, 1.0f); // Magenta
            case ResourceType::TextureCubemap:
                return ImVec4(0.6f, 0.2f, 0.8f, 1.0f); // Purple
            case ResourceType::Framebuffer:
                return ImVec4(0.8f, 0.2f, 0.2f, 1.0f); // Red
            case ResourceType::Shader:
                return ImVec4(0.8f, 0.8f, 0.2f, 1.0f); // Yellow
            case ResourceType::RenderTarget:
                return ImVec4(0.2f, 0.8f, 0.8f, 1.0f); // Cyan
            case ResourceType::CommandBuffer:
                return ImVec4(0.6f, 0.8f, 0.2f, 1.0f); // Lime
            case ResourceType::AccelerationStructure:
                return ImVec4(0.4f, 0.7f, 1.0f, 1.0f); // Sky blue
            case ResourceType::Other:
                return ImVec4(0.6f, 0.6f, 0.6f, 1.0f); // Gray
            default:
                return ImVec4(0.8f, 0.8f, 0.8f, 1.0f); // Light Gray
        }
    }

    bool RendererMemoryTracker::ExportReport(const std::string& filePath) const
    {
        OLO_PROFILE_FUNCTION();

        try
        {
            const RendererMemoryReport report = BuildReport();

            std::ofstream file(filePath);
            if (!file.is_open())
                return false;
            TUniqueLock<FMutex> lock(m_Mutex);

            file << "Renderer Memory Usage Report\n";
            file << "Generated: " << std::chrono::system_clock::now().time_since_epoch().count() << "\n";
            file << "========================================\n\n";

            file << "Summary (physical backing only; aliases excluded):\n";
            file << "GPU live: " << report.Gpu.LiveBytes << " bytes, retiring: " << report.Gpu.RetiringBytes
                 << " bytes, peak: " << report.Gpu.PeakBytes << " bytes\n";
            file << "GPU committed: " << report.Gpu.CommittedBytes << " bytes, format estimate: " << report.Gpu.EstimatedBytes << " bytes\n";
            file << "CPU live: " << report.Cpu.LiveBytes << " bytes, retiring: " << report.Cpu.RetiringBytes << " bytes\n";
            file << "Aliases: " << report.AliasCount << " (" << report.AliasLogicalBytes << " logical bytes), orphaned: "
                 << report.OrphanAliasCount << "\n";
            file << "Reconciliation: " << ToString(report.Reconciliation.Status) << "\n";
            file << "Residency: " << ToString(report.Observation.Residency) << "\n";
            file << "Total Allocations: " << m_TotalAllocations << "\n";
            file << "Total Deallocations: " << m_TotalDeallocations << "\n\n";

            file << "Memory by Type (live physical):\n";
            for (u32 i = 0; i < static_cast<u32>(std::to_underlying(ResourceType::COUNT)); ++i)
            {
                ResourceType type = (ResourceType)i;
                sizet usage = m_TypeUsage[i];
                u32 count = m_TypeCounts[i];

                if (usage > 0)
                {
                    file << GetResourceTypeName(type) << ": " << DebugUtils::FormatMemorySize(usage)
                         << " (" << count << " allocations)\n";
                }
            }

            file << "\nDetailed Allocations:\n";
            file << "Address,Size,Type,Location,Kind,Owner,Name,File,Line,Age\n";
            f64 currentTime = DebugUtils::GetCurrentTimeSeconds();
            for (const auto& [address, info] : m_Allocations)
            {
                f64 age = currentTime - info.m_Timestamp;
                file << std::hex << address << std::dec << ","
                     << info.m_Size << ","
                     << GetResourceTypeName(info.m_Type) << ","
                     << (info.m_IsGPU ? "GPU" : "CPU") << ","
                     << (info.IsAlias() ? "alias" : ToString(info.m_SizeSource)) << ","
                     << (info.m_OwnerId < m_OwnerNames.Num() ? m_OwnerNames[info.m_OwnerId].ToView() : std::string_view("?")) << ","
                     << info.m_Name.ToView() << ","
                     << info.m_File.ToView() << ","
                     << info.m_Line << ","
                     << std::fixed << std::setprecision(1) << age << "\n";
            }

            file.close();
            OLO_CORE_INFO("Memory report exported to: {0}", filePath);
            return true;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("Failed to export memory report: {0}", e.what());
            return false;
        }
    }
} // namespace OloEngine
