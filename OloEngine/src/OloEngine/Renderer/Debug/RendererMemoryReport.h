#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/String.h"

#include <functional>
#include <optional>
#include <string_view>

// The physical-allocation-aware renderer memory report (issue #1342).
//
// Plain data plus pure functions, so every classification here unit-tests with no
// device. RendererMemoryTracker fills it; the editor panel, olo_memory_report and the
// benchmark export read it. Three rules shape every field:
//
//   1. PHYSICAL BACKING IS COUNTED ONCE. A view, an attachment view or a second RHI
//      handle onto one image is an ALIAS entry: it reports logical bytes and adds zero
//      physical bytes. Only a backing entry contributes to Gpu/Cpu totals.
//   2. CPU AND GPU NEVER SHARE A TOTAL. Two separate MemoryTotals, each with its own
//      units, and no field anywhere that adds them.
//   3. UNKNOWN IS NOT ZERO. A quantity nothing can measure is an empty optional plus a
//      reason (see measurement-validity-and-sentinels.md). GL residency is the standing
//      example: GL has no residency query, so it reports Unknown, never 0 bytes.
namespace OloEngine
{
    enum class MemoryBackend : u8
    {
        Unknown = 0,
        OpenGL,
        Vulkan
    };

    // Where an allocation's byte count came from.
    enum class MemorySizeSource : u8
    {
        // Computed from width x height x format x mips x samples. The OpenGL path: GL
        // exposes no committed size, so the driver's padding and tiling are invisible.
        FormatEstimate = 0,
        // Reported by the allocator that holds the memory (VmaAllocationInfo::size).
        Committed
    };

    // What an allocation is for, and so how long it is expected to live. Set by the
    // RendererMemoryOwnerScope active on the allocating thread.
    enum class MemoryLifetime : u8
    {
        Unattributed = 0, // no owner scope was active
        Persistent,       // renderer-owned, lives until shutdown or a settings change
        Asset,            // loaded content: textures, meshes
        Pooled,           // reused across frames by a pool (TransientPool)
        History,          // temporal history, one per effect/view/plane
        PerFrame,         // frame arenas, per-frame rings
        Staging,          // upload/readback staging, freed within a frame or two
        PassOwned,        // created while a render-graph pass executed, owned by it
        COUNT
    };

    enum class MemoryReconciliationStatus : u8
    {
        Reconciled = 0, // tracked committed bytes == allocator-observed bytes, to the byte
        Untracked,      // the allocator holds bytes the tracker never saw
        OverCounted,    // the tracker holds MORE than the allocator: a double count
        Racing,         // an allocation was in flight while the two were read
        NotObservable,  // the backend has no allocator to ask (OpenGL)
        NoDevice        // no backend observer is registered
    };

    enum class MemoryResidencyStatus : u8
    {
        OsReported = 0,     // VK_EXT_memory_budget: the OS's per-heap usage and budget
        AllocatorHeuristic, // VMA without that extension: usage = VMA's own blocks,
                            // budget = a fixed fraction of the heap size
        Unknown             // no residency query on this backend (OpenGL)
    };

    struct MemoryHeapObservation
    {
        u32 Index = 0;
        bool DeviceLocal = false;
        u64 HeapSizeBytes = 0;
        u64 UsageBytes = 0;
        u64 BudgetBytes = 0;
        u64 AllocatorBlockBytes = 0;
        u64 AllocatorAllocationBytes = 0;
    };

    // What the backend itself says, independently of the tracker.
    struct BackendMemoryObservation
    {
        MemoryBackend Backend = MemoryBackend::Unknown;
        // True when AllocationBytes/BlockBytes/AllocationCount come from the allocator.
        bool HasAllocatorTotals = false;
        u64 AllocationBytes = 0; // sum of live allocation sizes (what backing entries should sum to)
        u64 BlockBytes = 0;      // device memory the allocator reserved (>= AllocationBytes)
        u64 AllocationCount = 0;
        MemoryResidencyStatus Residency = MemoryResidencyStatus::Unknown;
        TArray<MemoryHeapObservation> Heaps;
    };

    struct MemoryReconciliation
    {
        MemoryReconciliationStatus Status = MemoryReconciliationStatus::NoDevice;
        std::optional<u64> ObservedAllocationBytes;
        std::optional<u64> TrackedCommittedBytes;
        std::optional<i64> DifferenceBytes; // observed - tracked; > 0 untracked, < 0 over-counted
    };

    // Pure classification. `trackedCommittedBytes` is the tracker's sum of COMMITTED backing
    // entries (live and retiring) for the observed backend; `racing` is true when an
    // allocation was created or destroyed while the two figures were read.
    [[nodiscard]] MemoryReconciliation ReconcileCommittedBytes(u64 trackedCommittedBytes,
                                                               const BackendMemoryObservation* observation,
                                                               bool racing);

    struct MemoryTotals
    {
        u64 LiveBytes = 0;      // backing entries whose owner still holds them
        u64 RetiringBytes = 0;  // released by the owner, backing not yet freed (in-flight retirement)
        u64 EstimatedBytes = 0; // of Live+Retiring, how much is a format estimate
        u64 CommittedBytes = 0; // of Live+Retiring, how much the allocator reported
        u32 LiveCount = 0;
        u32 RetiringCount = 0;
        u64 PeakBytes = 0;       // max(Live+Retiring) since the tracker was initialized/reset
        u64 WindowPeakBytes = 0; // max(Live+Retiring) since the last BeginPeakWindow()

        [[nodiscard]] u64 ResidentBytes() const
        {
            return LiveBytes + RetiringBytes;
        }
    };

    // Physical bytes grouped by the owner scope active at allocation time.
    struct MemoryOwnerRow
    {
        FString Owner;
        MemoryLifetime Lifetime = MemoryLifetime::Unattributed;
        u64 GpuLiveBytes = 0;
        u64 GpuRetiringBytes = 0;
        u64 CpuLiveBytes = 0;
        u64 CpuRetiringBytes = 0;
        u32 AllocationCount = 0;
        u64 EstimatedBytes = 0;
        u64 CommittedBytes = 0;
    };

    // One owner's capacity-versus-demand view, published by the owner itself through a
    // capacity reporter. These rows ATTRIBUTE bytes already counted in the totals; they are
    // never added to them.
    struct MemoryCapacityRow
    {
        FString Owner;    // e.g. "TransientPool", "TemporalHistory"
        FString Category; // e.g. "G-buffer", "ReSTIR DI reservoirs"
        MemoryLifetime Lifetime = MemoryLifetime::Unattributed;
        MemorySizeSource Source = MemorySizeSource::FormatEstimate;
        bool IsGpu = true;
        std::optional<u64> CapacityBytes;     // what the owner holds
        std::optional<u64> ActiveDemandBytes; // what the last frame actually used
        std::optional<u64> AliasSavingsBytes; // logical bytes served without their own backing
        FString UnknownReason;                // required whenever an optional above is empty
    };

    struct RendererMemoryReport
    {
        MemoryTotals Gpu;
        MemoryTotals Cpu;
        u64 AliasLogicalBytes = 0; // bytes views/aliases would claim if counted as backing
        u32 AliasCount = 0;
        u32 OrphanAliasCount = 0; // aliases whose backing entry is not tracked (a hole, not a saving)
        TArray<MemoryOwnerRow> Owners;
        TArray<MemoryCapacityRow> Capacity;
        MemoryReconciliation Reconciliation;
        BackendMemoryObservation Observation;
    };

    // @brief Attributes every allocation made on this thread, while it lives, to `owner`.
    //
    // Scopes nest; the innermost wins. `owner` must outlive the scope (a string literal, or a
    // name owned by the object that opened the scope) — it is interned when an allocation
    // is booked. The render graph opens one per executing pass, so a pass's lazily created
    // resources are attributed to it with no code in the pass.
    class RendererMemoryOwnerScope
    {
      public:
        RendererMemoryOwnerScope(std::string_view owner, MemoryLifetime lifetime);
        ~RendererMemoryOwnerScope();

        RendererMemoryOwnerScope(const RendererMemoryOwnerScope&) = delete;
        RendererMemoryOwnerScope& operator=(const RendererMemoryOwnerScope&) = delete;

        struct Frame
        {
            std::string_view Owner;
            MemoryLifetime Lifetime = MemoryLifetime::Unattributed;
        };
        // The innermost active scope on this thread, or an empty Owner when none is open.
        [[nodiscard]] static Frame Current();

      private:
        bool m_Pushed = false;
    };

    // An owner's capacity-versus-demand rows, appended when a report is built. Runs on the
    // thread that builds the report (the main/render thread).
    using MemoryCapacityReporter = std::function<void(TArray<MemoryCapacityRow>&)>;

    // Registers a capacity reporter with RendererMemoryTracker for as long as it lives.
    // Movable, not copyable. Declared here rather than beside the tracker so an owner's
    // header can hold one without pulling in the tracker (and ImGui).
    class RendererMemoryReporterHandle
    {
      public:
        RendererMemoryReporterHandle() = default;
        explicit RendererMemoryReporterHandle(MemoryCapacityReporter reporter);
        ~RendererMemoryReporterHandle();
        RendererMemoryReporterHandle(RendererMemoryReporterHandle&& other) noexcept;
        RendererMemoryReporterHandle& operator=(RendererMemoryReporterHandle&& other) noexcept;
        RendererMemoryReporterHandle(const RendererMemoryReporterHandle&) = delete;
        RendererMemoryReporterHandle& operator=(const RendererMemoryReporterHandle&) = delete;

        void Reset();

      private:
        u64 m_Handle = 0;
    };

    // Views by RHI handle (#1342). A backend-neutral seam for the one place views are made
    // from a handle rather than from a tracked object: RenderCommand's view creation and
    // deletion. Declared here so RenderCommand.h need not include the tracker.
    namespace RendererMemory
    {
        // Binds a backing entry (tracked at `backingAddress`) to the RHI handle other code
        // knows it by, so a view made from that handle can find the storage it aliases.
        void BindBackingResourceHandle(void* backingAddress, u64 handleKey);
        // A view onto `sourceHandleKey`'s storage, with no backing of its own: booked as an
        // alias of the source's entry at the source's size.
        void TrackResourceView(u64 viewHandleKey, u64 sourceHandleKey, std::string_view name);
        // The view's handle was deleted. A no-op for a handle that is not a tracked view.
        void UntrackResourceView(u64 viewHandleKey);
    } // namespace RendererMemory

    [[nodiscard]] const char* ToString(MemoryBackend backend);
    [[nodiscard]] const char* ToString(MemorySizeSource source);
    [[nodiscard]] const char* ToString(MemoryLifetime lifetime);
    [[nodiscard]] const char* ToString(MemoryReconciliationStatus status);
    [[nodiscard]] const char* ToString(MemoryResidencyStatus status);

    // Owned FString plus scalars; no pointers into the record.
    template<>
    struct TIsTriviallyRelocatable<MemoryOwnerRow>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(MemoryOwnerRow::Owner)>::Value &&
                                      TIsTriviallyRelocatable<decltype(MemoryOwnerRow::Lifetime)>::Value &&
                                      TIsTriviallyRelocatable<decltype(MemoryOwnerRow::GpuLiveBytes)>::Value &&
                                      TIsTriviallyRelocatable<decltype(MemoryOwnerRow::GpuRetiringBytes)>::Value &&
                                      TIsTriviallyRelocatable<decltype(MemoryOwnerRow::CpuLiveBytes)>::Value &&
                                      TIsTriviallyRelocatable<decltype(MemoryOwnerRow::CpuRetiringBytes)>::Value &&
                                      TIsTriviallyRelocatable<decltype(MemoryOwnerRow::AllocationCount)>::Value &&
                                      TIsTriviallyRelocatable<decltype(MemoryOwnerRow::EstimatedBytes)>::Value &&
                                      TIsTriviallyRelocatable<decltype(MemoryOwnerRow::CommittedBytes)>::Value;
    };

    // Owned FStrings, optionals of scalars and enums; no pointers into the record.
    template<>
    struct TIsTriviallyRelocatable<MemoryCapacityRow>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(MemoryCapacityRow::Owner)>::Value &&
                                      TIsTriviallyRelocatable<decltype(MemoryCapacityRow::Category)>::Value &&
                                      TIsTriviallyRelocatable<decltype(MemoryCapacityRow::Lifetime)>::Value &&
                                      TIsTriviallyRelocatable<decltype(MemoryCapacityRow::Source)>::Value &&
                                      TIsTriviallyRelocatable<decltype(MemoryCapacityRow::IsGpu)>::Value &&
                                      TIsTriviallyRelocatable<decltype(MemoryCapacityRow::CapacityBytes)>::Value &&
                                      TIsTriviallyRelocatable<decltype(MemoryCapacityRow::ActiveDemandBytes)>::Value &&
                                      TIsTriviallyRelocatable<decltype(MemoryCapacityRow::AliasSavingsBytes)>::Value &&
                                      TIsTriviallyRelocatable<decltype(MemoryCapacityRow::UnknownReason)>::Value;
    };
} // namespace OloEngine
