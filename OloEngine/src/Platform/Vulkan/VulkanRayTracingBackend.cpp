#include "OloEnginePCH.h"
#include "Platform/Vulkan/VulkanRayTracingBackend.h"

#if OLO_WITH_VULKAN

#include "OloEngine/Memory/AlignmentTemplates.h"
#include "Platform/Vulkan/VulkanDeferredReclaim.h"
#include "Platform/Vulkan/VulkanFrameArena.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"
#include "Platform/Vulkan/VulkanTransientUpload.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace OloEngine::RayTracing
{
    namespace
    {
        // VUID-VkAccelerationStructureCreateInfoKHR-offset-03734: an AS's
        // offset within its buffer must be a multiple of 256. We give each AS
        // its own buffer at offset 0, so this is the buffer's own alignment.
        constexpr VkDeviceSize kAccelerationStructureAlignment = 256;

        // Instance data for a TLAS build must be 16-byte aligned
        // (VUID-vkCmdBuildAccelerationStructuresKHR-pInfos-03715).
        constexpr VkDeviceSize kInstanceDataAlignment = 16;

        // One device-local buffer, created with an explicit alignment. Plain
        // vmaCreateBuffer honours only the buffer's own memory requirement,
        // which is how a suballocation lands 16-aligned under a 256-byte
        // requirement — the bug VulkanResourceHeap already paid for once.
        struct DeviceBuffer
        {
            VkBuffer Buffer = VK_NULL_HANDLE;
            VmaAllocation Allocation = VK_NULL_HANDLE;
            VkDeviceSize Size = 0;
            VkDeviceAddress Address = 0;

            [[nodiscard]] bool IsValid() const
            {
                return Buffer != VK_NULL_HANDLE;
            }
        };

        [[nodiscard]] bool CreateDeviceBuffer(VulkanDevice& device, VkDeviceSize size, VkBufferUsageFlags usage,
                                              VkDeviceSize alignment, const char* what, DeviceBuffer& out)
        {
            VkBufferCreateInfo bufferInfo{};
            bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.size = std::max<VkDeviceSize>(size, 1);
            bufferInfo.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            // Pure device-local: no HOST_ACCESS flags at all. Scratch and AS
            // storage are GPU-write-heavy, and a ReBAR placement would be the
            // worst possible home for them.
            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
            allocInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

            DeviceBuffer created{};
            if (vmaCreateBufferWithAlignment(device.GetAllocator(), &bufferInfo, &allocInfo, alignment, &created.Buffer,
                                             &created.Allocation, nullptr) != VK_SUCCESS)
            {
                OLO_CORE_ERROR("[RayTracing/Vulkan] {} allocation failed ({} bytes)", what, size);
                return false;
            }
            vmaSetAllocationName(device.GetAllocator(), created.Allocation, what);

            VkBufferDeviceAddressInfo addressInfo{};
            addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
            addressInfo.buffer = created.Buffer;
            created.Address = vkGetBufferDeviceAddress(device.GetDevice(), &addressInfo);
            created.Size = bufferInfo.size;
            if (created.Address == 0u)
            {
                // A zero device address is a GPU page fault waiting to happen,
                // not a value to assert on in Debug and ignore in Release.
                OLO_CORE_ERROR("[RayTracing/Vulkan] {} returned a null device address", what);
                VulkanDeferredReclaim::Get().Enqueue(created.Buffer, created.Allocation);
                return false;
            }
            out = created;
            return true;
        }

        void RetireDeviceBuffer(DeviceBuffer& buffer)
        {
            if (buffer.Buffer != VK_NULL_HANDLE)
            {
                VulkanDeferredReclaim::Get().Enqueue(buffer.Buffer, buffer.Allocation);
            }
            buffer = DeviceBuffer{};
        }
    } // namespace

    // -------------------------------------------------------------------------

    class VulkanRayTracingBackend final : public IRayTracingBackend
    {
      public:
        VulkanRayTracingBackend()
        {
            RefreshCapabilities();
        }

        ~VulkanRayTracingBackend() override
        {
            Shutdown();
        }

        [[nodiscard]] Capabilities GetCapabilities() const override
        {
            return m_Capabilities;
        }

        u32 RecordBlasBuilds(std::span<const BlasBuildRequest> requests) override;
        void RetireBlas(const GeometryKey& key) override;
        [[nodiscard]] bool IsBlasResident(const GeometryKey& key) const override;
        TlasBuildReason RecordTlasBuild(std::span<const InstanceRecord> instances, TlasBuildReason requested) override;
        [[nodiscard]] u64 GetTlasDeviceAddress() const override;
        void RecordBuildToReadBarrier() override;
        void PublishStats(SceneStats& stats) const override;
        void Shutdown() override;

      private:
        struct BlasEntry
        {
            VkAccelerationStructureKHR Handle = VK_NULL_HANDLE;
            DeviceBuffer Storage;
            VkDeviceAddress Address = 0;
            VkDeviceSize BuiltSize = 0;     ///< Size the build was sized for.
            VkDeviceSize CompactedSize = 0; ///< 0 until the query resolves.
            GeometryClass Class = GeometryClass::Unsupported;
            bool AllowsUpdate = false;
            // The structure was CREATED and a build command was actually
            // recorded for it. Creation alone is not enough: if scratch
            // allocation fails after the handles exist, an unbuilt structure
            // would otherwise be reported resident and a TLAS instance would
            // reference uninitialised memory — undefined, and silent.
            bool Built = false;
            // Compaction is a multi-frame handshake so nothing ever waits:
            // NotRequested -> SizeQueryPending -> ReadyToCompact -> Compacted.
            enum class Compaction : u8
            {
                NotRequested,
                SizeQueryPending,
                Compacted,
            } CompactionState = Compaction::NotRequested;
            u32 QuerySlot = std::numeric_limits<u32>::max();
            // Which recording generation stamped the size query. A query may
            // not be READ until its vkCmdResetQueryPool has actually
            // EXECUTED — reading an uninitialised query is
            // VUID-vkGetQueryPoolResults-None-09401, and WITH_AVAILABILITY
            // does not excuse it: availability answers "has the write
            // finished", not "was this slot ever reset". The reset is recorded
            // into the same command buffer as the build, so the earliest legal
            // read is a later frame.
            u64 QueryRecordedAt = 0;
        };

        void RefreshCapabilities();
        [[nodiscard]] VkCommandBuffer AcquireCommandBuffer();
        [[nodiscard]] bool EnsureScratch(VkDeviceSize bytes);
        void EnsureQueryPool();
        void ResolveFinishedCompactions(VkCommandBuffer cmd);
        void DestroyBlasEntry(BlasEntry& entry);
        [[nodiscard]] u32 AcquireQuerySlot();

        Capabilities m_Capabilities{};
        std::unordered_map<GeometryKey, BlasEntry, GeometryKeyHash> m_Blas;

        DeviceBuffer m_Scratch;
        VkDeviceSize m_ScratchAlignment = 256;

        VkQueryPool m_CompactionQueryPool = VK_NULL_HANDLE;
        static constexpr u32 kCompactionQuerySlots = 256;
        std::vector<GeometryKey> m_QuerySlotOwner;
        std::vector<u32> m_FreeQuerySlots;

        VkAccelerationStructureKHR m_Tlas = VK_NULL_HANDLE;
        DeviceBuffer m_TlasStorage;
        // Finding: one persistent instance buffer rewritten every frame is the
        // cross-frame version of the last-write-wins archetype
        // (vulkan-command-ordered-buffer-writes.md). With two frames in
        // flight, frame N's copy can execute while frame N-1's TLAS build is
        // still reading, and an intra-command-buffer barrier says nothing
        // about that. A ring of kFramesInFlight buffers gives frame N and
        // frame N-1 different memory; frame N and N-2 share, and N-2's fence
        // has been waited by then — the same contract deferred reclaim uses.
        static constexpr u32 kFramesInFlight = 2;
        std::array<DeviceBuffer, kFramesInFlight> m_TlasInstances;
        VkDeviceAddress m_TlasAddress = 0;
        u32 m_TlasCapacity = 0;
        // The primitiveCount the CURRENT TLAS was last fully built with. An
        // update must use exactly this value
        // (VUID-vkCmdBuildAccelerationStructuresKHR-pInfos-03407): a refit
        // with a different count is invalid usage that leaves the structure
        // undefined, and shrinking hits it just as readily as growing.
        u32 m_TlasBuiltPrimitiveCount = 0;

        // Set whenever a build was recorded this frame, cleared by the
        // barrier. A read without a preceding barrier is the silent,
        // intermittent failure the RenderGraph hazard exists to prevent, so
        // the barrier is emitted whenever anything was built even if the graph
        // forgets to ask.
        bool m_BuildsPendingBarrier = false;

        u64 m_CompactionSavedBytes = 0;
        u32 m_FrameCompactions = 0;
        // Counted HERE, not by the caller. RecordBlasBuilds returns a bare
        // count while skipping arbitrary entries (a zero-size query, a failed
        // allocation), so crediting m_PendingBuilds[0..recorded) positionally
        // attributes the wrong reasons whenever anything was skipped. Only
        // this class knows which requests actually reached the command buffer.
        u32 m_FrameBlasBuilds = 0;
        u32 m_FrameBlasRefits = 0;
        // Advances once per RecordBlasBuilds call, i.e. once per frame that
        // reaches the RT scene. Only used to hold a compacted-size query back
        // until the command buffer that reset it has retired — the same
        // frames-in-flight separation VulkanDeferredReclaim uses, for the same
        // reason.
        u64 m_RecordGeneration = 0;
        static constexpr u64 kQueryReadDelayFrames = 2;
    };

    // -------------------------------------------------------------------------

    void VulkanRayTracingBackend::RefreshCapabilities()
    {
        m_Capabilities = Capabilities{};
        auto* device = VulkanDevice::Get();
        if (device == nullptr)
        {
            m_Capabilities.Reason = UnsupportedReason::NoDevice;
            return;
        }
        m_Capabilities.Supported = device->IsRayQueryEnabled();
        m_Capabilities.RayTracingPipeline = device->IsRayTracingPipelineEnabled();
        m_Capabilities.Reason = device->GetRayTracingUnsupportedReason();
        m_Capabilities.Properties = device->GetRayTracingProperties();
        m_ScratchAlignment = std::max<VkDeviceSize>(m_Capabilities.Properties.MinScratchOffsetAlignment, 256);
    }

    VkCommandBuffer VulkanRayTracingBackend::AcquireCommandBuffer()
    {
        // Only a live frame recording will do. TryGetRecordingVulkanAPI is the
        // sanctioned probe — it answers non-null exactly when the process
        // backend is Vulkan AND a frame command buffer is open, which is the
        // pair of conditions a raw GetAPI() static_cast gets wrong.
        VulkanRendererAPI* api = VulkanUpload::TryGetRecordingVulkanAPI();
        if (api == nullptr)
        {
            return VK_NULL_HANDLE;
        }
        return api->BeginAccelerationStructureRecording();
    }

    bool VulkanRayTracingBackend::EnsureScratch(VkDeviceSize bytes)
    {
        if (bytes == 0)
        {
            return true;
        }
        if (m_Scratch.IsValid() && m_Scratch.Size >= bytes)
        {
            return true;
        }
        auto* device = VulkanDevice::Get();
        if (device == nullptr)
        {
            return false;
        }
        // Grow geometrically and keep the pool: a scratch buffer resized every
        // frame is a per-frame allocation with extra steps.
        const VkDeviceSize target = std::max<VkDeviceSize>(bytes, m_Scratch.Size * 2);
        RetireDeviceBuffer(m_Scratch);
        return CreateDeviceBuffer(*device, target,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_ScratchAlignment,
                                  "RayTracing/BuildScratch", m_Scratch);
    }

    void VulkanRayTracingBackend::EnsureQueryPool()
    {
        if (m_CompactionQueryPool != VK_NULL_HANDLE)
        {
            return;
        }
        auto* device = VulkanDevice::Get();
        if (device == nullptr)
        {
            return;
        }
        // VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR has no
        // representation in RHI::QueryType, and widening that neutral enum
        // would leak a Vulkan-only concept into the RHI with no OpenGL arm.
        // So this pool is owned here and handed to deferred reclaim at
        // teardown.
        VkQueryPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
        info.queryCount = kCompactionQuerySlots;
        if (vkCreateQueryPool(device->GetDevice(), &info, nullptr, &m_CompactionQueryPool) != VK_SUCCESS)
        {
            OLO_CORE_WARN("[RayTracing/Vulkan] compaction query pool creation failed — BLASes will not compact");
            m_CompactionQueryPool = VK_NULL_HANDLE;
            return;
        }
        m_QuerySlotOwner.assign(kCompactionQuerySlots, GeometryKey{});
        m_FreeQuerySlots.resize(kCompactionQuerySlots);
        for (u32 i = 0; i < kCompactionQuerySlots; ++i)
        {
            m_FreeQuerySlots[i] = kCompactionQuerySlots - 1u - i;
        }
    }

    u32 VulkanRayTracingBackend::AcquireQuerySlot()
    {
        if (m_FreeQuerySlots.empty())
        {
            return std::numeric_limits<u32>::max();
        }
        const u32 slot = m_FreeQuerySlots.back();
        m_FreeQuerySlots.pop_back();
        return slot;
    }

    u32 VulkanRayTracingBackend::RecordBlasBuilds(std::span<const BlasBuildRequest> requests)
    {
        if (!m_Capabilities.Supported)
        {
            return 0;
        }
        auto* device = VulkanDevice::Get();
        const VkCommandBuffer cmd = AcquireCommandBuffer();
        if (device == nullptr || cmd == VK_NULL_HANDLE)
        {
            return 0;
        }
        EnsureQueryPool();
        ++m_RecordGeneration;
        m_FrameBlasBuilds = 0;
        m_FrameBlasRefits = 0;

        // A frame with nothing to build is exactly when a pending
        // compacted-size query is most likely to have resolved, so the poll
        // runs before the early-out rather than after it. Returning here
        // without it is how compaction silently never happens in a scene that
        // has stopped changing — which is every scene, most of the time.
        if (requests.empty())
        {
            ResolveFinishedCompactions(cmd);
            return 0;
        }

        // Two passes: size everything first so one scratch buffer can serve
        // the whole batch, then record every build into one
        // vkCmdBuildAccelerationStructuresKHR call.
        struct Pending
        {
            const BlasBuildRequest* Request = nullptr;
            VkAccelerationStructureGeometryKHR Geometry{};
            VkAccelerationStructureBuildRangeInfoKHR Range{};
            VkAccelerationStructureBuildGeometryInfoKHR Build{};
            VkDeviceSize ScratchOffset = 0;
            bool IsUpdate = false;
        };
        std::vector<Pending> pending;
        pending.reserve(requests.size());

        VkDeviceSize scratchTotal = 0;
        for (const BlasBuildRequest& request : requests)
        {
            Pending item{};
            item.Request = &request;

            item.Geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
            item.Geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            // Always OPAQUE here, and opacity decided per INSTANCE instead.
            //
            // A BLAS is shared by every instance of a mesh, but "is this
            // alpha-tested" is a property of the instance's MATERIAL — the
            // same mesh can be an opaque wall in one entity and a cutout in
            // another. Baking the answer into the geometry would force two
            // BLASes for one mesh. VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR /
            // FORCE_NO_OPAQUE_BIT_KHR override this flag per instance, so one
            // structure serves both uses and the Masked class expresses itself
            // where it belongs: on the instance.
            item.Geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

            VkAccelerationStructureGeometryTrianglesDataKHR& triangles = item.Geometry.geometry.triangles;
            triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            triangles.vertexData.deviceAddress = request.VertexAddress;
            triangles.vertexStride = request.VertexStride;
            // maxVertex is the highest vertex index this build will ADDRESS,
            // and the address of index i is
            // `vertexData + vertexStride * (firstVertex + i)`. A submesh sits
            // at BaseVertex within a shared stream, so the highest addressed
            // index is BaseVertex + VertexCount - 1, not VertexCount - 1 —
            // getting this wrong under-declares the range for every submesh
            // that is not the first in its mesh.
            triangles.maxVertex =
                request.VertexCount > 0 ? static_cast<u32>(request.BaseVertex) + request.VertexCount - 1u : 0u;
            triangles.indexType = VK_INDEX_TYPE_UINT32;
            triangles.indexData.deviceAddress = request.IndexAddress;
            triangles.transformData.deviceAddress = 0;

            item.Range.primitiveCount = request.TriangleCount();
            item.Range.primitiveOffset = request.FirstIndex * static_cast<u32>(sizeof(u32));
            item.Range.firstVertex = static_cast<u32>(request.BaseVertex);
            item.Range.transformOffset = 0;

            const bool wantsUpdate = UpdatePolicyFor(request.Class) == UpdatePolicy::RefitOrRebuild;
            item.Build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
            item.Build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            // ALLOW_COMPACTION and ALLOW_UPDATE are mutually exclusive here by
            // construction, not by convention: a compacted AS cannot be
            // refitted, so a class that refits never asks to compact.
            item.Build.flags = wantsUpdate ? VkBuildAccelerationStructureFlagsKHR{
                VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR |
                VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR
            }
                                           : VkBuildAccelerationStructureFlagsKHR{ VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR };
            item.Build.geometryCount = 1;
            item.Build.pGeometries = &item.Geometry;

            auto existing = m_Blas.find(request.Key);
            item.IsUpdate = request.Reason == BuildReason::DeformedRefit && existing != m_Blas.end() &&
                            existing->second.Handle != VK_NULL_HANDLE && existing->second.AllowsUpdate;
            item.Build.mode = item.IsUpdate ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                                            : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;

            VkAccelerationStructureBuildSizesInfoKHR sizes{};
            sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
            const u32 primitiveCount = item.Range.primitiveCount;
            vkGetAccelerationStructureBuildSizesKHR(device->GetDevice(),
                                                    VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &item.Build,
                                                    &primitiveCount, &sizes);
            if (sizes.accelerationStructureSize == 0)
            {
                continue;
            }

            if (item.IsUpdate)
            {
                item.Build.srcAccelerationStructure = existing->second.Handle;
                item.Build.dstAccelerationStructure = existing->second.Handle;
            }
            else
            {
                // A fresh AS. The old one, if any, is retired only after this
                // succeeds, so a failed allocation leaves the previous
                // structure traceable rather than blanking the object.
                DeviceBuffer storage{};
                if (!CreateDeviceBuffer(*device, sizes.accelerationStructureSize,
                                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                                        kAccelerationStructureAlignment, "RayTracing/BLAS", storage))
                {
                    continue;
                }
                VkAccelerationStructureCreateInfoKHR createInfo{};
                createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
                createInfo.buffer = storage.Buffer;
                createInfo.offset = 0;
                createInfo.size = sizes.accelerationStructureSize;
                createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
                VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
                if (vkCreateAccelerationStructureKHR(device->GetDevice(), &createInfo, nullptr, &handle) != VK_SUCCESS)
                {
                    RetireDeviceBuffer(storage);
                    continue;
                }

                BlasEntry& entry = m_Blas[request.Key];
                DestroyBlasEntry(entry);
                entry.Handle = handle;
                entry.Storage = storage;
                entry.BuiltSize = sizes.accelerationStructureSize;
                entry.Class = request.Class;
                entry.AllowsUpdate = wantsUpdate;
                entry.Built = false;
                entry.CompactionState = BlasEntry::Compaction::NotRequested;

                VkAccelerationStructureDeviceAddressInfoKHR addressInfo{};
                addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
                addressInfo.accelerationStructure = handle;
                entry.Address = vkGetAccelerationStructureDeviceAddressKHR(device->GetDevice(), &addressInfo);

                item.Build.dstAccelerationStructure = handle;
            }

            item.ScratchOffset = scratchTotal;
            const VkDeviceSize scratchNeeded =
                item.IsUpdate ? sizes.updateScratchSize : sizes.buildScratchSize;
            scratchTotal = Align(scratchTotal + scratchNeeded, m_ScratchAlignment);
            pending.push_back(item);
        }

        if (pending.empty() || !EnsureScratch(scratchTotal))
        {
            ResolveFinishedCompactions(cmd);
            return 0;
        }

        // The scratch base is already aligned by the allocation; each offset
        // was rounded to the same alignment as it was accumulated.
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> buildInfos;
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> rangePointers;
        buildInfos.reserve(pending.size());
        rangePointers.reserve(pending.size());
        for (Pending& item : pending)
        {
            item.Build.scratchData.deviceAddress = m_Scratch.Address + item.ScratchOffset;
            // pGeometries must still point at storage that outlives the call;
            // `pending` owns it and is alive for the whole recording.
            item.Build.pGeometries = &item.Geometry;
            buildInfos.push_back(item.Build);
            rangePointers.push_back(&item.Range);
        }

        vkCmdBuildAccelerationStructuresKHR(cmd, static_cast<u32>(buildInfos.size()), buildInfos.data(),
                                            rangePointers.data());
        m_BuildsPendingBarrier = true;
        // Only now is a structure traceable. Anything that fell out of the
        // sizing loop above never reaches this line and stays non-resident.
        for (const Pending& item : pending)
        {
            if (auto found = m_Blas.find(item.Request->Key); found != m_Blas.end())
            {
                found->second.Built = true;
            }
            if (item.IsUpdate)
            {
                ++m_FrameBlasRefits;
            }
            else
            {
                ++m_FrameBlasBuilds;
            }
        }

        // Compaction size queries need the builds to have completed, so they
        // are separated by a build->build barrier and stamped in the same
        // command buffer. The RESULT is read a later frame, never waited on.
        if (m_CompactionQueryPool != VK_NULL_HANDLE)
        {
            std::vector<VkAccelerationStructureKHR> toQuery;
            std::vector<u32> slots;
            for (const Pending& item : pending)
            {
                if (item.IsUpdate || !AllowsCompaction(item.Request->Class))
                {
                    continue;
                }
                auto found = m_Blas.find(item.Request->Key);
                if (found == m_Blas.end() || found->second.CompactionState != BlasEntry::Compaction::NotRequested)
                {
                    continue;
                }
                const u32 slot = AcquireQuerySlot();
                if (slot == std::numeric_limits<u32>::max())
                {
                    break; // Pool exhausted this frame; the BLAS simply stays uncompacted.
                }
                found->second.QuerySlot = slot;
                found->second.CompactionState = BlasEntry::Compaction::SizeQueryPending;
                found->second.QueryRecordedAt = m_RecordGeneration;
                m_QuerySlotOwner[slot] = item.Request->Key;
                toQuery.push_back(found->second.Handle);
                slots.push_back(slot);
            }
            if (!toQuery.empty())
            {
                VkMemoryBarrier2 barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
                barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
                barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
                VkDependencyInfo dep{};
                dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                dep.memoryBarrierCount = 1;
                dep.pMemoryBarriers = &barrier;
                vkCmdPipelineBarrier2(cmd, &dep);

                // Reset immediately before the write: reading a slot that was
                // never reset is undefined, and vkCmdResetQueryPool is illegal
                // inside a render-pass instance (we are outside one here).
                for (const u32 slot : slots)
                {
                    vkCmdResetQueryPool(cmd, m_CompactionQueryPool, slot, 1);
                }
                for (sizet i = 0; i < toQuery.size(); ++i)
                {
                    vkCmdWriteAccelerationStructuresPropertiesKHR(
                        cmd, 1, &toQuery[i], VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                        m_CompactionQueryPool, slots[i]);
                }
            }
        }

        ResolveFinishedCompactions(cmd);
        return static_cast<u32>(pending.size());
    }

    void VulkanRayTracingBackend::ResolveFinishedCompactions(VkCommandBuffer cmd)
    {
        if (m_CompactionQueryPool == VK_NULL_HANDLE)
        {
            return;
        }
        auto* device = VulkanDevice::Get();
        if (device == nullptr)
        {
            return;
        }
        m_FrameCompactions = 0;
        for (auto& [key, entry] : m_Blas)
        {
            if (entry.CompactionState != BlasEntry::Compaction::SizeQueryPending ||
                entry.QuerySlot == std::numeric_limits<u32>::max())
            {
                continue;
            }
            // Not yet legal to read: the reset for this slot is still in a
            // command buffer that may not have executed. Reading it anyway is
            // VUID-vkGetQueryPoolResults-None-09401 — which is a validation
            // error rather than a hang only because we never pass the WAIT
            // bit.
            if (m_RecordGeneration < entry.QueryRecordedAt + kQueryReadDelayFrames)
            {
                continue;
            }
            // WITH_AVAILABILITY and no WAIT bit: this is a poll. A blocking
            // read here would be the device idle this subsystem is forbidden
            // from taking, and on a slot whose write has not executed it would
            // never return at all.
            struct
            {
                u64 Size;
                u64 Available;
            } result{ 0, 0 };
            const VkResult status = vkGetQueryPoolResults(device->GetDevice(), m_CompactionQueryPool, entry.QuerySlot, 1,
                                                          sizeof(result), &result, sizeof(result),
                                                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            if (status != VK_SUCCESS || result.Available == 0 || result.Size == 0)
            {
                continue;
            }

            // A compaction that would not shrink the structure is not worth a
            // copy and a second allocation.
            if (result.Size >= entry.BuiltSize)
            {
                entry.CompactionState = BlasEntry::Compaction::Compacted;
                m_FreeQuerySlots.push_back(entry.QuerySlot);
                m_QuerySlotOwner[entry.QuerySlot] = GeometryKey{};
                entry.QuerySlot = std::numeric_limits<u32>::max();
                continue;
            }

            DeviceBuffer compactedStorage{};
            if (!CreateDeviceBuffer(*device, result.Size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                                    kAccelerationStructureAlignment, "RayTracing/BLAS(compacted)", compactedStorage))
            {
                continue;
            }
            VkAccelerationStructureCreateInfoKHR createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
            createInfo.buffer = compactedStorage.Buffer;
            createInfo.offset = 0;
            createInfo.size = result.Size;
            createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            VkAccelerationStructureKHR compacted = VK_NULL_HANDLE;
            if (vkCreateAccelerationStructureKHR(device->GetDevice(), &createInfo, nullptr, &compacted) != VK_SUCCESS)
            {
                RetireDeviceBuffer(compactedStorage);
                continue;
            }

            VkCopyAccelerationStructureInfoKHR copyInfo{};
            copyInfo.sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR;
            copyInfo.src = entry.Handle;
            copyInfo.dst = compacted;
            copyInfo.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
            vkCmdCopyAccelerationStructureKHR(cmd, &copyInfo);

            // Retire the source AFTER the copy is recorded, and the AS handle
            // BEFORE its backing buffer — deferred reclaim destroys in
            // insertion order within a generation, and freeing the memory
            // under a live structure is a use-after-free inside the driver.
            VulkanDeferredReclaim::Get().Enqueue(entry.Handle);
            RetireDeviceBuffer(entry.Storage);

            m_CompactionSavedBytes += entry.BuiltSize - result.Size;
            entry.Handle = compacted;
            entry.Storage = compactedStorage;
            entry.CompactedSize = result.Size;
            entry.CompactionState = BlasEntry::Compaction::Compacted;

            VkAccelerationStructureDeviceAddressInfoKHR addressInfo{};
            addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
            addressInfo.accelerationStructure = compacted;
            entry.Address = vkGetAccelerationStructureDeviceAddressKHR(device->GetDevice(), &addressInfo);

            m_FreeQuerySlots.push_back(entry.QuerySlot);
            m_QuerySlotOwner[entry.QuerySlot] = GeometryKey{};
            entry.QuerySlot = std::numeric_limits<u32>::max();
            ++m_FrameCompactions;
            m_BuildsPendingBarrier = true;
        }
    }

    void VulkanRayTracingBackend::DestroyBlasEntry(BlasEntry& entry)
    {
        if (entry.Handle != VK_NULL_HANDLE)
        {
            VulkanDeferredReclaim::Get().Enqueue(entry.Handle);
            entry.Handle = VK_NULL_HANDLE;
        }
        RetireDeviceBuffer(entry.Storage);
        if (entry.QuerySlot != std::numeric_limits<u32>::max())
        {
            m_FreeQuerySlots.push_back(entry.QuerySlot);
            if (entry.QuerySlot < m_QuerySlotOwner.size())
            {
                m_QuerySlotOwner[entry.QuerySlot] = GeometryKey{};
            }
            entry.QuerySlot = std::numeric_limits<u32>::max();
            entry.QueryRecordedAt = 0;
        }
        entry.Address = 0;
        entry.BuiltSize = 0;
        entry.CompactedSize = 0;
        entry.CompactionState = BlasEntry::Compaction::NotRequested;
    }

    void VulkanRayTracingBackend::RetireBlas(const GeometryKey& key)
    {
        auto found = m_Blas.find(key);
        if (found == m_Blas.end())
        {
            return;
        }
        DestroyBlasEntry(found->second);
        m_Blas.erase(found);
    }

    bool VulkanRayTracingBackend::IsBlasResident(const GeometryKey& key) const
    {
        const auto found = m_Blas.find(key);
        return found != m_Blas.end() && found->second.Handle != VK_NULL_HANDLE && found->second.Address != 0u &&
               found->second.Built;
    }

    TlasBuildReason VulkanRayTracingBackend::RecordTlasBuild(std::span<const InstanceRecord> instances,
                                                             TlasBuildReason requested)
    {
        if (!m_Capabilities.Supported)
        {
            return requested;
        }
        auto* device = VulkanDevice::Get();
        const VkCommandBuffer cmd = AcquireCommandBuffer();
        if (device == nullptr || cmd == VK_NULL_HANDLE)
        {
            return requested;
        }

        const u32 instanceCount = static_cast<u32>(instances.size());
        // A refit cannot grow past what the current TLAS was sized for, and a
        // TLAS that has never been built cannot be updated. Promote rather
        // than fail — the caller is told which mode was actually used.
        TlasBuildReason used = requested;
        if (m_Tlas == VK_NULL_HANDLE || instanceCount > m_TlasCapacity)
        {
            used = m_Tlas == VK_NULL_HANDLE ? TlasBuildReason::FirstBuild : TlasBuildReason::InstanceCountGrew;
        }
        // A compaction this frame MOVED a BLAS: the compacted copy has a
        // different device address, and the structure the old address names is
        // already on the reclaim queue. Every instance referencing it has to
        // be re-emitted against the new address before the old one is
        // destroyed, so a compaction forces a rebuild rather than a refit.
        // The caller cannot know this happened — compaction is entirely the
        // backend's business — which is why the promotion lives here.
        else if (m_FrameCompactions > 0 && used == TlasBuildReason::Update)
        {
            used = TlasBuildReason::TopologyChanged;
        }

        // The instance array is staged into a buffer written this frame and
        // read by this frame's build. It is reallocated whenever the count
        // grows rather than being one persistent buffer rewritten in place:
        // "same buffer written more than once per frame with work recorded
        // between the writes" is exactly the last-write-wins archetype
        // vulkan-command-ordered-buffer-writes.md is about, and an AS build
        // consumes its instance data on the GPU at build time.
        const VkDeviceSize instanceBytes =
            std::max<VkDeviceSize>(instanceCount * sizeof(VkAccelerationStructureInstanceKHR), 1);
        // See the member's comment: this frame writes a DIFFERENT buffer from
        // the one the previous frame's build may still be reading.
        DeviceBuffer& instanceBuffer = m_TlasInstances[m_RecordGeneration % kFramesInFlight];
        if (!instanceBuffer.IsValid() || instanceBuffer.Size < instanceBytes)
        {
            RetireDeviceBuffer(instanceBuffer);
            if (!CreateDeviceBuffer(*device, instanceBytes,
                                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    kInstanceDataAlignment, "RayTracing/TLASInstances", instanceBuffer))
            {
                return used;
            }
        }

        std::vector<VkAccelerationStructureInstanceKHR> packed;
        packed.reserve(instanceCount);
        for (const InstanceRecord& record : instances)
        {
            const auto found = m_Blas.find(record.Geometry);
            if (found == m_Blas.end() || found->second.Address == 0u)
            {
                continue;
            }
            VkAccelerationStructureInstanceKHR out{};
            // GPUSceneTransform IS VkTransformMatrixKHR's layout — three
            // row-major vec4s. Copying the twelve floats straight across is
            // deliberate; going via glm::mat4 is how a transpose gets in.
            std::memcpy(&out.transform.matrix[0][0], &record.Transform[0].x, sizeof(f32) * 12);
            out.instanceCustomIndex = record.CustomIndex & kMaxInstanceCustomIndex;
            out.mask = record.Mask;
            out.instanceShaderBindingTableRecordOffset = 0;
            out.flags = record.ForceOpaque ? VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR
                                           : VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
            out.accelerationStructureReference = found->second.Address;
            packed.push_back(out);
        }

        if (!packed.empty())
        {
            // Stage through the frame arena and copy in COMMAND ORDER, so the
            // build reads the bytes this frame recorded rather than whatever a
            // mapped write left behind.
            const auto staged = VulkanFrameArena::Get().Push(
                packed.data(), packed.size() * sizeof(VkAccelerationStructureInstanceKHR), kInstanceDataAlignment);
            if (!staged.IsValid())
            {
                OLO_CORE_WARN("[RayTracing/Vulkan] frame arena overflow staging {} TLAS instances", packed.size());
                return used;
            }
            VkBufferCopy copy{};
            copy.srcOffset = staged.Offset;
            copy.dstOffset = 0;
            copy.size = packed.size() * sizeof(VkAccelerationStructureInstanceKHR);
            vkCmdCopyBuffer(cmd, VulkanFrameArena::Get().GetSlotBuffer(VulkanFrameArena::Get().GetCurrentSlot()),
                            instanceBuffer.Buffer, 1, &copy);

            VkBufferMemoryBarrier2 uploadBarrier{};
            uploadBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            uploadBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            uploadBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            uploadBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
            // SHADER_READ, not ACCELERATION_STRUCTURE_READ. The two are not
            // interchangeable and the difference is exactly here:
            // ACCELERATION_STRUCTURE_READ covers reading an acceleration
            // STRUCTURE, while a build's read of its INPUT data — instances,
            // vertices, indices, transforms — is a shader read. Naming the
            // wrong one leaves a real read-after-write hazard on the instance
            // buffer that sync validation catches and nothing else would:
            // "vkCmdBuildAccelerationStructuresKHR reads instance data ...
            // which was previously written by vkCmdCopyBuffer".
            uploadBarrier.dstAccessMask =
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
            uploadBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            uploadBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            uploadBarrier.buffer = instanceBuffer.Buffer;
            uploadBarrier.offset = 0;
            uploadBarrier.size = VK_WHOLE_SIZE;
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.bufferMemoryBarrierCount = 1;
            dep.pBufferMemoryBarriers = &uploadBarrier;
            vkCmdPipelineBarrier2(cmd, &dep);
        }

        const u32 packedCount = static_cast<u32>(packed.size());

        VkAccelerationStructureGeometryKHR geometry{};
        geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        geometry.flags = 0;
        geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        geometry.geometry.instances.arrayOfPointers = VK_FALSE;
        geometry.geometry.instances.data.deviceAddress = instanceBuffer.Address;

        VkAccelerationStructureBuildGeometryInfoKHR build{};
        build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        build.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR |
                      VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        build.geometryCount = 1;
        build.pGeometries = &geometry;
        build.mode = used == TlasBuildReason::Update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                                                     : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;

        VkAccelerationStructureBuildSizesInfoKHR sizes{};
        sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        vkGetAccelerationStructureBuildSizesKHR(device->GetDevice(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                &build, &packedCount, &sizes);

        // An update whose primitiveCount differs from the source's build count
        // is invalid usage, so ANY change in instance count forces a rebuild —
        // shrinking as much as growing. The scene's own shrink heuristic still
        // decides when a rebuild is WORTH it; this is the correctness floor
        // under it.
        if (used == TlasBuildReason::Update && packedCount != m_TlasBuiltPrimitiveCount)
        {
            used = packedCount > m_TlasBuiltPrimitiveCount ? TlasBuildReason::InstanceCountGrew
                                                           : TlasBuildReason::InstanceCountShrank;
            build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        }

        if (used != TlasBuildReason::Update || m_Tlas == VK_NULL_HANDLE ||
            sizes.accelerationStructureSize > m_TlasStorage.Size)
        {
            DeviceBuffer storage{};
            if (!CreateDeviceBuffer(*device, sizes.accelerationStructureSize,
                                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                                    kAccelerationStructureAlignment, "RayTracing/TLAS", storage))
            {
                return used;
            }
            VkAccelerationStructureCreateInfoKHR createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
            createInfo.buffer = storage.Buffer;
            createInfo.offset = 0;
            createInfo.size = sizes.accelerationStructureSize;
            createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
            VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
            if (vkCreateAccelerationStructureKHR(device->GetDevice(), &createInfo, nullptr, &handle) != VK_SUCCESS)
            {
                RetireDeviceBuffer(storage);
                return used;
            }
            if (m_Tlas != VK_NULL_HANDLE)
            {
                VulkanDeferredReclaim::Get().Enqueue(m_Tlas);
                RetireDeviceBuffer(m_TlasStorage);
            }
            m_Tlas = handle;
            m_TlasStorage = storage;
            m_TlasCapacity = packedCount;
            if (used == TlasBuildReason::Update)
            {
                used = TlasBuildReason::InstanceCountGrew;
            }
            build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;

            VkAccelerationStructureDeviceAddressInfoKHR addressInfo{};
            addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
            addressInfo.accelerationStructure = handle;
            m_TlasAddress = vkGetAccelerationStructureDeviceAddressKHR(device->GetDevice(), &addressInfo);
        }

        if (build.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR)
        {
            build.srcAccelerationStructure = m_Tlas;
        }
        build.dstAccelerationStructure = m_Tlas;

        const VkDeviceSize scratchNeeded = build.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                                               ? sizes.updateScratchSize
                                               : sizes.buildScratchSize;
        if (!EnsureScratch(scratchNeeded))
        {
            return used;
        }
        build.scratchData.deviceAddress = m_Scratch.Address;

        // The BLAS builds recorded earlier this frame must complete before the
        // TLAS build reads their structures.
        VkMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        barrier.dstAccessMask =
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = packedCount;
        const VkAccelerationStructureBuildRangeInfoKHR* rangePtr = &range;
        vkCmdBuildAccelerationStructuresKHR(cmd, 1, &build, &rangePtr);
        m_BuildsPendingBarrier = true;
        // Only a full build redefines what a later update must match.
        if (build.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR)
        {
            m_TlasBuiltPrimitiveCount = packedCount;
        }
        return used;
    }

    u64 VulkanRayTracingBackend::GetTlasDeviceAddress() const
    {
        return m_Tlas != VK_NULL_HANDLE ? static_cast<u64>(m_TlasAddress) : 0u;
    }

    void VulkanRayTracingBackend::RecordBuildToReadBarrier()
    {
        if (!m_Capabilities.Supported || !m_BuildsPendingBarrier)
        {
            return;
        }
        const VkCommandBuffer cmd = AcquireCommandBuffer();
        if (cmd == VK_NULL_HANDLE)
        {
            return;
        }
        // The Vulkan spec models acceleration-structure synchronisation with
        // ordinary memory barriers — there is no per-AS barrier object — so
        // this is a global VkMemoryBarrier2 rather than a buffer barrier over
        // the storage. The destination scope names every shader stage a ray
        // query can run in.
        VkMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                               VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
        m_BuildsPendingBarrier = false;
    }

    void VulkanRayTracingBackend::PublishStats(SceneStats& stats) const
    {
        u64 asBytes = m_TlasStorage.Size;
        for (const DeviceBuffer& instances : m_TlasInstances)
        {
            asBytes += instances.Size;
        }
        for (const auto& [key, entry] : m_Blas)
        {
            asBytes += entry.Storage.Size;
        }
        stats.Resident.AccelerationStructureBytes = asBytes;
        stats.Resident.ScratchBytes = m_Scratch.Size;
        stats.Resident.CompactionSavedBytes = m_CompactionSavedBytes;
        stats.Frame.BlasCompactions = m_FrameCompactions;
        stats.Frame.BlasBuilds = m_FrameBlasBuilds;
        stats.Frame.BlasRefits = m_FrameBlasRefits;
    }

    void VulkanRayTracingBackend::Shutdown()
    {
        for (auto& [key, entry] : m_Blas)
        {
            DestroyBlasEntry(entry);
        }
        m_Blas.clear();
        if (m_Tlas != VK_NULL_HANDLE)
        {
            VulkanDeferredReclaim::Get().Enqueue(m_Tlas);
            m_Tlas = VK_NULL_HANDLE;
        }
        RetireDeviceBuffer(m_TlasStorage);
        for (DeviceBuffer& instances : m_TlasInstances)
        {
            RetireDeviceBuffer(instances);
        }
        RetireDeviceBuffer(m_Scratch);
        m_TlasAddress = 0;
        m_TlasCapacity = 0;
        m_TlasBuiltPrimitiveCount = 0;
        if (m_CompactionQueryPool != VK_NULL_HANDLE)
        {
            VulkanDeferredReclaim::Get().Enqueue(m_CompactionQueryPool);
            m_CompactionQueryPool = VK_NULL_HANDLE;
        }
        m_QuerySlotOwner.clear();
        m_FreeQuerySlots.clear();
        m_BuildsPendingBarrier = false;
    }

    std::unique_ptr<IRayTracingBackend> CreateVulkanRayTracingBackend()
    {
        return std::make_unique<VulkanRayTracingBackend>();
    }
} // namespace OloEngine::RayTracing

#endif // OLO_WITH_VULKAN
