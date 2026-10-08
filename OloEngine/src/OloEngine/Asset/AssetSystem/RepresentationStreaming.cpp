#include "OloEnginePCH.h"
#include "RepresentationStreaming.h"
#include "OloEngine/Core/Log.h"

#include <limits>
#include <chrono>

namespace OloEngine
{
    namespace
    {
        [[nodiscard]] bool Fits(u64 committed, u64 added, u64 limit)
        {
            return added <= std::numeric_limits<u64>::max() - committed &&
                   (limit == 0 || (committed <= limit && added <= limit - committed));
        }
    } // namespace

    FRepresentationLoadQueue::FRepresentationLoadQueue()
    {
        RepresentationStreaming::Get().RegisterQueue(this);
    }

    FRepresentationLoadQueue::~FRepresentationLoadQueue()
    {
        Shutdown();
        RepresentationStreaming::Get().UnregisterQueue(this);
    }

    void FRepresentationLoadQueue::SetStagingBudget(u64 bytes)
    {
        m_MaxStagingBytes = bytes;
    }

    ERepresentationRequestResult FRepresentationLoadQueue::Request(
        u64 key, const FRepresentationDescriptor& descriptor, std::function<Ref<FRepresentationPayload>()> work)
    {
        ReapAbandoned();
        if (m_Stopped)
            return ERepresentationRequestResult::Stopped;
        if (IsPending(key))
            return ERepresentationRequestResult::AlreadyPending;
        if (HasFailed(key) || !work)
            return ERepresentationRequestResult::Failed;

        const auto bytes = descriptor.CpuBytes.GetBytes();
        if ((!bytes && m_MaxStagingBytes != 0) || (m_MaxStagingBytes != 0 && GetStats().UnknownStagingCount != 0) ||
            !Fits(m_StagingBytes, bytes.value_or(0), m_MaxStagingBytes))
        {
            ++m_RejectedForStaging;
            return ERepresentationRequestResult::StagingBudget;
        }
        const auto reservation = RepresentationStreaming::Get().ReserveStaging(descriptor.CpuBytes);
        if (!reservation)
        {
            ++m_RejectedForStaging;
            return ERepresentationRequestResult::StagingBudget;
        }
        const u64 ticket = *reservation;
        m_Reservations.emplace(ticket, descriptor.CpuBytes);
        m_StagingBytes += bytes.value_or(0);
        m_Loads.Launch("RepresentationLoad", key, FReservation{ descriptor, ticket }, [work = std::move(work), diskBytes = descriptor.DiskBytes, key]() -> Ref<FRepresentationPayload>
                       {
                           const auto started = std::chrono::steady_clock::now();
                           Ref<FRepresentationPayload> payload;
                           try
                           {
                               payload = work();
                           }
                           catch (...)
                           {
                               OLO_CORE_ERROR("CPU representation preparation threw for content {}", key);
                           }
                           const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - started).count();
                           RepresentationStreaming::Get().RecordPreparation(static_cast<u64>(elapsed), diskBytes,
                                                                             static_cast<bool>(payload),
                                                                             payload ? payload->GetIOStats() : std::nullopt);
                           return payload; }, m_StartGate);
        return ERepresentationRequestResult::Queued;
    }

    EAssetLoadCancelResult FRepresentationLoadQueue::Cancel(u64 key)
    {
        const auto* live = m_Loads.FindLive(key);
        const u64 ticket = live ? live->Payload.Ticket : 0;
        TArray<FLoadRecord> dropped;
        const auto result = m_Loads.Cancel(key, dropped);
        // The successful Queued -> Cancelled transition permanently excludes
        // preparation. Its gated task must still drain at shutdown, but cannot
        // allocate a working set or produce a payload, so its reservation is free.
        if (result == EAssetLoadCancelResult::CancelledBeforeStart)
            ReleaseStaging(ticket);
        for (FLoadRecord& record : dropped)
        {
            record.Task.GetResult().Reset();
            record.Task = {};
            ReleaseStaging(record.Payload.Ticket);
        }
        ReapAbandoned();
        return result;
    }

    bool FRepresentationLoadQueue::IsPending(u64 key) const
    {
        return m_Loads.FindLive(key) != nullptr;
    }

    bool FRepresentationLoadQueue::HasFailed(u64 key) const
    {
        return m_Failed.contains(key);
    }

    void FRepresentationLoadQueue::ClearFailure(u64 key)
    {
        m_Failed.erase(key);
    }

    void FRepresentationLoadQueue::RetrieveCompleted(TArray<FCompletedRepresentationLoad>& outCompleted)
    {
        ReapAbandoned();
        TArray<FLoadRecord> completed;
        m_Loads.ExtractCompleted(completed);
        for (FLoadRecord& record : completed)
        {
            Ref<FRepresentationPayload> payload = std::move(record.Task.GetResult());
            // The task stores its own result reference. Drop that reference
            // before rejecting the payload or handing ownership to the caller.
            record.Task = {};
            if (payload && record.Payload.Descriptor.CpuBytes.IsKnown() &&
                payload->GetCpuBytes() > *record.Payload.Descriptor.CpuBytes.GetBytes())
            {
                // The callback violated its reservation. Do not publish an
                // unbudgeted result or keep retrying the same content forever.
                OLO_CORE_ERROR("CPU representation {} exceeded its staging reservation ({} > {} bytes)",
                               record.Key, payload->GetCpuBytes(), *record.Payload.Descriptor.CpuBytes.GetBytes());
                RepresentationStreaming::Get().m_FailedLoads.fetch_add(1, std::memory_order_relaxed);
                payload = nullptr;
            }
            if (!payload)
            {
                m_Failed.insert(record.Key);
                ++m_FailedLoads;
                ReleaseStaging(record.Payload.Ticket);
                outCompleted.Add({ record.Key, 0, record.Payload.Descriptor, nullptr });
                continue;
            }
            m_HeldCompletions.insert(record.Payload.Ticket);
            outCompleted.Add({ record.Key, record.Payload.Ticket, record.Payload.Descriptor, std::move(payload) });
        }
    }

    void FRepresentationLoadQueue::ReleaseStaging(u64 ticket)
    {
        if (const auto it = m_Reservations.find(ticket); it != m_Reservations.end())
        {
            m_StagingBytes -= it->second.GetBytes().value_or(0);
            m_Reservations.erase(it);
            m_HeldCompletions.erase(ticket);
            RepresentationStreaming::Get().ReleaseStaging(ticket);
        }
    }

    void FRepresentationLoadQueue::ReapAbandoned()
    {
        TArray<FLoadRecord> dropped;
        m_Loads.ReapAbandoned(dropped);
        for (FLoadRecord& record : dropped)
        {
            record.Task.GetResult().Reset();
            record.Task = {};
            ReleaseStaging(record.Payload.Ticket);
        }
    }

    void FRepresentationLoadQueue::Shutdown()
    {
        if (m_Stopped)
            return;
        m_Stopped = true;
        m_Loads.CancelUnstarted();
        TArray<Tasks::TTask<Ref<FRepresentationPayload>>> tasks;
        m_Loads.CollectTasks(tasks);
        for (auto& task : tasks)
            task.Wait();
        tasks.Reset();
        TArray<FLoadRecord> dropped;
        m_Loads.Clear(dropped);
        for (FLoadRecord& record : dropped)
        {
            record.Task.GetResult().Reset();
            record.Task = {};
            ReleaseStaging(record.Payload.Ticket);
        }
    }

    FRepresentationLoadStats FRepresentationLoadQueue::GetStats() const
    {
        FRepresentationLoadStats stats;
        stats.StagingBytes = m_StagingBytes;
        for (const auto& [ticket, bytes] : m_Reservations)
        {
            (void)ticket;
            if (!bytes.IsKnown())
                ++stats.UnknownStagingCount;
        }
        stats.MaxStagingBytes = m_MaxStagingBytes;
        stats.PendingCount = static_cast<u32>(m_Loads.LiveCount());
        stats.CompletedUnretrievedCount = m_Loads.CompletedLiveCount();
        stats.AbandonedRunningCount = m_Loads.AbandonedRunningCount();
        stats.HeldCompletionCount = static_cast<u32>(m_HeldCompletions.size());
        stats.RejectedForStaging = m_RejectedForStaging;
        stats.FailedLoads = m_FailedLoads;
        stats.Cancellation = m_Loads.Counters();
        return stats;
    }

    void FRepresentationLoadQueue::SetStartGate(std::optional<Tasks::FTaskEvent> gate)
    {
        m_StartGate = std::move(gate);
    }

    void FRepresentationStreamingBudget::BeginFrame(u64 frame, u64 residentBudget, u64 uploadBudget)
    {
        m_Stats.MaxResidentGpuBytes = residentBudget;
        m_Stats.MaxUploadBytesPerFrame = uploadBudget;
        if (!m_Frame || *m_Frame != frame)
        {
            m_Frame = frame;
            m_Stats.UploadedBytesThisFrame = 0;
            m_Stats.PinnedUploadBytesThisFrame = 0;
        }
    }

    bool FRepresentationStreamingBudget::TryAdmit(u64 key, const FRepresentationDescriptor& descriptor, bool pinned)
    {
        if (m_RetiringGpuBytesProvider)
            m_Stats.RetiringGpuBytes = m_RetiringGpuBytesProvider();
        u64 gpuAdded = descriptor.GpuBytes;
        u64 uploadAdded = descriptor.UploadBytes;
        const auto existing = m_Resident.find(key);
        if (existing != m_Resident.end())
        {
            // A resident identity may grow when another entity shares its
            // stream. Admission checks the additional capacity before upload.
            if (existing->second.Pinned != pinned)
                return false;
            gpuAdded = descriptor.GpuBytes > existing->second.GpuBytes ? descriptor.GpuBytes - existing->second.GpuBytes : 0;
            uploadAdded = descriptor.UploadBytes > existing->second.UploadBytes ? descriptor.UploadBytes - existing->second.UploadBytes : 0;
        }
        if (!pinned &&
            (m_OptionalResidentOverflow ||
             !Fits(m_Stats.OptionalResidentGpuBytes, m_Stats.RetiringGpuBytes, m_Stats.MaxResidentGpuBytes) ||
             !Fits(m_Stats.OptionalResidentGpuBytes + m_Stats.RetiringGpuBytes, gpuAdded, m_Stats.MaxResidentGpuBytes)))
        {
            ++m_Stats.DeniedForResident;
            return false;
        }
        // Lowering the detail budget must demote already-resident detail too.
        // Unchanged resident data needs no upload, so exhausted frame transfer
        // allowance alone must not force a drawable representation off screen.
        if (existing != m_Resident.end() && gpuAdded == 0 && uploadAdded == 0)
            return true;
        if (!pinned && !Fits(m_Stats.UploadedBytesThisFrame, uploadAdded, m_Stats.MaxUploadBytesPerFrame))
        {
            ++m_Stats.DeniedForUpload;
            return false;
        }
        if ((pinned && m_PinnedResidentOverflow) ||
            !Fits(pinned ? m_Stats.PinnedResidentGpuBytes : m_Stats.OptionalResidentGpuBytes, gpuAdded, 0) ||
            !Fits(m_Stats.UploadedBytesThisFrame, uploadAdded, 0))
            return false;
        if (existing == m_Resident.end())
            m_Resident.emplace(key, FCharge{ descriptor.GpuBytes, descriptor.UploadBytes, pinned });
        else
        {
            existing->second.GpuBytes += gpuAdded;
            existing->second.UploadBytes += uploadAdded;
        }
        (pinned ? m_Stats.PinnedResidentGpuBytes : m_Stats.OptionalResidentGpuBytes) += gpuAdded;
        m_Stats.UploadedBytesThisFrame += uploadAdded;
        if (pinned)
            m_Stats.PinnedUploadBytesThisFrame += uploadAdded;
        return true;
    }

    bool FRepresentationStreamingBudget::ReconcileResidentGpuBytes(u64 key, u64 actualGpuBytes)
    {
        const auto existing = m_Resident.find(key);
        if (existing == m_Resident.end())
            return false;
        if (actualGpuBytes > existing->second.GpuBytes)
        {
            existing->second.GpuBytes = actualGpuBytes;
            RecomputeResidentTotals();
        }
        return true;
    }

    void FRepresentationStreamingBudget::RecomputeResidentTotals()
    {
        m_Stats.OptionalResidentGpuBytes = 0;
        m_Stats.PinnedResidentGpuBytes = 0;
        m_OptionalResidentOverflow = false;
        m_PinnedResidentOverflow = false;
        for (const auto& [key, charge] : m_Resident)
        {
            (void)key;
            auto& bytes = charge.Pinned ? m_Stats.PinnedResidentGpuBytes : m_Stats.OptionalResidentGpuBytes;
            auto& overflow = charge.Pinned ? m_PinnedResidentOverflow : m_OptionalResidentOverflow;
            if (!Fits(bytes, charge.GpuBytes, 0))
            {
                bytes = std::numeric_limits<u64>::max();
                overflow = true;
            }
            else
                bytes += charge.GpuBytes;
        }
    }

    void FRepresentationStreamingBudget::Release(u64 key)
    {
        if (const auto it = m_Resident.find(key); it != m_Resident.end())
        {
            const bool overflow = m_OptionalResidentOverflow || m_PinnedResidentOverflow;
            if (!overflow)
                (it->second.Pinned ? m_Stats.PinnedResidentGpuBytes : m_Stats.OptionalResidentGpuBytes) -= it->second.GpuBytes;
            m_Resident.erase(it);
            if (overflow)
                RecomputeResidentTotals();
        }
    }

    bool FRepresentationStreamingBudget::IsResident(u64 key) const
    {
        return m_Resident.contains(key);
    }

    bool FRepresentationStreamingBudget::IsUnderPressure()
    {
        if (m_RetiringGpuBytesProvider)
            m_Stats.RetiringGpuBytes = m_RetiringGpuBytesProvider();
        return m_OptionalResidentOverflow ||
               (m_Stats.MaxResidentGpuBytes != 0 &&
                !Fits(m_Stats.OptionalResidentGpuBytes, m_Stats.RetiringGpuBytes, m_Stats.MaxResidentGpuBytes));
    }

    void FRepresentationStreamingBudget::SetRetiringGpuBytes(u64 bytes)
    {
        m_Stats.RetiringGpuBytes = bytes;
    }

    void FRepresentationStreamingBudget::SetRetiringGpuBytesProvider(std::function<u64()> provider)
    {
        m_RetiringGpuBytesProvider = std::move(provider);
    }

    void FRepresentationStreamingBudget::SetStagingBudget(u64 bytes)
    {
        m_Stats.MaxStagingCpuBytes = bytes;
    }

    std::optional<u64> FRepresentationStreamingBudget::ReserveStaging(const FAssetByteSize& bytes)
    {
        const auto known = bytes.GetBytes();
        if (m_NextStagingTicket == 0 || (!known && m_Stats.MaxStagingCpuBytes != 0) ||
            (m_Stats.UnknownStagingCount != 0 && m_Stats.MaxStagingCpuBytes != 0) ||
            !Fits(m_Stats.StagingCpuBytes, known.value_or(0), m_Stats.MaxStagingCpuBytes))
        {
            ++m_Stats.DeniedForStaging;
            return std::nullopt;
        }
        const u64 ticket = m_NextStagingTicket++;
        m_Staging.emplace(ticket, bytes);
        m_Stats.StagingCpuBytes += known.value_or(0);
        if (!known)
            ++m_Stats.UnknownStagingCount;
        return ticket;
    }

    void FRepresentationStreamingBudget::ReleaseStaging(u64 ticket)
    {
        if (const auto it = m_Staging.find(ticket); it != m_Staging.end())
        {
            m_Stats.StagingCpuBytes -= it->second.GetBytes().value_or(0);
            if (!it->second.IsKnown())
                --m_Stats.UnknownStagingCount;
            m_Staging.erase(it);
        }
    }

    void FRepresentationStreamingBudget::RegisterQueue(const FRepresentationLoadQueue* queue)
    {
        m_Queues.insert(queue);
    }

    void FRepresentationStreamingBudget::UnregisterQueue(const FRepresentationLoadQueue* queue)
    {
        m_Queues.erase(queue);
    }

    void FRepresentationStreamingBudget::RecordPreparation(u64 microseconds, const FAssetByteSize& diskBytes, bool success,
                                                           const std::optional<FRepresentationIOStats>& io)
    {
        m_PreparationMicroseconds.fetch_add(microseconds, std::memory_order_relaxed);
        m_CompletedLoads.fetch_add(1, std::memory_order_relaxed);
        if (!success)
        {
            m_FailedLoads.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (io)
            RecordIO(*io);
        else if (diskBytes.IsActual())
            m_ReadBytes.fetch_add(*diskBytes.GetBytes(), std::memory_order_relaxed);
        else
            m_UnknownReadCount.fetch_add(1, std::memory_order_relaxed);
    }

    void FRepresentationStreamingBudget::RecordIO(const FRepresentationIOStats& stats)
    {
        m_ReadBytes.fetch_add(stats.ReadBytes, std::memory_order_relaxed);
        m_ReadMicroseconds.fetch_add(stats.ReadMicroseconds, std::memory_order_relaxed);
    }

    void FRepresentationStreamingBudget::RecordUpload(u64 bytes, u64 microseconds)
    {
        m_ActualUploadedBytes.fetch_add(bytes, std::memory_order_relaxed);
        m_UploadMicroseconds.fetch_add(microseconds, std::memory_order_relaxed);
    }

    FRepresentationStreamingStats FRepresentationStreamingBudget::GetStats() const
    {
        auto stats = m_Stats;
        // Reports may be queried by renderer capacity reporters. Invoking the
        // physical provider here could recursively enter those reporters. This
        // snapshot is refreshed by admission (or SetRetiringGpuBytes).
        stats.PreparationMicroseconds = m_PreparationMicroseconds.load(std::memory_order_relaxed);
        stats.ReadBytes = m_ReadBytes.load(std::memory_order_relaxed);
        stats.ReadMicroseconds = m_ReadMicroseconds.load(std::memory_order_relaxed);
        stats.UnknownReadCount = m_UnknownReadCount.load(std::memory_order_relaxed);
        stats.CompletedLoads = m_CompletedLoads.load(std::memory_order_relaxed);
        stats.FailedLoads = m_FailedLoads.load(std::memory_order_relaxed);
        stats.UploadMicroseconds = m_UploadMicroseconds.load(std::memory_order_relaxed);
        stats.ActualUploadedBytes = m_ActualUploadedBytes.load(std::memory_order_relaxed);
        for (const FRepresentationLoadQueue* queue : m_Queues)
        {
            const auto loads = queue->GetStats();
            stats.PendingLoads += loads.PendingCount;
            stats.CompletedUnretrievedLoads += loads.CompletedUnretrievedCount;
            stats.AbandonedRunningLoads += loads.AbandonedRunningCount;
            stats.HeldCompletedLoads += loads.HeldCompletionCount;
        }
        return stats;
    }

    void FRepresentationStreamingBudget::Reset()
    {
        m_Resident.clear();
        m_OptionalResidentOverflow = false;
        m_PinnedResidentOverflow = false;
        m_RetiringGpuBytesProvider = {};
        // Scene teardown can reset renderer charges while an abandoned worker
        // still runs. Its staging reservation must survive until the queue drains.
        const u64 stagingBytes = m_Stats.StagingCpuBytes;
        const u64 stagingBudget = m_Stats.MaxStagingCpuBytes;
        const u32 unknownStagingCount = m_Stats.UnknownStagingCount;
        m_Stats = {};
        m_Stats.StagingCpuBytes = stagingBytes;
        m_Stats.MaxStagingCpuBytes = stagingBudget;
        m_Stats.UnknownStagingCount = unknownStagingCount;
        m_Frame.reset();
        m_PreparationMicroseconds.store(0, std::memory_order_relaxed);
        m_ReadBytes.store(0, std::memory_order_relaxed);
        m_ReadMicroseconds.store(0, std::memory_order_relaxed);
        m_UnknownReadCount.store(0, std::memory_order_relaxed);
        m_CompletedLoads.store(0, std::memory_order_relaxed);
        m_FailedLoads.store(0, std::memory_order_relaxed);
        m_UploadMicroseconds.store(0, std::memory_order_relaxed);
        m_ActualUploadedBytes.store(0, std::memory_order_relaxed);
    }

    FRepresentationStreamingBudget& RepresentationStreaming::Get()
    {
        // Renderer statics may destroy registered queues after a lazily-created
        // singleton's destruction point. Match RendererMemoryTracker's process
        // lifetime: queue teardown must always have a live reservation ledger.
        // This service owns no GPU resources or preparation payloads.
        static auto* budget = new FRepresentationStreamingBudget();
        return *budget;
    }
} // namespace OloEngine
