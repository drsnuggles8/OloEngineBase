#include "OloEnginePCH.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshRegistry.h"

#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualLightmapUVPacking.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshBuilder.h"

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <cmath>
#include <ranges>
#include <cstring>
#include <vector>

namespace OloEngine
{
    namespace
    {
        constexpr u32 kStateResident = 1u << 0;
        constexpr u32 kStateRequested = 1u << 1;
        constexpr u32 kStateTouched = 1u << 2;
        constexpr u64 kUploadRingBytes = 8ull * 1024 * 1024;
    } // namespace

    VirtualMeshRegistry& VirtualMeshRegistry::Get()
    {
        static VirtualMeshRegistry s_Instance;
        return s_Instance;
    }

    bool VirtualMeshRegistry::RegisterMeshSource(AssetHandle handle, const MeshSource& source)
    {
        if (auto it = m_EntryLookup.find(handle); it != m_EntryLookup.end())
        {
            return it->second.Valid;
        }

        // Fast path: a cooked blob (imported through the .omesh cache's VirtualMesh
        // section, or the asset pack's v4 trailing blob) skips the whole
        // clusterize/simplify build. DeserializeSetFromBlob treats the blob as hostile
        // input and also accepts a legacy single-DAG "OVGM" cook; a stale or corrupt one
        // falls back to the runtime build rather than failing the component.
        VirtualMeshSet built;
        bool haveBuilt = false;
        if (source.HasVirtualMeshBlob())
        {
            haveBuilt = VirtualMeshSerializer::DeserializeSetFromBlob(source.GetVirtualMeshBlob(), built);
            if (!haveBuilt)
            {
                OLO_CORE_WARN("VirtualMeshRegistry: cooked virtual-geometry blob failed validation for mesh asset {} — "
                              "rebuilding the cluster DAG from geometry",
                              static_cast<u64>(handle));
            }
        }
        if (!haveBuilt)
        {
            built = VirtualMeshBuilder::BuildSet(source);
        }

        // One MeshEntry per part, pushed contiguously so MeshParts is just a range. The pool
        // packing walks m_Entries and stays entirely part-agnostic.
        MeshParts parts;
        parts.FirstEntry = static_cast<u32>(m_Entries.size());
        for (const VirtualMeshPart& part : built.Parts)
        {
            MeshEntry entry;
            entry.SubmeshIndex = part.SubmeshIndex;
            if (part.Dag.IsValid())
            {
                entry.Packed = PackVirtualMeshForGpu(part.Dag);
                entry.LevelCount = part.Dag.LevelCount;
                entry.SourceTriangleCount = part.Dag.SourceTriangleCount;
                entry.Valid = entry.Packed.IsValid();
                // Mesh-shader path eligibility (#813): one mesh workgroup
                // renders one cluster, so EVERY cluster must fit the declared
                // output limits. Decided per part at registration — the draw
                // loop routes per instance, never per cluster. (Invalid/empty
                // packed data is incompatible by IsMeshletCompatible's own
                // IsValid() guard — no external pre-check needed.)
                entry.MeshletCompatible = IsMeshletCompatible(entry.Packed);
            }
            parts.Valid = parts.Valid || entry.Valid;
            m_PoolsDirty = m_PoolsDirty || entry.Valid;
            m_Entries.push_back(std::move(entry));
        }
        parts.Count = static_cast<u32>(m_Entries.size()) - parts.FirstEntry;

        if (!parts.Valid)
        {
            OLO_CORE_WARN("VirtualMeshRegistry: Building the cluster DAG failed for mesh asset {} — "
                          "the VirtualMeshComponent will not render",
                          static_cast<u64>(handle));
        }
        else
        {
            OLO_CORE_TRACE("VirtualMeshRegistry: mesh asset {} registered as {} part(s)",
                           static_cast<u64>(handle), parts.Count);
        }

        m_EntryLookup.emplace(handle, parts);
        return parts.Valid;
    }

    bool VirtualMeshRegistry::IsRegistered(AssetHandle handle) const
    {
        return m_EntryLookup.contains(handle);
    }

    void VirtualMeshRegistry::Invalidate(AssetHandle handle)
    {
        auto it = m_EntryLookup.find(handle);
        if (it == m_EntryLookup.end())
        {
            return;
        }

        // Drop the lookup and mark the mesh's parts dead. The MeshEntry slots themselves are
        // left in place (never erased): entry indices are baked into MeshParts::FirstEntry for
        // every OTHER registered mesh, and the pool packing walks m_Entries positionally — so
        // erasing would silently rebase every mesh after this one. RebuildPools skips !Valid
        // entries, so a dead run costs a few hundred bytes of CPU-side geometry until the next
        // Shutdown; the GPU arenas are reclaimed on the next rebuild.
        //
        // Without this, RegisterMeshSource's cache-by-AssetHandle was permanent: hot-reloading
        // a MeshSource left the OLD cluster DAG live for the rest of the process (the caller
        // takes the IsRegistered() fast path and never rebuilds), so the virtual path kept
        // drawing the pre-edit geometry — self-consistently, since the DAG bakes its own copy
        // of the vertices, and therefore with nothing to trip a validation check.
        for (u32 i = 0; i < it->second.Count; ++i)
        {
            MeshEntry& entry = m_Entries[it->second.FirstEntry + i];
            entry.Valid = false;
            entry.Packed = {};
        }
        m_EntryLookup.erase(it);
        m_BlendRejectionWarned.erase(static_cast<u64>(handle));
        m_PoolsDirty = true;

        OLO_CORE_TRACE("VirtualMeshRegistry: invalidated cluster DAG for mesh asset {} (source reloaded)",
                       static_cast<u64>(handle));
    }

    bool VirtualMeshRegistry::MeshHasLightmapUVs(AssetHandle handle) const
    {
        // Every PART of the mesh must carry the stream, not merely one: a draw
        // samples per part, and a part whose cook predates the unwrap would read
        // the tail region a sibling part wrote. All-or-nothing, like the blob's
        // own LightmapUVCount validation (issue #867).
        const MeshParts parts = FindParts(handle);
        if (!parts.Valid || parts.Count == 0)
        {
            return false;
        }
        for (u32 i = 0; i < parts.Count; ++i)
        {
            const auto& packed = GetEntry(parts.FirstEntry + i).Packed;
            if (packed.LightmapUVs.empty() || packed.LightmapUVs.size() != packed.Vertices.size())
            {
                return false;
            }
        }
        return true;
    }

    VirtualMeshRegistry::MeshParts VirtualMeshRegistry::FindParts(AssetHandle handle) const
    {
        if (auto it = m_EntryLookup.find(handle); it != m_EntryLookup.end())
        {
            return it->second;
        }
        return {};
    }

    void VirtualMeshRegistry::BeginFrame()
    {
        m_Submissions.clear();
        m_FrameInstances.clear();
        m_TotalFrameClusterCount = 0;
        m_FramePrepared = false;
        m_FramePreparedResult = false;
        m_ResidencyProcessed = false;
        m_SubmissionDiagnostics = {};
    }

    void VirtualMeshRegistry::Submit(const VirtualMeshSubmission& submission)
    {
        m_Submissions.push_back(submission);
    }

    void VirtualMeshRegistry::SetPageBudgetSlots(u32 budgetSlots)
    {
        if (m_BudgetSlotsSetting != budgetSlots)
        {
            m_BudgetSlotsSetting = budgetSlots;
            m_PoolsDirty = true; // arenas + residency rebuild on next frame
        }
    }

    void VirtualMeshRegistry::CopyThroughRing(RHI::ResourceHandle targetBuffer, u64 targetOffset,
                                              const void* payload, u64 bytes)
    {
        if (bytes == 0)
        {
            return;
        }

        // Reserve waits out only the fences still covering the reused range
        // (GPUCircularBuffer, #704) — the old hand-rolled ring stalled the
        // whole device on wrap. nullptr means the payload exceeds the ring (or
        // the ring never mapped): direct upload.
        u64 ringOffset = 0;
        u8* dst = m_UploadRing.IsCreated() ? m_UploadRing.Reserve(bytes, ringOffset) : nullptr;
        if (dst == nullptr)
        {
            RenderCommand::UploadBufferSubData(targetBuffer, targetOffset, bytes, payload);
            return;
        }

        std::memcpy(dst, payload, bytes);
        RenderCommand::CopyBufferSubData(m_UploadRing.GetDeviceHandle(), targetBuffer, ringOffset, targetOffset,
                                         bytes);
        m_UploadRing.Commit(bytes);
    }

    bool VirtualMeshRegistry::LoadPage(u32 pageIndex)
    {
        PageRuntime& page = m_Pages[pageIndex];
        if (page.Resident)
        {
            return true;
        }

        // Allocate a slot through the shared paged-cache substrate (#704):
        // free slot first, else the policy delegates back to
        // SelectResidencyVictim (the exact pre-#704 LRU scan) and the victim's
        // slot transfers to this page — OnResidencyEvicted does the victim's
        // bookkeeping via the eviction listener. Failure means the budget is
        // exhausted by pinned/in-use pages; the coarser cut keeps rendering.
        SlotCache::ObjectAllocation slotAlloc;
        if (!m_SlotCache.AllocatePages(static_cast<u64>(pageIndex), 1, slotAlloc))
        {
            return false;
        }
        u32 const slot = slotAlloc.m_StartPage;

        const MeshEntry& entry = m_Entries[page.MeshEntryIndex];
        const VirtualMeshGpuData& packed = entry.Packed;

        // Geometry payloads into the arena slot
        u64 const slotVertexBase = static_cast<u64>(slot) * m_SlotVertexCapacity;
        u64 const slotIndexBase = static_cast<u64>(slot) * m_SlotIndexCapacity;
        CopyThroughRing(m_VertexBuffer->GetRHIHandle(), slotVertexBase * sizeof(VirtualGpuVertex),
                        packed.Vertices.data() + page.Info.VertexOffset,
                        static_cast<u64>(page.Info.VertexCount) * sizeof(VirtualGpuVertex));
        CopyThroughRing(m_IndexBuffer, slotIndexBase * sizeof(u32),
                        packed.Indices.data() + page.Info.IndexOffset,
                        static_cast<u64>(page.Info.IndexCount) * sizeof(u32));

        // The page's baked lightmap uv2, packed four pairs to a 32-byte element
        // (issue #867). A page's vertices always start at slot-local index 0,
        // and m_SlotVertexCapacity is 4-aligned, so the destination element is
        // exactly `uvBase + slotVertexBase / 4` and the lane of slot-local
        // vertex i is `i & 3` — no per-page offset math, which is the whole
        // reason the capacity is rounded.
        if (m_LightmapUVBaseElement != 0 && packed.LightmapUVs.size() == packed.Vertices.size() &&
            !packed.LightmapUVs.empty())
        {
            // Packed slot-locally from 0, which is legal only because a page's
            // vertices start at slot-local 0 AND m_SlotVertexCapacity is
            // 4-aligned — see VirtualLightmapUVPacking.h. The lane of a
            // slot-local index therefore equals the lane of its global index,
            // which is what the shader relies on.
            u32 const packedElements = VirtualLightmapUVElementCount(page.Info.VertexCount);
            std::vector<VirtualGpuVertex> uvStaging(packedElements);
            for (u32 v = 0; v < page.Info.VertexCount; ++v)
            {
                PackVirtualLightmapUV(uvStaging[VirtualLightmapUVElementOffset(v)], v,
                                      packed.LightmapUVs[page.Info.VertexOffset + v]);
            }
            CopyThroughRing(m_VertexBuffer->GetRHIHandle(),
                            (static_cast<u64>(m_LightmapUVBaseElement) +
                             VirtualLightmapUVElementOffset(static_cast<u32>(slotVertexBase))) *
                                sizeof(VirtualGpuVertex),
                            uvStaging.data(),
                            static_cast<u64>(packedElements) * sizeof(VirtualGpuVertex));
        }

        // Rebase the page's cluster records onto the live slot and publish them
        // (contiguous range in the pooled cluster buffer).
        std::vector<VirtualClusterGpuRecord> rebased(page.Info.ClusterCount);
        for (u32 c = 0; c < page.Info.ClusterCount; ++c)
        {
            VirtualClusterGpuRecord record = m_PooledClusters[page.PooledFirstCluster + c];
            record.VertexBase = static_cast<u32>(slotVertexBase) + (record.VertexBase - page.Info.VertexOffset);
            record.IndexBase = static_cast<u32>(slotIndexBase) + (record.IndexBase - page.Info.IndexOffset);
            rebased[c] = record;
        }
        m_ClusterBuffer->SetData(rebased.data(),
                                 static_cast<u32>(rebased.size() * sizeof(VirtualClusterGpuRecord)),
                                 page.PooledFirstCluster * static_cast<u32>(sizeof(VirtualClusterGpuRecord)));

        page.SlotIndex = slot;
        page.Resident = true;
        page.LastUsedFrame = m_FrameCounter;
        m_GroupStatesCpu[page.PooledGroup] |= kStateResident;
        m_DirtyResidencyGroups.push_back(page.PooledGroup);
        ++m_ResidencyStats.PageUploads;
        ++m_ResidencyStats.ResidentPages;
        return true;
    }

    void VirtualMeshRegistry::OnResidencyEvicted(u32 pageIndex)
    {
        // The slot itself has already been reclaimed by the cache (transferred
        // to the requesting page); this is only the registry-side bookkeeping.
        PageRuntime& page = m_Pages[pageIndex];
        if (!page.Resident)
        {
            return;
        }
        page.SlotIndex = kNoSlot;
        page.Resident = false;
        m_GroupStatesCpu[page.PooledGroup] &= ~kStateResident;
        m_DirtyResidencyGroups.push_back(page.PooledGroup);
        ++m_ResidencyStats.PageEvictions;
        --m_ResidencyStats.ResidentPages;
    }

    bool VirtualMeshRegistry::SelectResidencyVictim(u64 excludePage, u64& outPage) const
    {
        u32 victim = kNoSlot;
        u64 oldestUse = ~0ull;
        auto const pageCount = static_cast<u32>(m_Pages.size());
        for (u32 p = 0; p < pageCount; ++p)
        {
            const PageRuntime& candidate = m_Pages[p];
            if (p == excludePage || !candidate.Resident || candidate.Pinned ||
                candidate.LastUsedFrame >= m_FrameCounter)
            {
                continue;
            }
            if (candidate.LastUsedFrame < oldestUse)
            {
                oldestUse = candidate.LastUsedFrame;
                victim = p;
            }
        }
        if (victim == kNoSlot)
        {
            return false;
        }
        outPage = victim;
        return true;
    }

    void VirtualMeshRegistry::RebuildPools()
    {
        OLO_PROFILE_FUNCTION();

        std::vector<VirtualGroupGpuRecord> groups;
        m_PooledClusters.clear();
        m_Pages.clear();
        m_PageOfPooledGroup.clear();
        m_SlotCache.Destroy();
        m_ResidencyStats = {};

        u32 maxPageVertices = 0;
        u32 maxPageIndices = 0;
        u32 pinnedPages = 0;

        for (u32 entryIndex = 0; entryIndex < m_Entries.size(); ++entryIndex)
        {
            MeshEntry& entry = m_Entries[entryIndex];
            if (!entry.Valid)
            {
                continue;
            }

            entry.ClusterBase = static_cast<u32>(m_PooledClusters.size());
            entry.GroupBase = static_cast<u32>(groups.size());

            for (const VirtualClusterGpuRecord& record : entry.Packed.Clusters)
            {
                VirtualClusterGpuRecord pooled = record; // geometry bases stay MESH-LOCAL until the page loads
                pooled.GroupIndex += entry.GroupBase;
                if (pooled.RefinedGroup != VirtualClusterGpuRecord::kNoRefinedGroup)
                {
                    pooled.RefinedGroup += entry.GroupBase;
                }
                m_PooledClusters.push_back(pooled);
            }
            groups.insert(groups.end(), entry.Packed.Groups.begin(), entry.Packed.Groups.end());

            for (const VirtualPageInfo& info : entry.Packed.Pages)
            {
                PageRuntime page;
                page.Info = info;
                page.MeshEntryIndex = entryIndex;
                page.PooledGroup = entry.GroupBase + info.GroupIndex;
                page.PooledFirstCluster = entry.ClusterBase + info.FirstCluster;
                page.Pinned = info.Pinned;
                maxPageVertices = std::max(maxPageVertices, info.VertexCount);
                maxPageIndices = std::max(maxPageIndices, info.IndexCount);
                pinnedPages += info.Pinned ? 1u : 0u;
                m_Pages.push_back(page);
            }
        }

        if (m_PooledClusters.empty())
        {
            return;
        }
        m_PageOfPooledGroup.assign(groups.size(), kNoSlot);
        for (u32 p = 0; p < m_Pages.size(); ++p)
        {
            m_PageOfPooledGroup[m_Pages[p].PooledGroup] = p;
        }

        // Slot arenas: uniform slot capacity = largest page; slot count from the
        // budget (0 = eager: every page resident, no streaming pressure). A
        // budget below pinned + headroom cannot make progress — clamp with a warn.
        // Rounded UP to a multiple of 4 (issue #867). The uv2 tail packs four
        // pairs per 32-byte element, so a slot's uv2 sub-region is exactly
        // cap/4 elements and `slot * cap` is always 4-aligned — which is what
        // lets the shader turn a global vertex index into (element, lane) with
        // no per-page fixup. Rounding costs at most 3 unused vertices a slot.
        m_SlotVertexCapacity = (std::max(maxPageVertices, 1u) + 3u) & ~3u;
        m_SlotIndexCapacity = std::max(maxPageIndices, 1u);
        auto const totalPages = static_cast<u32>(m_Pages.size());
        u32 slotCount = (m_BudgetSlotsSetting == 0) ? totalPages
                                                    : std::min(m_BudgetSlotsSetting, totalPages);
        u32 const minimumSlots = std::min(pinnedPages + 2u, totalPages);
        if (slotCount < minimumSlots)
        {
            OLO_CORE_WARN("VirtualMeshRegistry: page budget {} below pinned+headroom minimum {} — clamping",
                          slotCount, minimumSlots);
            slotCount = minimumSlots;
        }
        m_SlotCount = slotCount;
        m_ResidencyStats.TotalPages = totalPages;
        m_ResidencyStats.PinnedPages = pinnedPages;
        m_ResidencyStats.BudgetSlots = slotCount;

        // The slot arena's residency directory (#704). Directory-only (no atom
        // storage — the payload arenas are below) and HostOnly (no shader reads
        // it). The substrate hands out free slots lowest-index-first, matching
        // the ascending order the old hand-rolled free list produced. A zero
        // page count (a registered mesh set with no streamable pages) leaves
        // the cache uncreated — LoadPage is unreachable then.
        if (slotCount > 0)
        {
            bool const cacheCreated = m_SlotCache.Create(0, slotCount, GPUCacheBacking::HostOnly);
            OLO_CORE_ASSERT(cacheCreated, "VirtualMeshRegistry: slot cache creation cannot fail host-side");
            m_SlotCache.GetPolicy().SetVictimSelector([this](u64 excludePage, u64& outPage)
                                                      { return SelectResidencyVictim(excludePage, outPage); });
            m_SlotCache.SetEvictionListener([this](const u64& victimPage)
                                            { OnResidencyEvicted(static_cast<u32>(victimPage)); });
        }

        auto uploadPool = [](Ref<StorageBuffer>& buffer, u32 binding, const void* dataPtr, sizet bytes)
        {
            auto const size = static_cast<u32>(bytes);
            if (!buffer || buffer->GetSize() < size)
            {
                buffer = StorageBuffer::Create(size, binding, StorageBufferUsage::DynamicDraw);
            }
            if (dataPtr != nullptr && size > 0)
            {
                buffer->SetData(dataPtr, size, 0);
            }
        };

        uploadPool(m_ClusterBuffer, ShaderBindingLayout::SSBO_VIRTUAL_CLUSTERS,
                   m_PooledClusters.data(), m_PooledClusters.size() * sizeof(VirtualClusterGpuRecord));
        uploadPool(m_GroupBuffer, ShaderBindingLayout::SSBO_VIRTUAL_GROUPS,
                   groups.data(), groups.size() * sizeof(VirtualGroupGpuRecord));

        // Geometry arenas (contents populated per page load).
        //
        // DYNAMIC_COPY, not DYNAMIC_DRAW: these are never written by the CPU. Page loads
        // fill them with glCopyNamedBufferSubData out of the persistent-mapped ring
        // (CopyThroughRing), so the GPU is both producer and consumer. Hinting DRAW made
        // NVIDIA place them for CPU-write access and log, every frame:
        //   "Buffer usage warning: ... the GPU is the primary producer and consumer ...
        //    GL_DYNAMIC_DRAW is inconsistent with this usage pattern. Try GL_DYNAMIC_COPY"
        u64 const vertexElements = static_cast<u64>(m_SlotVertexCapacity) * slotCount;

        // The baked lightmap uv2 tail (issue #867) — see MeshHasLightmapUVs in
        // the header for why it lives inside this buffer rather than getting a
        // binding. Allocated only when some registered mesh carries UV2, so an
        // unbaked scene's arena is byte-for-byte what it was before.
        const bool anyLightmapUVs =
            std::ranges::any_of(m_Entries, [](const MeshEntry& entry)
                                { return entry.Packed.LightmapUVs.size() == entry.Packed.Vertices.size() &&
                                         !entry.Packed.LightmapUVs.empty(); });
        u64 const lightmapElements =
            anyLightmapUVs ? VirtualLightmapUVElementCount(static_cast<u32>(vertexElements)) : 0u;
        m_LightmapUVBaseElement = anyLightmapUVs ? static_cast<u32>(vertexElements) : 0u;

        u64 const vertexArenaBytes = (vertexElements + lightmapElements) * sizeof(VirtualGpuVertex);
        if (!m_VertexBuffer || m_VertexBuffer->GetSize() < vertexArenaBytes)
        {
            m_VertexBuffer = StorageBuffer::Create(static_cast<u32>(vertexArenaBytes),
                                                   ShaderBindingLayout::SSBO_VIRTUAL_VERTICES,
                                                   StorageBufferUsage::DynamicCopy);
        }
        u64 const indexArenaBytes = static_cast<u64>(m_SlotIndexCapacity) * slotCount * sizeof(u32);
        if (!m_IndexBuffer.IsValid())
        {
            m_IndexBuffer = RenderCommand::CreateBufferHandle();
        }
        RenderCommand::AllocateBufferStorage(m_IndexBuffer, indexArenaBytes, RHI::MemoryResidency::DeviceLocal);
        if (!m_Vao.IsValid())
        {
            m_Vao = RenderCommand::CreateVertexArrayHandle();
        }
        RenderCommand::SetVertexArrayIndexBuffer(m_Vao, m_IndexBuffer);

        // Persistent-mapped upload ring (fence-locked, #704). A failed create
        // (no device / mapping failure) degrades to direct uploads inside
        // CopyThroughRing, exactly as the old null-m_RingPtr path did.
        if (!m_UploadRing.IsCreated())
        {
            [[maybe_unused]] bool const ringCreated = m_UploadRing.Create(kUploadRingBytes);
        }

        // Residency reset: nothing resident, then load pinned pages (always) and
        // — when the budget fits everything — every page eagerly so the default
        // configuration has no pop-in.
        m_GroupStatesCpu.assign(groups.size(), 0u);
        for (PageRuntime& page : m_Pages)
        {
            page.Resident = false;
            page.SlotIndex = kNoSlot;
            page.LastUsedFrame = 0;
        }
        bool const eager = (slotCount >= totalPages);
        for (u32 p = 0; p < m_Pages.size(); ++p)
        {
            if (m_Pages[p].Pinned || eager)
            {
                LoadPage(p);
            }
        }

        auto const statesBytes = static_cast<u32>(m_GroupStatesCpu.size() * sizeof(u32));
        if (!m_GroupStatesBuffer || m_GroupStatesBuffer->GetSize() < statesBytes)
        {
            m_GroupStatesBuffer = StorageBuffer::Create(statesBytes,
                                                        ShaderBindingLayout::SSBO_VIRTUAL_GROUP_STATES,
                                                        StorageBufferUsage::DynamicCopy);
        }
        m_GroupStatesBuffer->SetData(m_GroupStatesCpu.data(), statesBytes, 0);

        // The group count just changed size, so any readback ring slot sized
        // for the old count would decode garbage next poll — drop the ring and
        // let ProcessResidency recreate it at the new size. Whatever it was
        // draining is moot: the buffer it read from no longer exists in this
        // shape, and this full publish above is already authoritative.
        DestroyResidencyReadbackSlots();
        m_DirtyResidencyGroups.clear();

        m_PoolsDirty = false;

        OLO_CORE_TRACE("VirtualMeshRegistry: pools rebuilt — {} clusters, {} groups, {} pages ({} pinned), "
                       "{} slots x ({} verts / {} indices), {} resident",
                       m_PooledClusters.size(), groups.size(), totalPages, pinnedPages,
                       slotCount, m_SlotVertexCapacity, m_SlotIndexCapacity, m_ResidencyStats.ResidentPages);
    }

    void VirtualMeshRegistry::ProcessResidency()
    {
        OLO_PROFILE_FUNCTION();

        if (m_Pages.empty() || !m_GroupStatesBuffer || m_ResidencyProcessed)
        {
            return;
        }
        m_ResidencyProcessed = true;
        ++m_FrameCounter;

        // Fully-resident configurations skip the readback entirely.
        if (m_ResidencyStats.ResidentPages == m_ResidencyStats.TotalPages)
        {
            return;
        }

        if (!EnsureResidencyReadbackSlots())
        {
            return;
        }

        CaptureResidencyStates();
        PollResidencyReadback();
        m_ResidencyStats.RequestReadbackSlotsInFlight = m_ResidencyReadbackSlotsInFlight;
    }

    bool VirtualMeshRegistry::EnsureResidencyReadbackSlots()
    {
        auto const bytes = static_cast<u32>(m_GroupStatesCpu.size() * sizeof(u32));
        if (bytes == 0)
        {
            return false;
        }
        if (m_ResidencyReadbackBytes == bytes && m_ResidencyReadbackSlots[0].m_Buffer.IsValid())
        {
            return true;
        }

        DestroyResidencyReadbackSlots();
        for (ResidencyReadbackSlot& slot : m_ResidencyReadbackSlots)
        {
            slot.m_Buffer = RenderCommand::CreateBufferHandle();
            // DeviceToHost, not persistent-mapped: the RHI's persistent mapping
            // is WRITE-only (docs/agent-rules/gpu-readback-stats-channel.md
            // §3), so a readback goes through a device-to-host buffer +
            // glGetBufferSubData. What keeps this from stalling is the FENCE in
            // CaptureResidencyStates/PollResidencyReadback, not the mapping.
            RenderCommand::AllocateBufferStorage(slot.m_Buffer, bytes, RHI::MemoryResidency::DeviceToHost);
        }
        m_ResidencyReadbackBytes = bytes;
        m_NextResidencyReadbackSlot = 0;
        return true;
    }

    void VirtualMeshRegistry::DestroyResidencyReadbackSlots()
    {
        bool const deviceAlive = RenderCommand::IsDeviceAvailable();
        for (ResidencyReadbackSlot& slot : m_ResidencyReadbackSlots)
        {
            if (deviceAlive)
            {
                if (slot.m_Fence != 0)
                {
                    RenderCommand::DestroyFence(slot.m_Fence);
                }
                if (slot.m_Buffer.IsValid())
                {
                    RenderCommand::DeleteBuffer(slot.m_Buffer);
                }
            }
            slot = ResidencyReadbackSlot{};
        }
        m_ResidencyReadbackBytes = 0;
        m_NextResidencyReadbackSlot = 0;
        m_ResidencyReadbackSlotsInFlight = 0;
    }

    void VirtualMeshRegistry::CaptureResidencyStates()
    {
        // The cull dispatches OR request/touch bits into this buffer; make
        // those writes visible to the copy below.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::BufferUpdate);

        ResidencyReadbackSlot& slot = m_ResidencyReadbackSlots[m_NextResidencyReadbackSlot];
        if (!slot.m_Pending && slot.m_Buffer.IsValid())
        {
            RenderCommand::CopyBufferSubData(m_GroupStatesBuffer->GetRHIHandle(), slot.m_Buffer, 0, 0,
                                             m_ResidencyReadbackBytes);
            slot.m_Fence = RenderCommand::CreateFence();
            if (slot.m_Fence == 0)
            {
                // A pending slot with a zero fence would never poll signaled,
                // permanently wedging this ring slot (mirrors
                // GPUReadbackStats::EndFrame's handling of the same case).
                // Leave the slot free and drop this capture rather than that.
                OLO_CORE_WARN("VirtualMeshRegistry: residency fence creation failed; dropping this capture");
            }
            else
            {
                slot.m_Pending = true;
                m_NextResidencyReadbackSlot = (m_NextResidencyReadbackSlot + 1u) % kResidencyReadbackSlots;
            }
        }
        // A full ring means every slot is still executing — skip the capture
        // and keep the newest retired snapshot instead of blocking on the
        // oldest.

        // Unconditional, including on the skipped-capture path (mirrors
        // TerrainVirtualTexture::CaptureFeedback). The copy above already has
        // whatever this word held; leaving the request/touch bits in place
        // instead of resetting them here would freeze LastUsedFrame at
        // "touched" forever for a page the camera turned away from frames ago
        // (nothing else clears them — the SlotCache LRU has no other signal),
        // corrupting eviction, and would make an abandoned request read as
        // permanently pending. Nothing is lost by resetting: THIS frame's cull
        // dispatch, which always runs right after ProcessResidency returns,
        // re-derives and re-asserts via atomicOr whatever is still actually
        // true — exactly the cadence the pre-#719 synchronous version had
        // (it republished resident-only bits at the end of every call).
        m_GroupStatesBuffer->SetData(m_GroupStatesCpu.data(),
                                     static_cast<u32>(m_GroupStatesCpu.size() * sizeof(u32)), 0);
    }

    void VirtualMeshRegistry::PollResidencyReadback()
    {
        u32 inFlight = 0;
        // One upload budget for the WHOLE call, not one per snapshot: several
        // slots can signal in the same frame (e.g. the GPU catching up after a
        // hitch), and a per-snapshot budget would let one call load up to
        // kResidencyReadbackSlots * m_MaxPageUploadsPerFrame pages — the exact
        // per-frame spike this cap exists to prevent, since each load stages
        // through the finite CopyThroughRing upload ring.
        u32 uploadBudget = m_MaxPageUploadsPerFrame;
        // OLDEST FIRST, not array order. m_NextResidencyReadbackSlot is the
        // slot the NEXT capture will use, so it is also the oldest one still
        // in flight; walking from there wraps the ring in ISSUE order. Array
        // order would let a wrapped ring apply an older snapshot after a newer
        // one already applied — residency decided from a camera pose that has
        // already moved on.
        for (u32 offset = 0; offset < kResidencyReadbackSlots; ++offset)
        {
            ResidencyReadbackSlot& slot =
                m_ResidencyReadbackSlots[(m_NextResidencyReadbackSlot + offset) % kResidencyReadbackSlots];
            if (!slot.m_Pending)
            {
                continue;
            }
            // Ask, never wait. IsFenceSignaled is a poll — no ClientWaitFence
            // anywhere in this path, ever: a slot the GPU has not finished
            // copying into is simply left for a later frame.
            if (!RenderCommand::IsFenceSignaled(slot.m_Fence))
            {
                ++inFlight;
                continue;
            }

            std::vector<u32> gpuStates(m_GroupStatesCpu.size());
            RenderCommand::ReadBufferSubData(slot.m_Buffer, 0, m_ResidencyReadbackBytes, gpuStates.data());

            RenderCommand::DestroyFence(slot.m_Fence);
            slot.m_Fence = 0;
            slot.m_Pending = false;

            ApplyResidencySnapshot(gpuStates, uploadBudget);
        }
        m_ResidencyReadbackSlotsInFlight = inFlight;
    }

    void VirtualMeshRegistry::ApplyResidencySnapshot(const std::vector<u32>& gpuStates, u32& uploadBudget)
    {
        // LRU touches first so this snapshot's loads cannot evict just-used pages.
        for (u32 g = 0; g < gpuStates.size(); ++g)
        {
            if ((gpuStates[g] & kStateTouched) != 0u)
            {
                u32 const pageIndex = m_PageOfPooledGroup[g];
                if (pageIndex != kNoSlot)
                {
                    m_Pages[pageIndex].LastUsedFrame = m_FrameCounter;
                }
            }
        }

        for (u32 g = 0; g < gpuStates.size() && uploadBudget > 0; ++g)
        {
            if ((gpuStates[g] & kStateRequested) == 0u)
            {
                continue;
            }
            u32 const pageIndex = m_PageOfPooledGroup[g];
            if (pageIndex == kNoSlot || m_Pages[pageIndex].Resident)
            {
                continue;
            }
            if (LoadPage(pageIndex))
            {
                --uploadBudget;
            }
        }

        // Publish exactly the groups LoadPage/OnResidencyEvicted touched above —
        // a single-word write per group, never a whole-array overwrite. The
        // snapshot this call just applied is already stale, for groups it did
        // NOT touch, against whatever the GPU has OR'd into them on frames
        // after the snapshot was captured; a whole-array republish would stomp
        // those live bits. A stale request/touch bit left on an untouched word
        // is harmless and self-heals (a still-missing page just stays
        // requested; a stale touch only makes LRU a little more conservative)
        // — see docs/agent-rules/gpu-readback-stats-channel.md §5 on why a
        // reserved/observed slot must never be handed back on a guess.
        //
        // Deduplicated: an eviction cascade (LoadPage's slot allocation can
        // evict a victim as a side effect) can push the same group in twice
        // within one call — harmless either way (last write wins, and it
        // always reads the current m_GroupStatesCpu value), but redundant.
        std::sort(m_DirtyResidencyGroups.begin(), m_DirtyResidencyGroups.end());
        m_DirtyResidencyGroups.erase(std::unique(m_DirtyResidencyGroups.begin(), m_DirtyResidencyGroups.end()),
                                     m_DirtyResidencyGroups.end());
        for (u32 group : m_DirtyResidencyGroups)
        {
            u32 const value = m_GroupStatesCpu[group];
            m_GroupStatesBuffer->SetData(&value, sizeof(u32), group * static_cast<u32>(sizeof(u32)));
        }
        m_DirtyResidencyGroups.clear();
    }

    void VirtualMeshRegistry::EnsureVisbuffer(u32 viewportWidth, u32 viewportHeight)
    {
        auto const visbufferBytes = viewportWidth * viewportHeight * 8u;
        if (visbufferBytes > 0 && (!m_VisbufferBuffer || m_VisbufferBuffer->GetSize() < visbufferBytes))
        {
            m_VisbufferBuffer = StorageBuffer::Create(visbufferBytes,
                                                      ShaderBindingLayout::SSBO_VIRTUAL_VISBUFFER,
                                                      StorageBufferUsage::DynamicCopy);
        }
        m_VisbufferWidth = viewportWidth;
        m_VisbufferHeight = viewportHeight;

        // Clear to "empty" (all bits set: farthest depth + sentinel payload)
        if (m_VisbufferBuffer)
        {
            u32 const clearValue = 0xFFFFFFFFu;
            RenderCommand::ClearBufferUInt(m_VisbufferBuffer->GetRHIHandle(), clearValue);
        }
    }

    VirtualCullStats VirtualMeshRegistry::ReadFrameCullStats() const
    {
        VirtualCullStats stats;
        if (m_FrameInstances.empty() || !m_ArgsBuffer)
            return stats;

        stats.InstanceCount = static_cast<u32>(m_FrameInstances.size());
        // Both phase regions: [0, n) is phase 1, [n, 2n) is phase 2 (issue #682).
        std::vector<VirtualDrawArgs> args(m_FrameInstances.size() * 2);
        auto const bytes = static_cast<u32>(args.size() * sizeof(VirtualDrawArgs));

        // Stage the args through a dedicated GL_DYNAMIC_READ buffer rather than reading
        // m_ArgsBuffer directly. m_ArgsBuffer is GL_DYNAMIC_COPY and must stay that way:
        // the GPU both writes it (the cull compute) and reads it every frame as the
        // glMultiDrawElementsIndirectCount GL_PARAMETER_BUFFER, so it has to live in video
        // memory. A CPU glGetNamedBufferSubData straight off it makes NVIDIA log "Analysis
        // of buffer object N usage indicates that CPU is consuming buffer object data. The
        // usage hint ... GL_DYNAMIC_COPY, is inconsistent with this usage pattern" (131188)
        // and then migrate the buffer VIDEO -> HOST (perf warning 131186) — permanently
        // slowing the indirect draws that read it.
        if (!m_ArgsReadback.IsValid() || m_ArgsReadbackBytes < bytes)
        {
            if (m_ArgsReadback.IsValid())
            {
                RenderCommand::DeleteBuffer(m_ArgsReadback);
                m_ArgsReadback = RHI::NullResource;
            }
            m_ArgsReadback = RenderCommand::CreateBufferHandle();
            RenderCommand::AllocateBufferStorage(m_ArgsReadback, bytes, RHI::MemoryResidency::DeviceToHost);
            m_ArgsReadbackBytes = bytes;
        }
        RenderCommand::CopyBufferSubData(m_ArgsBuffer->GetRHIHandle(), m_ArgsReadback, 0, 0, bytes);
        RenderCommand::ReadBufferSubData(m_ArgsReadback, 0, bytes, args.data());
        sizet const phaseStride = m_FrameInstances.size();
        for (sizet i = 0; i < args.size(); ++i)
        {
            const VirtualDrawArgs& a = args[i];
            bool const phase2 = i >= phaseStride;
            // Only phase 1 runs the DAG cut, so its tested/selected counters are
            // the frame's — phase 2 re-tests occlusion for an already-selected
            // subset and would double-count them.
            if (!phase2)
            {
                stats.TestedClusters += a.TestedCount;
                stats.CutSelected += a.CutSelected;
            }
            else
            {
                stats.Phase2Recovered += a.DrawCount + a.SwCount;
            }
            stats.HardwareDraws += a.DrawCount;
            stats.SoftwareRasterized += a.SwCount;
        }
        return stats;
    }

    void VirtualMeshRegistry::EnsureDebugTargets(u32 viewportWidth, u32 viewportHeight)
    {
        if (viewportWidth == 0 || viewportHeight == 0)
            return;

        if (!m_DebugColorTex.IsValid() || m_DebugWidth != viewportWidth || m_DebugHeight != viewportHeight)
        {
            if (m_DebugColorTex.IsValid())
                RenderCommand::DeleteTexture(m_DebugColorTex);
            if (m_DebugCountTex.IsValid())
                RenderCommand::DeleteTexture(m_DebugCountTex);

            // RGBA8 colour target — imageStore'd by both raster paths, imported
            // into the graph as "VirtualGeometryDebug", captured via MCP.
            m_DebugColorTex = RenderCommand::CreateTexture2DHandle(viewportWidth, viewportHeight,
                                                                   RHI::Format::RGBA8UNorm);
            RenderCommand::SetTextureFilter(m_DebugColorTex, RHI::Filter::Nearest, RHI::Filter::Nearest);

            // R32UI overdraw-count target — imageAtomicAdd'd per fragment, then
            // colorized into the colour target by VirtualDebugColorize.comp.
            m_DebugCountTex = RenderCommand::CreateTexture2DHandle(viewportWidth, viewportHeight,
                                                                   RHI::Format::R32UInt);
            RenderCommand::SetTextureFilter(m_DebugCountTex, RHI::Filter::Nearest, RHI::Filter::Nearest);

            m_DebugWidth = viewportWidth;
            m_DebugHeight = viewportHeight;
        }

        // Clear both targets for this frame (colour -> TRANSPARENT black, count -> 0).
        //
        // Alpha is the "a virtual fragment landed here" bit, and the viewport overlay
        // (VirtualDebugOverlay.glsl, composited at the end of DeferredLightingPass) depends on
        // it: it discards where alpha == 0 so the lit frame shows through, and replaces the
        // pixel where alpha == 1. Clearing to OPAQUE black — which this used to do — makes
        // every pixel look "written" and blacks out the entire viewport outside the virtual
        // geometry. Both writers set alpha = 1 exactly where they touch a pixel
        // (VirtualDebugViz.glsl's imageStore; VirtualDebugColorize.comp for a non-zero count).
        //
        // Nothing reads this alpha as colour: the debug capture target is inspected per-RGB.
        {
            constexpr glm::vec4 kTransparentBlack(0.0f);
            RenderCommand::ClearTextureFloat(m_DebugColorTex, 0, kTransparentBlack);
            RenderCommand::ClearTextureUInt(m_DebugCountTex, 0, 0u);
        }
    }

    void VirtualMeshRegistry::EnsureFrameBuffers()
    {
        auto const instanceCount = static_cast<u32>(m_FrameInstances.size());
        auto const instanceBytes = instanceCount * static_cast<u32>(sizeof(VirtualInstanceGpuRecord));
        if (!m_InstanceBuffer || m_InstanceBuffer->GetSize() < instanceBytes)
        {
            m_InstanceBuffer = StorageBuffer::Create(std::max(instanceBytes, 1024u),
                                                     ShaderBindingLayout::SSBO_VIRTUAL_INSTANCES,
                                                     StorageBufferUsage::DynamicDraw);
        }

        // Two regions of everything the cull compacts into, one per occlusion
        // phase (issue #682). Phase 2 cannot append to phase 1's segment: the
        // phase-1 MDI has already read its parameter word and replayed its
        // commands by the time phase 2 runs, so a shared segment would make the
        // second replay re-issue every phase-1 draw. The phase-2 region starts at
        // a CPU-known fixed offset — cluster count for commands/visible, instance
        // count for args — which is what lets the second MDI name it without a
        // GPU readback. Each cluster is emitted by at most one phase, so the
        // second region is never more than a duplicate worst case.
        auto const commandBytes = m_TotalFrameClusterCount * 32u * 2u; // DrawElementsIndirectCommand stride
        if (!m_CommandBuffer || m_CommandBuffer->GetSize() < commandBytes)
        {
            m_CommandBuffer = StorageBuffer::Create(std::max(commandBytes, 1024u),
                                                    ShaderBindingLayout::SSBO_VIRTUAL_DRAW_COMMANDS,
                                                    StorageBufferUsage::DynamicCopy);
        }

        auto const argsBytes = instanceCount * static_cast<u32>(sizeof(VirtualDrawArgs)) * 2u;
        if (!m_ArgsBuffer || m_ArgsBuffer->GetSize() < argsBytes)
        {
            m_ArgsBuffer = StorageBuffer::Create(std::max(argsBytes, 2u * static_cast<u32>(sizeof(VirtualDrawArgs))),
                                                 ShaderBindingLayout::SSBO_VIRTUAL_DRAW_ARGS,
                                                 StorageBufferUsage::DynamicCopy);
        }

        auto const visibleBytes = m_TotalFrameClusterCount * static_cast<u32>(sizeof(VirtualVisibleCluster)) * 2u;
        if (!m_VisibleBuffer || m_VisibleBuffer->GetSize() < visibleBytes)
        {
            m_VisibleBuffer = StorageBuffer::Create(std::max(visibleBytes, 1024u),
                                                    ShaderBindingLayout::SSBO_VIRTUAL_VISIBLE,
                                                    StorageBufferUsage::DynamicCopy);
        }

        // Software-raster work list: 16-byte header + one record per cluster.
        // Shared by both phases — the SW rasterizer runs once, after phase 2, so
        // it consumes the union list in a single dispatch.
        auto const swListBytes = 16u + m_TotalFrameClusterCount * static_cast<u32>(sizeof(VirtualVisibleCluster));
        if (!m_SwListBuffer || m_SwListBuffer->GetSize() < swListBytes)
        {
            m_SwListBuffer = StorageBuffer::Create(std::max(swListBytes, 1024u),
                                                   ShaderBindingLayout::SSBO_VIRTUAL_SW_LIST,
                                                   StorageBufferUsage::DynamicCopy);
        }

        // Two-phase reject list: same shape as the SW list, one record per
        // cluster (each cluster gets exactly one phase-1 thread, so that is the
        // exact worst case — the shader still bounds-checks the append).
        auto const rejectBytes = 16u + m_TotalFrameClusterCount * static_cast<u32>(sizeof(VirtualVisibleCluster));
        if (!m_RejectListBuffer || m_RejectListBuffer->GetSize() < rejectBytes)
        {
            m_RejectListBuffer = StorageBuffer::Create(std::max(rejectBytes, 1024u),
                                                       ShaderBindingLayout::SSBO_VIRTUAL_REJECTED,
                                                       StorageBufferUsage::DynamicCopy);
        }
    }

    bool VirtualMeshRegistry::PrepareFrame(const glm::vec3& renderOrigin)
    {
        OLO_PROFILE_FUNCTION();

        if (m_FramePrepared)
        {
            return m_FramePreparedResult;
        }
        m_FramePrepared = true;
        m_FramePreparedResult = false;

        if (m_Submissions.empty())
        {
            return false;
        }

        if (m_PoolsDirty)
        {
            RebuildPools();
        }
        if (!m_ClusterBuffer)
        {
            return false; // no valid mesh has ever been registered
        }

        m_FrameInstances.clear();
        m_FrameInstances.reserve(m_Submissions.size());
        m_TotalFrameClusterCount = 0;

        for (const VirtualMeshSubmission& submission : m_Submissions)
        {
            MeshParts const parts = FindParts(submission.Mesh);
            if (!parts.Valid)
            {
                continue;
            }

            // Degenerate (zero-scale) transforms would poison the projected-error
            // math with 0 * FLT_MAX = NaN — nothing would be visible anyway.
            f32 const scaleX = glm::length(glm::vec3(submission.Transform[0]));
            f32 const scaleY = glm::length(glm::vec3(submission.Transform[1]));
            f32 const scaleZ = glm::length(glm::vec3(submission.Transform[2]));
            f32 const maxScale = std::max({ scaleX, scaleY, scaleZ });
            f32 const minScale = std::min({ scaleX, scaleY, scaleZ });
            if (!(minScale > 1e-12f))
            {
                continue;
            }

            // One GPU instance PER PART. Each part is an independent DAG over one submesh, so
            // it gets its own cluster range and its own material — which is what makes a
            // multi-material mesh work with no shader change: the cull, the raster and the
            // material resolve already operate per instance.
            for (u32 partIndex = 0; partIndex < parts.Count; ++partIndex)
            {
                const MeshEntry& entry = m_Entries[parts.FirstEntry + partIndex];
                if (!entry.Valid)
                {
                    continue;
                }

                FrameInstance instance;
                // A submission always carries one material slot per part; fall back to the
                // first if a caller ever under-fills it rather than reading out of bounds.
                instance.MaterialDataIndex =
                    partIndex < submission.MaterialDataIndices.size()
                        ? submission.MaterialDataIndices[partIndex]
                        : (submission.MaterialDataIndices.empty() ? 0u : submission.MaterialDataIndices.front());
                bool const partAlphaMasked = partIndex < submission.PartAlphaMasked.size() &&
                                             submission.PartAlphaMasked[partIndex] != 0u;

                // ── AlphaMode::Blend is NOT representable in the virtual path (issue #629) ──
                // The classic path (Renderer3DDrawHelpers::BuildRenderState) enables GL_BLEND
                // and disables depth-write for MaterialFlag::Blend. Virtual geometry rasterizes
                // into the DEFERRED G-Buffer, which stores one opaque surface per pixel and has
                // nowhere to put a blended fragment — the pass forces glDepthMask(GL_TRUE) +
                // glDisable(GL_BLEND) for every instance, so a Blend part was written into the
                // G-Buffer FULLY OPAQUE. Drawing it wrong is worse than not drawing it, so skip
                // it and say so, once per mesh (this runs every frame).
                if (partIndex < submission.MaterialDataIndices.size() &&
                    FrameDataBufferManager::Get()
                            .GetMaterialData(static_cast<u16>(submission.MaterialDataIndices[partIndex]))
                            .alphaMode == static_cast<i32>(AlphaMode::Blend))
                {
                    if (m_BlendRejectionWarned.insert(static_cast<u64>(submission.Mesh)).second)
                    {
                        OLO_CORE_WARN("VirtualMeshRegistry: mesh asset {} part {} uses AlphaMode::Blend, which the "
                                      "virtualized-geometry (deferred G-Buffer) path cannot express — the part is "
                                      "SKIPPED. Use AlphaMode::Mask, or draw it with a classic MeshComponent so it "
                                      "goes through the forward/transparent pass.",
                                      static_cast<u64>(submission.Mesh), partIndex);
                    }
                    continue;
                }

                // Alpha-masked parts do NOT cast shadows, matching the classic path
                // (Scene.cpp's MeshComponent/ModelComponent loops): the shared shadow-depth
                // shader never samples the albedo alpha, so a cutout leaf card would project
                // as a SOLID quad silhouette instead of a leaf.
                instance.CastShadows = submission.CastShadows && !partAlphaMasked;
                instance.TwoSided = partIndex < submission.PartTwoSided.size() &&
                                    submission.PartTwoSided[partIndex] != 0u;
                instance.MeshletCompatible = entry.MeshletCompatible;

                VirtualInstanceGpuRecord& gpu = instance.Gpu;
                gpu.Transform = submission.Transform;
                gpu.PrevTransform = submission.PrevTransform;
                // Camera-relative rendering (#429): shift translations by the render origin
                gpu.Transform[3] -= glm::vec4(renderOrigin, 0.0f);
                gpu.PrevTransform[3] -= glm::vec4(renderOrigin, 0.0f);
                gpu.NormalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(submission.Transform))));
                gpu.ClusterBase = entry.ClusterBase;
                gpu.ClusterCount = static_cast<u32>(entry.Packed.Clusters.size());
                gpu.GroupBase = entry.GroupBase;
                gpu.EntityID = submission.EntityID;
                gpu.ErrorThresholdPixels = submission.ErrorThresholdPixels;
                // Baked lightmap atlas region (issue #867). Per instance, not
                // per part: one VirtualMeshComponent is one MeshSource and so
                // one unwrap and one region, and the software-raster resolve
                // reaches its surface through the instance index rather than
                // through a draw call.
                gpu.LightmapScaleOffset = submission.LightmapScaleOffset;
                gpu.CommandBase = m_TotalFrameClusterCount;

                // Conservative world-space sphere scaling + cone validity
                gpu.MaxScale = maxScale;
                gpu.Flags = 0;
                if ((maxScale / minScale) < 1.01f)
                {
                    gpu.Flags |= VirtualInstanceGpuRecord::kFlagUniformScale;
                }
                if (partAlphaMasked)
                {
                    gpu.Flags |= VirtualInstanceGpuRecord::kFlagAlphaMasked;
                }
                // Two-sidedness has to reach the GPU as well as the CPU draw loop: the cull's
                // normal-cone rejection and the software rasterizer's backface cull both need
                // to know (see VirtualInstanceGpuRecord::kFlagTwoSided).
                if (instance.TwoSided)
                {
                    gpu.Flags |= VirtualInstanceGpuRecord::kFlagTwoSided;
                }

                m_TotalFrameClusterCount += gpu.ClusterCount;
                m_FrameInstances.push_back(instance);
            }
        }

        if (m_FrameInstances.empty())
        {
            return false;
        }

        EnsureFrameBuffers();

        std::vector<VirtualInstanceGpuRecord> gpuRecords;
        gpuRecords.reserve(m_FrameInstances.size());
        for (const FrameInstance& instance : m_FrameInstances)
        {
            gpuRecords.push_back(instance.Gpu);
        }
        m_InstanceBuffer->SetData(gpuRecords.data(),
                                  static_cast<u32>(gpuRecords.size() * sizeof(VirtualInstanceGpuRecord)), 0);

        // Zero this frame's draw counts + stats before the cull dispatches —
        // BOTH phase regions (issue #682).
        std::vector<VirtualDrawArgs> const zeroArgs(m_FrameInstances.size() * 2);
        m_ArgsBuffer->SetData(zeroArgs.data(),
                              static_cast<u32>(zeroArgs.size() * sizeof(VirtualDrawArgs)), 0);

        // Zero the software-raster + two-phase reject list headers (Count + padding)
        u32 const zeroHeader[4] = { 0, 0, 0, 0 };
        m_SwListBuffer->SetData(zeroHeader, sizeof(zeroHeader), 0);
        m_RejectListBuffer->SetData(zeroHeader, sizeof(zeroHeader), 0);

        m_FramePreparedResult = true;
        return true;
    }

    void VirtualMeshRegistry::Shutdown()
    {
        m_ClusterBuffer = nullptr;
        m_GroupBuffer = nullptr;
        m_GroupStatesBuffer = nullptr;
        m_VertexBuffer = nullptr;
        m_LightmapUVBaseElement = 0;
        m_InstanceBuffer = nullptr;
        m_CommandBuffer = nullptr;
        m_ArgsBuffer = nullptr;
        m_VisibleBuffer = nullptr;
        m_SwListBuffer = nullptr;
        m_RejectListBuffer = nullptr;
        m_VisbufferBuffer = nullptr;
        m_VisbufferWidth = 0;
        m_VisbufferHeight = 0;
        if (m_ArgsReadback.IsValid())
        {
            RenderCommand::DeleteBuffer(m_ArgsReadback);
            m_ArgsReadback = RHI::NullResource;
            m_ArgsReadbackBytes = 0;
        }
        m_UploadRing.Destroy();
        DestroyResidencyReadbackSlots();
        m_DirtyResidencyGroups.clear();
        if (m_Vao.IsValid())
        {
            RenderCommand::DeleteVertexArray(m_Vao);
            m_Vao = RHI::NullResource;
        }
        if (m_IndexBuffer.IsValid())
        {
            RenderCommand::DeleteBuffer(m_IndexBuffer);
            m_IndexBuffer = RHI::NullResource;
        }
        if (m_DebugColorTex.IsValid())
        {
            RenderCommand::DeleteTexture(m_DebugColorTex);
            m_DebugColorTex = RHI::NullResource;
        }
        if (m_DebugCountTex.IsValid())
        {
            RenderCommand::DeleteTexture(m_DebugCountTex);
            m_DebugCountTex = RHI::NullResource;
        }
        m_DebugWidth = 0;
        m_DebugHeight = 0;
        m_Entries.clear();
        m_EntryLookup.clear();
        m_BlendRejectionWarned.clear();
        m_Submissions.clear();
        m_FrameInstances.clear();
        m_Pages.clear();
        m_PageOfPooledGroup.clear();
        m_PooledClusters.clear();
        m_GroupStatesCpu.clear();
        m_SlotCache.Destroy();
        m_ResidencyStats = {};
        m_FrameCounter = 0;
        m_PoolsDirty = false;
    }
} // namespace OloEngine
