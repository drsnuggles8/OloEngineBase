#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/GroomRenderPass.h"

#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/Texture3D.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <algorithm>
#include <bit>
#include <cstdint>

namespace OloEngine
{
    GroomRenderPass::~GroomRenderPass()
    {
        OnReset();
    }

    namespace
    {
        [[nodiscard]] u64 StreamingGeometryKey(const GroomStrandRequest& request)
        {
            // Namespace this consumer's charge and include held CPU identities:
            // a hot reload under the same handle cannot reuse an old preparation.
            u64 key = 0x125747524f4f4dull;
            const auto mix = [&key](u64 value)
            { key = (key ^ value) * 1099511628211ull; };
            mix(static_cast<u64>(request.Handle));
            mix(reinterpret_cast<uintptr_t>(request.Groom.Raw()));
            mix(reinterpret_cast<uintptr_t>(request.Binding.Raw()));
            mix(reinterpret_cast<uintptr_t>(request.LodLevel));
            mix(request.Build.CoatDigest);
            mix(request.Build.MaxStrands);
            mix(request.Build.MaxSegments);
            mix(request.Build.GuidesOnly ? 1u : 0u);
            mix(std::bit_cast<u32>(request.Build.MaxWidthCompensation));
            mix(std::to_underlying(request.Lod.Representation));
            return key;
        }
    } // namespace

    void GroomRenderPass::BeginStreamingFrame(u64 frame, bool enabled, u32 fallbackStrands)
    {
        if (frame == m_StreamingFrame && m_StreamingEnabled == enabled)
        {
            return;
        }
        m_StreamingFrame = frame;
        m_StreamingEnabled = enabled;
        m_StreamingFallbackStrands = std::clamp(fallbackStrands, 1u, 65536u);
        const u64 cancelled = m_StreamingStats.Cancelled;
        const u64 evictions = m_StreamingStats.Evictions;
        m_StreamingStats = {};
        m_StreamingStats.Cancelled = cancelled;
        m_StreamingStats.Evictions = evictions;
        m_StreamingBaseAssets.clear();
        m_StreamingBaseBindings.clear();
        m_StreamingLoads.ReapAbandoned();
        TArray<FCompletedRepresentationLoad> completed;
        m_StreamingLoads.RetrieveCompleted(completed);
        for (auto& result : completed)
        {
            if (auto found = m_StreamingDetails.find(result.Key); found != m_StreamingDetails.end() && result.Payload)
            {
                found->second.Prepared = result.Payload.As<FPreparedGroomGeometry>();
                result.Payload = nullptr;
                found->second.StagingTicket = result.StagingTicket;
                found->second.Descriptor = result.Descriptor;
                // Charge the actual emitted buffers, including the caster's
                // second index order; predictions only bound worker staging.
                const auto& stats = found->second.Prepared->Stats;
                found->second.RootCount = found->second.Binding
                                              ? static_cast<u32>(found->second.Prepared->RootCurves.Num())
                                              : stats.StrandsSelected;
                found->second.Descriptor.UploadBytes = stats.VertexBytes + stats.IndexBytes * 2u;
                // Three immutable buffers: retain alignment headroom in the
                // admission envelope, then reconcile against tracked backing.
                found->second.Descriptor.GpuBytes = found->second.Descriptor.UploadBytes + 3u * 65536u;
            }
            else
            {
                // An orphan completion still owns its prepared output here.
                // Drop that last reference before reopening the staging allowance.
                result.Payload = nullptr;
                m_StreamingLoads.ReleaseStaging(result.StagingTicket);
            }
        }
    }

    void GroomRenderPass::ResolveStreamingRequest(GroomStrandRequest& request)
    {
        if (!m_StreamingEnabled || !request.Groom)
        {
            return;
        }
        request.StreamingEnabled = true;
        request.StreamingRequestedRepresentation = request.Lod.Representation;
        request.StreamingRequestedStrands = request.Build.MaxStrands;
        ++m_StreamingStats.Requested;
        if (m_StreamingBaseAssets.emplace(request.Groom.Raw(), request.Groom).second)
        {
            m_StreamingStats.BaseCpuBytes += request.Groom->GetCpuMemoryBytes();
        }
        if (request.Binding && m_StreamingBaseBindings.emplace(request.Binding.Raw(), request.Binding).second)
        {
            m_StreamingStats.BaseCpuBytes += request.Binding->GetCpuMemoryBytes();
        }
        const u64 desiredKey = StreamingGeometryKey(request);
        GroomStrandRequest floor = request;
        ApplyGroomStreamingFloor(floor, m_StreamingFallbackStrands);
        const u64 floorKey = StreamingGeometryKey(floor);
        const bool desiredIsFloor = desiredKey == floorKey;
        auto& budget = RepresentationStreaming::Get();

        bool detail = false;
        if (!desiredIsFloor && (!request.Binding || m_GpuDeformation))
        {
            auto& entry = m_StreamingDetails[desiredKey];
            entry.Groom = request.Groom;
            entry.Binding = request.Binding;
            entry.LastWanted = m_StreamingFrame;
            const bool pressure = budget.IsUnderPressure();
            auto resident = entry.Descriptor;
            resident.UploadBytes = 0;
            if (entry.Admitted && budget.IsResident(desiredKey) && !pressure)
            {
                detail = true;
            }
            else if (entry.Prepared && budget.TryAdmit(desiredKey, resident))
            {
                entry.Admitted = true;
                detail = true;
            }
            else
            {
                auto queued = ERepresentationRequestResult::AlreadyPending;
                if (!entry.Prepared && !entry.Admitted && !m_StreamingLoads.IsPending(desiredKey) &&
                    !m_StreamingLoads.HasFailed(desiredKey))
                {
                    entry.Descriptor = DescribeGroomGeometry(request);
                    const Ref<GroomAsset> groom = request.Groom;
                    const Ref<GroomBindingAsset> binding = request.Binding;
                    const GroomLodLevel* level = request.LodLevel;
                    const auto build = request.Build;
                    const auto coat = request.Coat;
                    queued = m_StreamingLoads.Request(desiredKey, entry.Descriptor, [groom, binding, level, build, coat]()
                                                      { return PrepareGroomGeometry(groom, binding, level, build, coat).As<FRepresentationPayload>(); });
                }
                request.StreamingPending = m_StreamingLoads.IsPending(desiredKey);
                const auto budgetStats = budget.GetStats();
                const u64 capacity = budgetStats.MaxResidentGpuBytes;
                const u64 added = entry.Admitted && budget.IsResident(desiredKey) ? 0u : entry.Descriptor.GpuBytes;
                // Subtract only after each term fits: a huge deferred-retirement
                // snapshot cannot wrap a projected sum and masquerade as upload pressure.
                const bool residentDenied = capacity != 0 &&
                                            (budgetStats.RetiringGpuBytes > capacity ||
                                             budgetStats.OptionalResidentGpuBytes > capacity - budgetStats.RetiringGpuBytes ||
                                             added > capacity - budgetStats.RetiringGpuBytes - budgetStats.OptionalResidentGpuBytes);
                request.StreamingFallback = request.StreamingPending                                ? EGroomStreamingFallback::Pending
                                            : m_StreamingLoads.HasFailed(desiredKey)                ? EGroomStreamingFallback::PreparationFailed
                                            : queued == ERepresentationRequestResult::StagingBudget ? EGroomStreamingFallback::StagingBudget
                                            : residentDenied                                        ? EGroomStreamingFallback::ResidentBudget
                                                                                                    : EGroomStreamingFallback::UploadBudget;
                m_StreamingStats.Pending += request.StreamingPending ? 1u : 0u;
            }
        }
        else if (!desiredIsFloor)
        {
            request.StreamingFallback = EGroomStreamingFallback::CpuDeformationReference;
        }
        if (detail)
        {
            auto& entry = m_StreamingDetails.at(desiredKey);
            if (!entry.Integrated &&
                (entry.UploadAdmittedFrame != m_StreamingFrame || entry.UploadAdmittedTick != m_CacheTick))
            {
                // Entity admission can defer integration after shared capacity
                // was admitted. Until the immutable buffers exist, reserve their
                // transfer in the frame that may actually construct them.
                auto transfer = entry.Descriptor;
                transfer.GpuBytes = 0;
                const u64 uploadKey = desiredKey ^ 0x1257534841524544ull;
                if (budget.TryAdmit(uploadKey, transfer))
                {
                    budget.Release(uploadKey);
                    entry.UploadAdmittedFrame = m_StreamingFrame;
                    entry.UploadAdmittedTick = m_CacheTick;
                }
                else
                {
                    detail = false;
                    request.StreamingFallback = EGroomStreamingFallback::UploadBudget;
                }
            }
        }
        if (detail && !AdmitStreamingEntity(request, desiredKey, false))
        {
            detail = false;
        }
        if (detail)
        {
            request.StreamingKey = desiredKey;
            ++m_StreamingStats.DetailDraws;
        }
        else
        {
            const auto reason = request.StreamingFallback;
            const bool pending = request.StreamingPending;
            request = std::move(floor);
            request.StreamingKey = floorKey;
            request.StreamingFallback = reason;
            request.StreamingPending = pending;
            (void)AdmitStreamingEntity(request, floorKey, true);
            auto& entry = m_StreamingDetails[floorKey];
            entry.Groom = request.Groom;
            entry.Binding = request.Binding;
            entry.Floor = true;
            entry.LastWanted = m_StreamingFrame;
            if (!entry.Admitted)
            {
                entry.Descriptor = DescribeGroomGeometry(request);
                entry.Admitted = budget.TryAdmit(floorKey, entry.Descriptor, true);
            }
            ++m_StreamingStats.FallbackDraws;
        }
    }

    bool GroomRenderPass::AdmitStreamingEntity(GroomStrandRequest& request, u64 streamKey, bool floor)
    {
        if (!request.Binding || !m_GpuDeformation)
        {
            return true;
        }
        const u64 key = (streamKey ^ (static_cast<u64>(static_cast<u32>(request.EntityID)) << 32u) ^
                         0x12574445464f524dull) *
                        1099511628211ull;
        auto& entry = m_StreamingDetails[key];
        entry.Groom = request.Groom;
        entry.Binding = request.Binding;
        entry.EntityState = true;
        entry.Floor = floor;
        entry.LastWanted = m_StreamingFrame;
        u32 roots = std::min(request.Build.MaxStrands, request.BuildSource().Curves.GetCurveCount());
        if (const auto shared = m_StreamingDetails.find(streamKey);
            shared != m_StreamingDetails.end() && shared->second.RootCount != 0)
        {
            roots = shared->second.RootCount;
        }
        const auto layout = GroomDeformBufferLayout::Make(roots, request.StreamingGuideSlots,
                                                          request.StreamingDisplacementPoints,
                                                          request.StreamingSurfaceVertices, request.StreamingBones);
        FRepresentationDescriptor descriptor;
        descriptor.CpuBytes = FAssetByteSize::Actual(0);
        descriptor.DiskBytes = FAssetByteSize::Actual(0);
        descriptor.GpuBytes = std::max(layout.TotalBytes() + 65536u, entry.Descriptor.GpuBytes);
        // Capacity admission does not consume transfer twice. A separate
        // transient charge spends this frame's upload and is released at once;
        // the governor retains its per-frame upload tally after Release.
        auto& budget = RepresentationStreaming::Get();
        const bool wasResident = entry.Admitted;
        u64 layoutKey = layout.TotalBytes();
        for (const u64 value : { static_cast<u64>(roots), static_cast<u64>(request.StreamingGuideSlots),
                                 static_cast<u64>(request.StreamingDisplacementPoints),
                                 static_cast<u64>(request.StreamingSurfaceVertices),
                                 static_cast<u64>(request.StreamingBones), request.GpuRootSurfaceKey,
                                 static_cast<u64>(reinterpret_cast<uintptr_t>(request.Influence.Raw())) })
        {
            layoutKey = (layoutKey ^ value) * 1099511628211ull;
        }
        entry.LayoutKey = layoutKey;
        if (!budget.TryAdmit(key, descriptor, floor))
        {
            request.StreamingFallback = EGroomStreamingFallback::ResidentBudget;
            return false;
        }
        entry.Admitted = true;
        entry.Descriptor = descriptor;
        if (entry.UploadAdmittedFrame != m_StreamingFrame || entry.UploadAdmittedTick != m_CacheTick)
        {
            // Include CPU root updates even with a planned GPU root kernel:
            // unavailable compute can choose the CPU root fallback at integration.
            const u64 dynamic = static_cast<u64>(roots) * sizeof(GroomDeformRootRecord) +
                                static_cast<u64>(request.StreamingGuideSlots) * sizeof(GroomDeformSlotRecord) +
                                static_cast<u64>(request.StreamingDisplacementPoints) * sizeof(GroomDeformDisplacementRecord) +
                                static_cast<u64>(layout.BoneCount) * sizeof(glm::mat4) * 2u;
            descriptor.GpuBytes = 0;
            descriptor.UploadBytes = entry.Integrated && entry.IntegratedLayoutKey == layoutKey ? dynamic : layout.TotalBytes();
            const u64 uploadKey = key ^ 0x125755504c4f4144ull;
            if (!budget.TryAdmit(uploadKey, descriptor, floor))
            {
                request.StreamingFallback = EGroomStreamingFallback::UploadBudget;
                if (!wasResident)
                {
                    budget.Release(key);
                    entry.Admitted = false;
                }
                return false;
            }
            budget.Release(uploadKey);
            entry.UploadAdmittedFrame = m_StreamingFrame;
            entry.UploadAdmittedTick = m_CacheTick;
        }
        request.StreamingEntityKey = key;
        return true;
    }

    Ref<FPreparedGroomGeometry> GroomRenderPass::PreparedStreamingGeometry(const GroomStrandRequest& request) const
    {
        if (const auto found = m_StreamingDetails.find(request.StreamingKey); found != m_StreamingDetails.end())
        {
            const auto& prepared = found->second.Prepared;
            return prepared && prepared->Groom == request.Groom && prepared->Binding == request.Binding ? prepared : nullptr;
        }
        return nullptr;
    }

    void GroomRenderPass::CompleteStreamingUpload(u64 key, u64 uploadBytes, u64 uploadMicroseconds, u64 physicalBytes, u32 roots)
    {
        if (const auto found = m_StreamingDetails.find(key); found != m_StreamingDetails.end())
        {
            auto& entry = found->second;
            entry.Integrated = true;
            entry.RootCount = roots;
            RepresentationStreaming::Get().RecordUpload(uploadBytes, uploadMicroseconds);
            if (physicalBytes > entry.Descriptor.GpuBytes)
            {
                entry.Descriptor.GpuBytes = physicalBytes;
                (void)RepresentationStreaming::Get().ReconcileResidentGpuBytes(key, physicalBytes);
            }
            found->second.Prepared = nullptr;
            m_StreamingLoads.ReleaseStaging(found->second.StagingTicket);
            found->second.StagingTicket = 0;
        }
    }

    void GroomRenderPass::CompleteStreamingEntityUpload(u64 key, u64 physicalBytes, bool integrated)
    {
        if (const auto found = m_StreamingDetails.find(key); found != m_StreamingDetails.end())
        {
            auto& entry = found->second;
            // Creation already happened under a conservative admission envelope.
            // If backing exceeds it, charge the actual live bytes even under
            // pressure; later admission/retirement must never lose that ownership.
            if (physicalBytes > entry.Descriptor.GpuBytes)
            {
                entry.Descriptor.GpuBytes = physicalBytes;
                (void)RepresentationStreaming::Get().ReconcileResidentGpuBytes(key, physicalBytes);
            }
            entry.Integrated = integrated;
            entry.IntegratedLayoutKey = entry.LayoutKey;
        }
    }

    void GroomRenderPass::FinishStreamingFrame()
    {
        auto& budget = RepresentationStreaming::Get();
        if (m_StreamingEnabled)
        {
            // Enabling residency on an already-open eager scene must retire
            // its previous full streams too. The old soft cache cap otherwise
            // retains them indefinitely underneath the new optional budget.
            for (auto cached = m_Cache.begin(); cached != m_Cache.end();)
            {
                if (cached->second.StreamingKey == 0 && cached->second.LastUsedFrame != m_CacheTick)
                {
                    m_CacheBytes -= std::min(m_CacheBytes, cached->second.Bytes + cached->second.CoatBytes);
                    cached = m_Cache.erase(cached);
                    ++m_StreamingStats.Evictions;
                }
                else
                {
                    ++cached;
                }
            }
            PruneRestStreams();
        }
        for (auto it = m_StreamingDetails.begin(); it != m_StreamingDetails.end();)
        {
            const bool pressure = budget.IsUnderPressure();
            const bool used = m_StreamingEnabled && it->second.LastWanted == m_StreamingFrame;
            if (used && (it->second.Floor || !pressure))
            {
                ++it;
                continue;
            }
            const u64 key = it->first;
            if (m_StreamingLoads.IsPending(key))
            {
                (void)m_StreamingLoads.Cancel(key);
                ++m_StreamingStats.Cancelled;
            }
            it->second.Prepared = nullptr;
            m_StreamingLoads.ReleaseStaging(it->second.StagingTicket);
            it->second.StagingTicket = 0;
            // Frame lists borrow raw CPU run pointers. Eviction happens only
            // after both shadow and beauty consumers finished recording.
            for (auto cached = m_Cache.begin(); cached != m_Cache.end();)
            {
                if ((cached->second.StreamingKey != key && cached->second.StreamingEntityKey != key) ||
                    cached->second.LastUsedFrame == m_CacheTick)
                {
                    ++cached;
                    continue;
                }
                m_CacheBytes -= std::min(m_CacheBytes, cached->second.Bytes + cached->second.CoatBytes);
                cached = m_Cache.erase(cached);
                ++m_StreamingStats.Evictions;
            }
            // A still-used entry must remain charged until a later frame. Its
            // buffers will retire through the backend's completed-generation queue.
            const bool held = std::any_of(m_Cache.begin(), m_Cache.end(), [key](const auto& pair)
                                          { return pair.second.StreamingKey == key || pair.second.StreamingEntityKey == key; });
            if (held)
            {
                ++it;
                continue;
            }
            for (auto rest = m_RestStreams.begin(); rest != m_RestStreams.end();)
            {
                if (rest->second && rest->second->StreamingKey == key && rest->second->GetRefCount() <= 1u)
                {
                    m_CacheBytes -= std::min(m_CacheBytes, rest->second->Bytes);
                    rest = m_RestStreams.erase(rest);
                }
                else
                {
                    ++rest;
                }
            }
            budget.Release(key);
            it = m_StreamingDetails.erase(it);
        }
        m_StreamingStats.OptionalGpuBytes = 0;
        m_StreamingStats.FloorGpuBytes = 0;
        for (const auto& [key, entry] : m_StreamingDetails)
        {
            if (entry.Admitted)
            {
                (entry.Floor ? m_StreamingStats.FloorGpuBytes : m_StreamingStats.OptionalGpuBytes) += entry.Descriptor.GpuBytes;
            }
        }
    }
} // namespace OloEngine
