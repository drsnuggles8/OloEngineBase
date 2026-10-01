#pragma once

#include "OloEngine/Core/Base.h"
#include "DebugUtils.h"
#include "RendererMemoryReport.h"
#include <imgui.h>
#include <string>
#include <string_view>
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/String.h"
#include <unordered_map>
#include <map>
#include <memory>
#include <atomic>
#include <array>
#include <functional>
#include <utility>

#include "OloEngine/Threading/Mutex.h"

namespace OloEngine
{
    // @brief The renderer's physical allocation table (issue #1342).
    //
    // One entry per BACKING allocation. The OpenGL resource classes book format estimates
    // through the OLO_TRACK_* macros below; the Vulkan backend books every VmaAllocation
    // through VulkanTrackedAllocation (committed sizes). A view or a second handle onto
    // an existing allocation is booked with TrackAlias, which carries logical bytes only —
    // counting it as backing is the double count RendererMemoryReportSnapshotTest pins.
    //
    // An allocation passes through two states:
    //   live     — its owner holds it;
    //   retiring — its owner released it, but the backend still holds the memory until
    //              the GPU is done with it (FrameResourceManager's deletion queue on GL,
    //              VulkanDeferredReclaim on Vulkan). RetireAllocation moves it there and
    //              hands back a ticket; ReleaseRetired(ticket) is called when the backing
    //              is actually freed.
    class RendererMemoryTracker
    {
      public:
        // Resource types for categorization
        enum class ResourceType : u8
        {
            VertexBuffer = 0,
            IndexBuffer,
            UniformBuffer,
            StorageBuffer,
            Texture2D,
            TextureCubemap,
            Framebuffer,
            Shader,
            RenderTarget,
            CommandBuffer,
            AccelerationStructure,
            Other,
            COUNT
        };

        // Memory allocation info
        struct AllocationInfo
        {
            void* m_Address = nullptr;
            // Non-null for an alias: the backing entry this view/handle points into.
            void* m_BackingAddress = nullptr;
            sizet m_Size = 0;
            ResourceType m_Type = ResourceType::Other;
            FString m_Name;
            FString m_File;
            u32 m_Line = 0;
            f64 m_Timestamp = 0.0;
            bool m_IsGPU = false;
            MemoryBackend m_Backend = MemoryBackend::Unknown;
            MemorySizeSource m_SizeSource = MemorySizeSource::FormatEstimate;
            MemoryLifetime m_Lifetime = MemoryLifetime::Unattributed;
            u16 m_OwnerId = 0;   // index into the owner-name table; 0 = "Unattributed"
            u64 m_HandleKey = 0; // RHI::HashKey of the handle bound by BindResourceHandle, or 0

            [[nodiscard]] bool IsAlias() const
            {
                return m_BackingAddress != nullptr;
            }
        };

        // Everything TrackAllocation needs, for the callers that know more than the
        // OpenGL macros do (the Vulkan allocator wrapper).
        struct AllocationDesc
        {
            void* Address = nullptr;
            sizet Size = 0;
            ResourceType Type = ResourceType::Other;
            std::string_view Name;
            bool IsGPU = true;
            MemoryBackend Backend = MemoryBackend::Unknown;
            MemorySizeSource SizeSource = MemorySizeSource::FormatEstimate;
            const char* File = nullptr;
            u32 Line = 0;
        };

        // Memory leak detection
        struct LeakInfo
        {
            AllocationInfo m_Allocation;
            f64 m_AgeSeconds = 0.0;
            bool m_IsSuspicious = false;
        };

        // An owner's capacity-versus-demand rows. Called by BuildReport on the thread that
        // builds the report (the main/render thread), never under the tracker's mutex, so a
        // reporter may read the tracker. It must not register or unregister reporters.
        using CapacityReporter = MemoryCapacityReporter;

        // What the active backend's allocator says (Renderer::Init installs one that asks
        // RendererAPI::ObserveDeviceMemory). Returns false when there is no device to ask.
        using BackendObserver = bool (*)(BackendMemoryObservation&);

      public:
        static RendererMemoryTracker& GetInstance();

        // @brief Initialize the memory tracker
        void Initialize();

        // @brief Shutdown the memory tracker
        void Shutdown();

        // @brief Reset all tracking data and statistics
        void Reset();

        // @brief Track a backing allocation (OpenGL macro path: a format estimate).
        void TrackAllocation(void* address, sizet size, ResourceType type,
                             const std::string& name, bool isGPU,
                             const char* file, u32 line);

        // @brief Track a backing allocation with its backend and size provenance.
        void TrackAllocation(const AllocationDesc& desc);

        // @brief Track a view/alias onto an already-tracked backing allocation. It reports
        // `logicalBytes` as alias bytes and adds nothing to the physical totals. Untrack it
        // with TrackDeallocation(aliasAddress).
        void TrackAlias(void* aliasAddress, void* backingAddress, sizet logicalBytes, ResourceType type,
                        std::string_view name, const char* file, u32 line);

        // @brief Views by RHI handle — see RendererMemory:: in RendererMemoryReport.h.
        void BindResourceHandle(void* address, u64 handleKey);
        void TrackAliasOfHandle(u64 aliasHandleKey, u64 backingHandleKey, ResourceType type, std::string_view name,
                                const char* file, u32 line);
        void UntrackAliasOfHandle(u64 aliasHandleKey);

        // @brief Rename a live entry (the Vulkan wrapper books first, then learns the name).
        void RenameAllocation(void* address, std::string_view name);

        // @brief Track a memory deallocation (the backing is freed now).
        void TrackDeallocation(void* address, const char* file, u32 line);

        // @brief The owner released `address`, but the backing is only freed later (deferred
        // deletion). Moves the entry to the retiring set and returns a ticket for
        // ReleaseRetired; returns 0 when `address` is not tracked. `address` may be reused
        // immediately after this returns.
        [[nodiscard]] u64 RetireAllocation(void* address);

        // @brief The retired backing behind `ticket` is freed. 0 is a no-op.
        void ReleaseRetired(u64 ticket);

        // @brief Update memory statistics (call once per frame)
        void UpdateStats();

        // @brief Render the memory tracker UI
        void RenderUI(bool* open = nullptr);

        // @brief Live physical bytes of one resource type (aliases and retiring entries excluded)
        sizet GetMemoryUsage(ResourceType type) const;

        // @brief Live physical bytes, CPU and GPU together. Prefer BuildReport(), which keeps
        // the two apart; this remains for the leak/teardown checks that want one number.
        sizet GetTotalMemoryUsage() const;

        // @brief Live + retiring physical GPU bytes: what the device holds for the renderer.
        u64 GetGpuResidentBytes() const;
        // @brief Live + retiring CPU-side bookings. Never added to the GPU figure.
        u64 GetCpuResidentBytes() const;

        // @brief Allocation count by type (backing entries only)
        u32 GetAllocationCount(ResourceType type) const;

        // @brief Start a peak window: MemoryTotals::WindowPeakBytes restarts from the
        // current resident bytes. Used to record transient coexistence around a resize, a
        // hot reload or a scene load.
        void BeginPeakWindow();

        // @brief Backing allocations older than the detection threshold. An age heuristic:
        // persistent resources pass it too, so it is a list to inspect, not a leak verdict.
        TArray<LeakInfo> DetectLeaks() const;
        [[nodiscard]] f64 GetLeakDetectionThresholdSeconds() const
        {
            return m_LeakDetectionThreshold;
        }

        // @brief Export memory report to file
        bool ExportReport(const std::string& filePath) const;

        // @brief Assemble the physical report: totals, per-owner rows, capacity rows and
        // the backend reconciliation. Call on the main/render thread (the capacity
        // reporters read render-thread state).
        [[nodiscard]] RendererMemoryReport BuildReport() const;

        // Capacity reporters. The handle is non-zero; unregister before the owner dies.
        [[nodiscard]] u64 RegisterCapacityReporter(CapacityReporter reporter);
        void UnregisterCapacityReporter(u64 handle);

        // The backend allocator observer; nullptr clears it. Committed entries are reconciled
        // against it only when their backend matches the observation's.
        void SetBackendObserver(BackendObserver observer);
        // The default observer: RenderCommand's live RendererAPI::ObserveDeviceMemory.
        static bool ObserveActiveRendererAPI(BackendMemoryObservation& out);

        // Bracket a backend allocation or free whose tracker call is not atomic with the
        // allocator's own bookkeeping. BuildReport reports Racing instead of a false
        // difference when a bracket was open (or opened) while it read both sides.
        void BeginExternalMutation();
        void EndExternalMutation();

      private:
        RendererMemoryTracker() = default;
        ~RendererMemoryTracker() = default;
        // Helper methods
        void RenderOverviewTab();
        void RenderPhysicalReportTab();
        void RenderDetailedTab();
        void RenderLeakDetectionTab();
        void RenderPoolStatsTab();
        void RenderHistoryGraphs();

        std::string GetResourceTypeName(ResourceType type) const;
        ImVec4 GetResourceTypeColor(ResourceType type) const;

        // Internal helpers (assume the lock is already held)
        sizet GetTotalMemoryUsageUnlocked() const;
        void AddPhysicalUnlocked(const AllocationInfo& info, bool retiring);
        void RemovePhysicalUnlocked(const AllocationInfo& info, bool retiring);
        void NotePeakUnlocked();
        u16 InternOwnerUnlocked(std::string_view owner);
        void StampOwnerUnlocked(AllocationInfo& info);
        void InsertBackingUnlocked(AllocationInfo info);
        void TrackAliasUnlocked(void* aliasAddress, void* backingAddress, sizet logicalBytes, ResourceType type,
                                std::string_view name, const char* file, u32 line);
        void ForgetHandleUnlocked(const AllocationInfo& info);

        // Thread safety
        mutable FMutex m_Mutex;
        // Allocation tracking
        std::unordered_map<void*, AllocationInfo> m_Allocations;
        std::unordered_map<u64, AllocationInfo> m_Retiring;
        // RHI handle key -> backing address (BindResourceHandle), and the alias keys for views
        // tracked by handle: each view's address key is its node's value in m_ViewTokens —
        // a stable, unique heap address that can never collide with a real allocation's.
        std::unordered_map<u64, void*> m_BackingByHandle;
        std::unordered_map<u64, u8> m_ViewTokens;
        u64 m_NextRetireTicket = 1;
        std::array<sizet, static_cast<sizet>(std::to_underlying(ResourceType::COUNT))> m_TypeUsage{};
        std::array<u32, static_cast<sizet>(std::to_underlying(ResourceType::COUNT))> m_TypeCounts{};

        // Physical running totals (backing entries only; aliases never reach these).
        u64 m_GpuLiveBytes = 0;
        u64 m_GpuRetiringBytes = 0;
        u64 m_CpuLiveBytes = 0;
        u64 m_CpuRetiringBytes = 0;
        u64 m_PeakGpuResidentBytes = 0;
        u64 m_WindowPeakGpuResidentBytes = 0;
        u64 m_PeakCpuResidentBytes = 0;
        u64 m_WindowPeakCpuResidentBytes = 0;

        // Owner names, interned. Index 0 is "Unattributed". Owner names outlive every entry
        // that refers to them (the table only grows until Reset).
        TArray<FString> m_OwnerNames;
        std::unordered_map<std::string, u16> m_OwnerIds;

        // Capacity reporters and the backend observer, under their own lock so BuildReport
        // can call them without holding m_Mutex.
        mutable FMutex m_ReporterMutex;
        std::map<u64, CapacityReporter> m_CapacityReporters;
        u64 m_NextReporterHandle = 1;
        // Defaults to asking the live RendererAPI; tests substitute a fake.
        BackendObserver m_BackendObserver = &ObserveActiveRendererAPI;

        std::atomic<u32> m_ExternalMutationsInFlight{ 0 };
        std::atomic<u64> m_ExternalMutationEpoch{ 0 };

        // History for graphs
        static constexpr u32 OLO_HISTORY_SIZE = 300; // 5 minutes at 60fps
        TArray<f32> m_MemoryHistory;
        TArray<f32> m_AllocationHistory;
        TArray<f32> m_GPUMemoryHistory;
        TArray<f32> m_CPUMemoryHistory;
        u32 m_HistoryIndex = 0;

        // Leak detection parameters
        f64 m_LeakDetectionThreshold = 30.0; // seconds
        f64 m_LastLeakCheck = 0.0;

        // UI state
        bool m_ShowSystemMemory = true;
        bool m_ShowDetailedView = false;
        bool m_EnableLeakDetection = true;
        f32 m_RefreshInterval = 1.0f / 60.0f; // 60 FPS

        // Statistics
        sizet m_PeakMemoryUsage = 0;
        sizet m_TotalAllocations = 0;
        sizet m_TotalDeallocations = 0;
        f64 m_LastUpdateTime = 0.0;
        // Shutdown tracking
        std::atomic<bool> m_IsShutdown{ false };
        std::atomic<bool> m_IsInitialized{ false };
    };

    // AllocationInfo owns two FStrings plus scalar/address metadata; the tracked
    // allocation address points outside the record and never to its own fields.
    template<>
    struct TIsTriviallyRelocatable<RendererMemoryTracker::AllocationInfo>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(RendererMemoryTracker::AllocationInfo::m_Address)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererMemoryTracker::AllocationInfo::m_BackingAddress)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererMemoryTracker::AllocationInfo::m_Size)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererMemoryTracker::AllocationInfo::m_Type)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererMemoryTracker::AllocationInfo::m_Name)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererMemoryTracker::AllocationInfo::m_File)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererMemoryTracker::AllocationInfo::m_Line)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererMemoryTracker::AllocationInfo::m_Timestamp)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererMemoryTracker::AllocationInfo::m_IsGPU)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererMemoryTracker::AllocationInfo::m_Backend)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererMemoryTracker::AllocationInfo::m_SizeSource)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererMemoryTracker::AllocationInfo::m_Lifetime)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererMemoryTracker::AllocationInfo::m_OwnerId)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererMemoryTracker::AllocationInfo::m_HandleKey)>::Value;
    };

    template<>
    struct TIsTriviallyRelocatable<RendererMemoryTracker::LeakInfo>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(RendererMemoryTracker::LeakInfo::m_Allocation)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererMemoryTracker::LeakInfo::m_AgeSeconds)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererMemoryTracker::LeakInfo::m_IsSuspicious)>::Value;
    };
} // namespace OloEngine

// Convenience macros for the OpenGL resource classes (defined outside the namespace for
// global use). They book FORMAT ESTIMATES against the OpenGL backend: GL exposes no
// committed allocation size. Vulkan never uses them — see VulkanTrackedAllocation.h.
#define OLO_TRACK_GPU_ALLOC(ptr, size, type, name)                                                                        \
    do                                                                                                                    \
    {                                                                                                                     \
        OloEngine::RendererMemoryTracker::GetInstance().TrackAllocation(ptr, size, type, name, true, __FILE__, __LINE__); \
    } while (0)

#define OLO_TRACK_CPU_ALLOC(ptr, size, type, name)                                                                         \
    do                                                                                                                     \
    {                                                                                                                      \
        OloEngine::RendererMemoryTracker::GetInstance().TrackAllocation(ptr, size, type, name, false, __FILE__, __LINE__); \
    } while (0)

#define OLO_TRACK_DEALLOC(ptr)                                                                      \
    do                                                                                              \
    {                                                                                               \
        OloEngine::RendererMemoryTracker::GetInstance().TrackDeallocation(ptr, __FILE__, __LINE__); \
    } while (0)

// The owner is done with `ptr`, but its backing is freed by a deferred-deletion lambda.
// Evaluates to the ticket; pass it into the lambda and call OLO_TRACK_RELEASE_RETIRED there.
#define OLO_TRACK_RETIRE(ptr) (OloEngine::RendererMemoryTracker::GetInstance().RetireAllocation(ptr))

#define OLO_TRACK_RELEASE_RETIRED(ticket) (OloEngine::RendererMemoryTracker::GetInstance().ReleaseRetired(ticket))
