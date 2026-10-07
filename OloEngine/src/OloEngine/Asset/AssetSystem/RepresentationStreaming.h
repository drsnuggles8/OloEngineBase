#pragma once

#include "OloEngine/Asset/AssetByteSize.h"
#include "OloEngine/Asset/AssetSystem/CancellableLoadSet.h"

#include <atomic>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace OloEngine
{
    struct FRepresentationIOStats
    {
        u64 ReadBytes = 0;
        u64 ReadMicroseconds = 0;
    };

    // CPU-only immutable preparation output. GPU construction belongs to the
    // consuming main-thread renderer, never to a load callback.
    class FRepresentationPayload : public RefCounted
    {
      public:
        [[nodiscard]] virtual u64 GetCpuBytes() const noexcept = 0;
        // Present only when the preparer measured actual source reads. A
        // descriptor estimate cannot substitute for measured I/O telemetry.
        [[nodiscard]] virtual std::optional<FRepresentationIOStats> GetIOStats() const noexcept
        {
            return std::nullopt;
        }
    };

    struct FRepresentationDescriptor
    {
        FAssetByteSize CpuBytes;
        FAssetByteSize DiskBytes;
        u64 UploadBytes = 0;
        u64 GpuBytes = 0;
    };

    enum class ERepresentationRequestResult : u8
    {
        Queued,
        AlreadyPending,
        Failed,
        StagingBudget,
        Stopped
    };

    struct FCompletedRepresentationLoad
    {
        u64 Key = 0;
        u64 StagingTicket = 0;
        FRepresentationDescriptor Descriptor;
        Ref<FRepresentationPayload> Payload;
    };

    template<>
    struct TIsTriviallyRelocatable<FCompletedRepresentationLoad>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(FCompletedRepresentationLoad::Key)> &&
                                      TIsTriviallyRelocatable_V<decltype(FCompletedRepresentationLoad::StagingTicket)> &&
                                      TIsTriviallyRelocatable_V<decltype(FCompletedRepresentationLoad::Descriptor)> &&
                                      TIsTriviallyRelocatable_V<decltype(FCompletedRepresentationLoad::Payload)>;
    };

    struct FRepresentationLoadStats
    {
        u64 StagingBytes = 0;
        u32 UnknownStagingCount = 0;
        u64 MaxStagingBytes = 0;
        u32 PendingCount = 0;
        u32 CompletedUnretrievedCount = 0;
        u32 AbandonedRunningCount = 0;
        u32 HeldCompletionCount = 0;
        u64 RejectedForStaging = 0;
        u64 FailedLoads = 0;
        FCancellableLoadCounters Cancellation;
    };

    // Specialised consumers reuse the general asset load state machine. This
    // owns tasks and staging reservations, not asset identities or a registry.
    // All methods run on the main thread; work captures only immutable CPU data.
    // CpuBytes must conservatively bound the callback's preparation working set.
    // Unknown sizes are refused when bounded. A violating callback is detected
    // on completion, but no allocator can enforce its transient working set here.
    // Consumers must release retrieved tickets before destroying their queue.
    class FRepresentationLoadQueue
    {
      public:
        FRepresentationLoadQueue();
        ~FRepresentationLoadQueue();
        FRepresentationLoadQueue(const FRepresentationLoadQueue&) = delete;
        FRepresentationLoadQueue& operator=(const FRepresentationLoadQueue&) = delete;

        void SetStagingBudget(u64 bytes);
        ERepresentationRequestResult Request(u64 key, const FRepresentationDescriptor& descriptor,
                                             std::function<Ref<FRepresentationPayload>()> work);
        EAssetLoadCancelResult Cancel(u64 key);
        [[nodiscard]] bool IsPending(u64 key) const;
        [[nodiscard]] bool HasFailed(u64 key) const;
        void ClearFailure(u64 key);
        // Reservations survive retrieval: release the ticket only after dropping
        // or integrating its CPU payload. Cancellation never releases running work.
        void RetrieveCompleted(TArray<FCompletedRepresentationLoad>& outCompleted);
        void ReleaseStaging(u64 ticket);
        void ReapAbandoned();
        void Shutdown();
        [[nodiscard]] FRepresentationLoadStats GetStats() const;
        void SetStartGate(std::optional<Tasks::FTaskEvent> gate);

      private:
        struct FReservation
        {
            FRepresentationDescriptor Descriptor;
            u64 Ticket = 0;
        };
        using FLoadRecord = TCancellableLoadRecord<u64, Ref<FRepresentationPayload>, FReservation>;
        TCancellableLoadSet<u64, Ref<FRepresentationPayload>, FReservation> m_Loads;
        std::unordered_map<u64, FAssetByteSize> m_Reservations;
        std::unordered_set<u64> m_HeldCompletions;
        std::unordered_set<u64> m_Failed;
        std::optional<Tasks::FTaskEvent> m_StartGate;
        u64 m_StagingBytes = 0;
        u64 m_MaxStagingBytes = 0;
        u64 m_RejectedForStaging = 0;
        u64 m_FailedLoads = 0;
        bool m_Stopped = false;
    };

    struct FRepresentationStreamingStats
    {
        u64 OptionalResidentGpuBytes = 0;
        u64 PinnedResidentGpuBytes = 0;
        u64 RetiringGpuBytes = 0;
        u64 MaxResidentGpuBytes = 0;
        u64 MaxUploadBytesPerFrame = 0;
        u64 UploadedBytesThisFrame = 0;
        u64 PinnedUploadBytesThisFrame = 0;
        u64 DeniedForResident = 0;
        u64 DeniedForUpload = 0;
        u64 StagingCpuBytes = 0;
        u64 MaxStagingCpuBytes = 0;
        u32 UnknownStagingCount = 0;
        u64 DeniedForStaging = 0;
        u64 PreparationMicroseconds = 0;
        u64 ReadBytes = 0;
        u64 ReadMicroseconds = 0;
        u64 UnknownReadCount = 0;
        u64 CompletedLoads = 0;
        u64 FailedLoads = 0;
        u64 UploadMicroseconds = 0;
        u64 ActualUploadedBytes = 0;
        u32 PendingLoads = 0;
        u32 CompletedUnretrievedLoads = 0;
        u32 AbandonedRunningLoads = 0;
        u32 HeldCompletedLoads = 0;
    };

    // Logical optional-detail admission. RetiringGpuBytes must come from the
    // backend's physical allocation report; Release is not physical reclamation.
    // The pinned drawable floor is reported separately and cannot be evicted to
    // meet an optional-detail budget. UploadBytes counts transfer/build input,
    // GpuBytes counts logical GPU capacity: neither is added to CPU staging bytes.
    class FRepresentationStreamingBudget
    {
      public:
        void BeginFrame(u64 frame, u64 residentBudget, u64 uploadBudget);
        [[nodiscard]] bool TryAdmit(u64 key, const FRepresentationDescriptor& descriptor, bool pinned = false);
        // Correct an admitted allocation's measured backing after construction.
        // This always retains its increased charge, even above the configured
        // cap; it does not authorize allocating new storage. Missing keys fail.
        [[nodiscard]] bool ReconcileResidentGpuBytes(u64 key, u64 actualGpuBytes);
        void Release(u64 key);
        [[nodiscard]] bool IsResident(u64 key) const;
        // Refreshes the cheap backend retirement snapshot. A zero optional
        // residency budget is unlimited, including mandatory pinned floors.
        [[nodiscard]] bool IsUnderPressure();
        void SetRetiringGpuBytes(u64 bytes);
        // A frame-start snapshot misses evictions followed by new admission in
        // that frame. Production supplies a current backend report here.
        void SetRetiringGpuBytesProvider(std::function<u64()> provider);
        void SetStagingBudget(u64 bytes);
        [[nodiscard]] std::optional<u64> ReserveStaging(const FAssetByteSize& bytes);
        void ReleaseStaging(u64 ticket);
        // Measured creation/upload CPU wall cost, separate from admission
        // estimates and from CPU preparation (which includes source I/O).
        void RecordUpload(u64 bytes, u64 microseconds);
        // CPU texture preparation outside a load task reports actual reads here.
        void RecordIO(const FRepresentationIOStats& stats);
        [[nodiscard]] FRepresentationStreamingStats GetStats() const;
        void Reset();

      private:
        struct FCharge
        {
            u64 GpuBytes = 0;
            u64 UploadBytes = 0;
            bool Pinned = false;
        };
        friend class FRepresentationLoadQueue;
        void RegisterQueue(const FRepresentationLoadQueue* queue);
        void UnregisterQueue(const FRepresentationLoadQueue* queue);
        void RecordPreparation(u64 microseconds, const FAssetByteSize& diskBytes, bool success,
                               const std::optional<FRepresentationIOStats>& io);
        void RecomputeResidentTotals();
        std::unordered_map<u64, FCharge> m_Resident;
        std::unordered_map<u64, FAssetByteSize> m_Staging;
        std::unordered_set<const FRepresentationLoadQueue*> m_Queues;
        std::function<u64()> m_RetiringGpuBytesProvider;
        u64 m_NextStagingTicket = 1;
        FRepresentationStreamingStats m_Stats;
        std::optional<u64> m_Frame;
        bool m_OptionalResidentOverflow = false;
        bool m_PinnedResidentOverflow = false;
        // Workers only touch cumulative atomics. Registry/admission state above
        // remains main-thread-owned.
        std::atomic<u64> m_PreparationMicroseconds{ 0 };
        std::atomic<u64> m_ReadBytes{ 0 };
        std::atomic<u64> m_ReadMicroseconds{ 0 };
        std::atomic<u64> m_UnknownReadCount{ 0 };
        std::atomic<u64> m_CompletedLoads{ 0 };
        std::atomic<u64> m_FailedLoads{ 0 };
        std::atomic<u64> m_UploadMicroseconds{ 0 };
        std::atomic<u64> m_ActualUploadedBytes{ 0 };
    };

    namespace RepresentationStreaming
    {
        [[nodiscard]] FRepresentationStreamingBudget& Get();
    }
} // namespace OloEngine
