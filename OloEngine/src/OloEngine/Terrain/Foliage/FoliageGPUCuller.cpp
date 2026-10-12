#include "OloEnginePCH.h"
#include "OloEngine/Terrain/Foliage/FoliageGPUCuller.h"

#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/Debug/StagedBufferReadback.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/VertexBuffer.h"

#include <algorithm>
#include <bit>
#include <limits>

namespace OloEngine
{
    namespace
    {
        constexpr u32 kGroupWorkgroupSize = 64;
        constexpr u32 kInstanceWorkgroupSize = 256;

        // The dispatch-local halves of the binding set. 15/16/17 are the
        // instance-cull family's own numbers used for their own roles; 18/19 are
        // the two dispatch-local slots GPUFrustumCuller already borrows. See the
        // binding note at the top of FoliageCullCommon.glsl.
        constexpr u32 kLayerBinding = 18;
        constexpr u32 kStateBinding = 19;

        // A row whose canonical record could not be found. The shader rejects
        // any group index >= s_GroupCount, so a tail left at this value would
        // make the plant vanish with no diagnostic — which is why BuildLayer
        // refuses the whole layer instead (see its comment).
        constexpr u32 kUnmappedRow = ~0u;

        [[nodiscard]] glm::vec4 PlaneToVec4(const Plane& plane)
        {
            return { plane.Normal.x, plane.Normal.y, plane.Normal.z, plane.Distance };
        }

        [[nodiscard]] u32 DispatchGroups(u32 items, u32 workgroupSize)
        {
            return (items + workgroupSize - 1u) / workgroupSize;
        }
    } // namespace

    void FoliageGPUCuller::EnsureInitialised()
    {
        if (m_Initialised || m_LoadFailed)
        {
            return;
        }

        m_GroupCullShader = ComputeShader::Create("assets/shaders/compute/FoliageGroupCull.comp");
        m_InstanceCullShader = ComputeShader::Create("assets/shaders/compute/FoliageInstanceCull.comp");

        const bool ok = m_GroupCullShader && m_GroupCullShader->IsValid() &&
                        m_InstanceCullShader && m_InstanceCullShader->IsValid();
        if (!ok)
        {
            // LOUD, once, and the caller keeps drawing every instance — a
            // correct frame that costs more, never a quietly empty one.
            OLO_CORE_ERROR("FoliageGPUCuller: compute shader load failed (group={}, instance={}) — foliage "
                           "falls back to submitting every generated instance, uncompacted",
                           m_GroupCullShader && m_GroupCullShader->IsValid(),
                           m_InstanceCullShader && m_InstanceCullShader->IsValid());
            m_LoadFailed = true;
            m_GroupCullShader.Reset();
            m_InstanceCullShader.Reset();
            return;
        }

        // Optional (#1580): the clip-level variant only adds the dirty-page
        // test. Without it a clip level culls like any view -- every group in
        // its frustum -- which draws more and is still correct; said once.
        m_GroupCullVsmShader = ComputeShader::Create("assets/shaders/compute/FoliageGroupCullVsm.comp");
        if (!m_GroupCullVsmShader || !m_GroupCullVsmShader->IsValid())
        {
            OLO_CORE_ERROR("FoliageGPUCuller: FoliageGroupCullVsm.comp did not load -- virtual-shadow-map clip levels "
                           "cull foliage without the dirty-page test and redraw every cached page they cover");
            m_GroupCullVsmShader.Reset();
        }

        m_Initialised = true;
    }

    bool FoliageGPUCuller::BuildLayer(LayerResources& out, const FoliageInstanceRegistry& registry, u32 layerIndex,
                                      u32 instanceCount, const FoliageBoundsProfile& profile)
    {
        OLO_PROFILE_FUNCTION();

        if (instanceCount == 0)
        {
            out = {};
            return false;
        }

        const u64 generation = registry.GetGeneration();
        if (out.BuiltGeneration == generation && out.InstanceCount == instanceCount)
        {
            // Includes the REFUSED case. Without that arm a refusal resets
            // BuiltGeneration, so nothing ever caches: the full record scan and
            // its error line run again for every view slot, every frame --
            // hundreds of identical lines a second, which is how a diagnostic
            // stops being read at all.
            if (out.IsValid())
                return true;
            if (out.RefusedForBuiltGeneration)
                return false;
        }

        // Latches a refusal against this generation. A later regeneration moves
        // the generation and the layer is reconsidered from scratch.
        const auto refuse = [&out, instanceCount, generation]()
        {
            out = {};
            out.InstanceCount = instanceCount;
            out.BuiltGeneration = generation;
            out.RefusedForBuiltGeneration = true;
            return false;
        };

        const auto prepared = PrepareLayer(registry, layerIndex, instanceCount, profile);
        if (!BeginLayerUpload(out, prepared))
            return refuse();
        out.LayerBuffer->SetData(prepared.Words.GetData(), static_cast<u32>(prepared.Words.Num() * sizeof(u32)), 0);
        return true;
    }

    FoliageGPUCuller::PreparedLayer FoliageGPUCuller::PrepareLayer(const FoliageInstanceRegistry& registry,
                                                                   u32 layerIndex, u32 instanceCount,
                                                                   const FoliageBoundsProfile& profile)
    {
        PreparedLayer out;
        out.InstanceCount = instanceCount;
        out.Generation = registry.GetGeneration();
        if (instanceCount == 0)
            return out;
        // Layer-local group indices. The registry's groups are a flat list over
        // every layer, and the shader indexes the tail densely, so the two need
        // a translation rather than a filter.
        const auto& groups = registry.GetGroups();
        TArray<u32> globalToLocal(groups.Num(), kUnmappedRow);
        TArray<u32> localToGlobal;
        localToGlobal.Reserve(groups.Num());
        for (u32 g = 0; g < static_cast<u32>(groups.Num()); ++g)
        {
            if (groups[g].m_LayerIndex != layerIndex)
                continue;
            globalToLocal[g] = static_cast<u32>(localToGlobal.Num());
            localToGlobal.Add(g);
        }

        const auto groupCount = static_cast<u32>(localToGlobal.Num());
        if (groupCount == 0)
        {
            return out;
        }

        TArray<u32> rowGroup(instanceCount, kUnmappedRow);
        for (const auto& record : registry.GetRecords())
        {
            if (record.m_LayerIndex != layerIndex || record.m_BufferIndex >= instanceCount)
                continue;
            if (record.m_GroupIndex >= globalToLocal.Num())
                continue;
            rowGroup[record.m_BufferIndex] = globalToLocal[record.m_GroupIndex];
        }

        // A row the registry does not account for would be rejected by the
        // shader's `group >= s_GroupCount` guard, i.e. a plant that silently
        // stops drawing. Refusing the whole layer keeps it on the uncompacted
        // path — every plant still drawn — and says why, which is the only
        // honest response to "the canonical records and the buffer disagree".
        const auto unmapped = static_cast<u32>(std::count(rowGroup.GetData(), rowGroup.GetData() + rowGroup.Num(), kUnmappedRow));
        if (unmapped != 0)
        {
            OLO_CORE_ERROR("FoliageGPUCuller: layer {} has {} of {} instance rows with no canonical record — GPU "
                           "culling disabled for this layer (it would drop those plants silently). Logged once "
                           "per regeneration.",
                           layerIndex, unmapped, instanceCount);
            return out;
        }

        // Tail: 8 uints of group AABB per group (min.xyz + pad, max.xyz + pad),
        // then one group index per row.
        const u32 boundsWords = groupCount * 8u;
        const u32 tailWords = boundsWords + instanceCount;
        const u32 sizeBytes = static_cast<u32>(sizeof(FoliageCullLayerHeader)) + tailWords * 4u;

        auto& scratch = out.Words;
        scratch.Init(0u, sizeBytes / 4u);

        FoliageCullLayerHeader header{};
        header.GroupCount = groupCount;
        header.InstanceCount = instanceCount;
        header.RowGroupOffset = boundsWords;
        header.HalfExtentXZ = profile.m_HalfExtentXZ;
        header.HalfExtentXZHeightScaled = profile.m_HalfExtentXZHeightScaled;
        header.MinY = profile.m_MinY;
        header.MaxY = profile.m_MaxY;
        header.WindDisplacement = profile.m_WindDisplacement;
        header.InteractionDisplacement = profile.m_InteractionDisplacement;
        std::memcpy(scratch.GetData(), &header, sizeof(header));

        const u32 tailBase = static_cast<u32>(sizeof(FoliageCullLayerHeader)) / 4u;
        glm::vec3 unionMin(std::numeric_limits<f32>::max());
        glm::vec3 unionMax(std::numeric_limits<f32>::lowest());
        for (u32 local = 0; local < groupCount; ++local)
        {
            // TERRAIN-LOCAL bounds, matching the space the instance rows are in.
            // The registry's world bounds are the same box through the terrain
            // transform; using them here would need the inverse transform on
            // every test for no gain.
            const BoundingBox& box = groups[localToGlobal[local]].m_LocalBounds;
            const u32 base = tailBase + local * 8u;
            scratch[base + 0] = std::bit_cast<u32>(box.Min.x);
            scratch[base + 1] = std::bit_cast<u32>(box.Min.y);
            scratch[base + 2] = std::bit_cast<u32>(box.Min.z);
            scratch[base + 4] = std::bit_cast<u32>(box.Max.x);
            scratch[base + 5] = std::bit_cast<u32>(box.Max.y);
            scratch[base + 6] = std::bit_cast<u32>(box.Max.z);
            unionMin = glm::min(unionMin, box.Min);
            unionMax = glm::max(unionMax, box.Max);
        }
        std::memcpy(scratch.GetData() + tailBase + boundsWords, rowGroup.GetData(), instanceCount * sizeof(u32));

        out.LocalBounds = BoundingBox(unionMin, unionMax);
        out.GroupCount = groupCount;
        return out;
    }

    bool FoliageGPUCuller::BeginLayerUpload(LayerResources& out, const PreparedLayer& prepared)
    {
        out = {};
        out.InstanceCount = prepared.InstanceCount;
        out.BuiltGeneration = prepared.Generation;
        out.RefusedForBuiltGeneration = prepared.Words.IsEmpty();
        if (prepared.Words.IsEmpty())
            return false;
        const u64 bytes = static_cast<u64>(prepared.Words.Num()) * sizeof(u32);
        if (bytes > std::numeric_limits<u32>::max())
            return false;
        out.LayerBuffer = StorageBuffer::Create(static_cast<u32>(bytes), kLayerBinding, StorageBufferUsage::DynamicDraw);
        if (!out.LayerBuffer || !out.LayerBuffer->GetRHIHandle().IsValid())
            return false;
        out.GroupCount = prepared.GroupCount;
        out.LocalBounds = prepared.LocalBounds;
        return true;
    }

    FoliageGPUCuller::SurvivorEstimate FoliageGPUCuller::EstimateSurvivors(const FoliageInstanceRegistry& registry,
                                                                           u32 layerIndex, const ViewInputs& inputs,
                                                                           f32 meshReach)
    {
        OLO_PROFILE_FUNCTION();

        // The same three tests FoliageGroupCull.comp applies to the same
        // terrain-local boxes, so a group counted here is a group the GPU keeps
        // (and the mesh test reads the box, which contains every member's
        // pivot, where the kernel reads the pivot: never smaller).
        SurvivorEstimate out;
        for (const auto& group : registry.GetGroups())
        {
            if (group.m_LayerIndex != layerIndex)
                continue;
            const BoundingBox& box = group.m_LocalBounds;
            if (!inputs.ViewFrustum.IsBoundingBoxVisible(box))
                continue;
            const glm::vec3 closest = glm::clamp(inputs.DistanceOrigin, box.Min, box.Max);
            const f32 distance = glm::length(closest - inputs.DistanceOrigin);
            if (distance > inputs.MaxDistance)
                continue;
            const auto members = static_cast<u32>(group.m_Instances.Num());
            out.List += members;
            if (meshReach > 0.0f && distance <= meshReach)
                out.Mesh += members;
        }
        return out;
    }

    bool FoliageGPUCuller::EnsureViewCapacity(ViewResources& view, u32 listCapacity, u32 meshCapacity,
                                              u32 groupCount) const
    {
        OLO_PROFILE_FUNCTION();

        bool compactedRecreated = false;

        const u32 wantedList = std::max(std::max(view.ListCapacity, listCapacity), 1u);
        const u32 wantedMesh = std::max(view.MeshCapacity, meshCapacity);
        if (!view.Compacted || wantedList != view.ListCapacity || wantedMesh != view.MeshCapacity)
        {
            // No slack added here: the caller's capacities already carry
            // StreamCapacityFor's headroom, and a view that outgrows them
            // overflows (bounded, counted) and comes back through here with
            // what it asked for (#1581).
            const u64 total = static_cast<u64>(wantedList) + wantedMesh;
            const u64 bytes64 = total * sizeof(FoliageInstanceData);
            if (bytes64 > std::numeric_limits<u32>::max())
            {
                OLO_CORE_ERROR("FoliageGPUCuller: a {} + {} instance compacted stream does not fit a 32-bit buffer size",
                               wantedList, wantedMesh);
                return false;
            }
            const auto bytes = static_cast<u32>(bytes64);
            view.Compacted = VertexBuffer::Create(bytes);
            if (!view.Compacted)
            {
                OLO_CORE_ERROR("FoliageGPUCuller: could not allocate a {} byte compacted instance buffer", bytes);
                view.Capacity = 0;
                view.ListCapacity = 0;
                view.MeshCapacity = 0;
                return false;
            }
            // The SAME layout FoliageRenderer gives its source instance VBO —
            // the compacted buffer takes its place as VAO stream 1, so a
            // mismatch here would feed the vertex stage a different struct.
            view.Compacted->SetLayout({
                { ShaderDataType::Float4, "a_PositionScale" },
                { ShaderDataType::Float4, "a_RotationHeight" },
                { ShaderDataType::Float4, "a_ColorAlpha" },
            });
            view.Capacity = static_cast<u32>(total);
            view.ListCapacity = wantedList;
            view.MeshCapacity = wantedMesh;
            compactedRecreated = true;
        }

        const u32 stateBytes =
            static_cast<u32>(sizeof(FoliageCullStateHeader)) + (groupCount + view.Capacity) * 4u;
        if (!view.State || view.State->GetSize() != stateBytes || compactedRecreated)
        {
            view.State = StorageBuffer::Create(stateBytes, kStateBinding, StorageBufferUsage::DynamicCopy);
            // Zeroed once, here, because nothing else ever seeds the demand
            // pair: Cull() uploads the header only up to it. A recreated buffer
            // is a new capacity, so its demand starts over against it.
            if (view.State)
                view.State->ClearData();
        }
        // Set unconditionally, not inside the branch above: two different
        // (groupCount, capacity) pairs can land on the same byte size, and a
        // GroupCapacity left describing the previous pair is exactly the kind of
        // stale bound the Cull() precondition is there to catch.
        view.GroupCapacity = groupCount;

        if (!view.DrawArgs)
        {
            view.DrawArgs = StorageBuffer::Create(kMaxParts * kDrawArgsStride,
                                                  ShaderBindingLayout::SSBO_INSTANCE_DRAW_INDIRECT,
                                                  StorageBufferUsage::DynamicCopy);
        }

        return compactedRecreated;
    }

    bool FoliageGPUCuller::Cull(const LayerResources& layer, ViewResources& view, RHI::ResourceHandle sourceInstances,
                                std::span<const Part> parts, const ViewInputs& inputs, bool emitStats,
                                const LodInputs& lod)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Initialised || !layer.IsValid() || !view.IsValid() || parts.empty() || !sourceInstances.IsValid())
        {
            return false;
        }
        // The stream no longer has to hold the whole layer (#1581): a list
        // smaller than this view's survivors overflows the bounded, counted
        // way and the caller grows it from the demand pair.
        if (parts.size() > kMaxParts || view.ListCapacity == 0 || view.Capacity < view.ListCapacity + view.MeshCapacity ||
            view.GroupCapacity < layer.GroupCount)
        {
            return false;
        }

        const auto partCount = static_cast<u32>(parts.size());

        // THE MESH REGION (#1533): a layer's mesh parts draw only the survivors
        // the mesh band can reach, from a second region of the compacted stream
        // that starts right after the all-survivor list (ListCapacity), through
        // the command's BaseInstance. It needs that BaseInstance honoured, room
        // for the region, and no debug capacity (that knob exercises the
        // all-survivor list's overflow). Without it every part draws every
        // survivor, as before.
        bool anyMeshPart = false;
        for (const Part& part : parts)
            anyMeshPart = anyMeshPart || part.MeshRegion;
        const bool meshRegion = anyMeshPart && lod.MeshReach > 0.0f && m_DebugOutputCapacity == 0 &&
                                view.MeshCapacity > 0 && RenderCommand::SupportsIndirectFirstInstance();
        const u32 meshRegionBase = meshRegion ? view.ListCapacity : 0u;

        // A layer wholly outside this view rejects here, before either kernel is
        // dispatched. Two dispatches are ~22 us of launch cost on this box and
        // they are paid per layer PER VIEW, so with five views and five layers a
        // camera facing away from an island was paying for fifty of them to
        // compute zero survivors.
        //
        // It must still SEED the args and report success, not `return false`:
        // false means "no compacted set", which sends the draw down the
        // uncompacted path and renders every instance of the layer that was just
        // proven invisible. Seeding instanceCount = 0 makes the indirect draw
        // draw nothing, which is the whole point. The bound is the union of the
        // group bounds, each of which already carries the deformation padding,
        // so it can only reject a layer the per-instance test would reject
        // entirely.
        const bool layerVisible = inputs.ViewFrustum.IsBoundingBoxVisible(layer.LocalBounds);

        // Write-after-READ. This slot's buffers may still be the source of a
        // draw recorded earlier in the frame -- the shadow regions run one after
        // another and reuse the slots, so the atlas region's cull writes over
        // what the CSM region's draws read. GL and Vulkan both allow a dispatch
        // to overlap an earlier draw, so the ordering has to be asked for.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::VertexAttribArray |
                                     MemoryBarrierFlags::Command);

        // ── 1. Seed the indirect args. Count / FirstIndex are CPU knowledge;
        // instanceCount starts at zero and the kernel counts into it. ──
        std::array<FoliageCullDrawArgs, kMaxParts> args{};
        for (u32 i = 0; i < partCount; ++i)
        {
            const bool drawsRegion = meshRegion && parts[i].MeshRegion;
            args[i].Count = parts[i].IndexCount;
            args[i].InstanceCount = 0;
            args[i].FirstIndex = parts[i].BaseIndex;
            args[i].BaseInstance = drawsRegion ? meshRegionBase : 0u;
            args[i].MeshRegion = drawsRegion ? 1u : 0u;
        }
        view.DrawArgs->SetData(args.data(), partCount * kDrawArgsStride, 0);

        if (!layerVisible)
        {
            view.PartCount = partCount;
            return true;
        }

        // ── 2. Seed the state header. Only the header: the tail's group bits
        // are fully rewritten by the group kernel every dispatch, and the
        // survivor -> source row map is only read for slots the kernel wrote. ──
        // The ALL-SURVIVOR LIST's bound: the mesh region past it has its own.
        const u32 capacity = (m_DebugOutputCapacity != 0)
                                 ? std::min(m_DebugOutputCapacity, view.ListCapacity)
                                 : view.ListCapacity;

        FoliageCullStateHeader state{};
        state.Planes[0] = PlaneToVec4(inputs.ViewFrustum.GetPlane(Frustum::Planes::Left));
        state.Planes[1] = PlaneToVec4(inputs.ViewFrustum.GetPlane(Frustum::Planes::Right));
        state.Planes[2] = PlaneToVec4(inputs.ViewFrustum.GetPlane(Frustum::Planes::Bottom));
        state.Planes[3] = PlaneToVec4(inputs.ViewFrustum.GetPlane(Frustum::Planes::Top));
        state.Planes[4] = PlaneToVec4(inputs.ViewFrustum.GetPlane(Frustum::Planes::Near));
        state.Planes[5] = PlaneToVec4(inputs.ViewFrustum.GetPlane(Frustum::Planes::Far));
        state.DistanceOrigin = glm::vec4(inputs.DistanceOrigin, inputs.MaxDistance);
        // The instance kernel's density drop reads these (issue #1237), and it
        // drops a row exactly where the vertex stages would have drawn it at
        // alpha zero — the same shared include evaluates both sides.
        state.LodTransition0 = lod.Transition0;
        state.LodTransition1 = lod.Transition1;
        state.InstanceCount = layer.InstanceCount;
        state.GroupCount = layer.GroupCount;
        state.OutputCapacity = capacity;
        state.PartCount = partCount;
        state.SourceRowOffset = layer.GroupCount;
        state.EmitStats = emitStats ? 1u : 0u;
        state.MeshRegionBase = meshRegionBase;
        state.MeshReach = meshRegion ? lod.MeshReach : 0.0f;
        state.MeshCursor = 0;
        state.MeshCapacity = meshRegion ? view.MeshCapacity : 0u;
        // A clip level is culled against the VSM's dirty pages as well (#1580)
        // when the variant and the pyramid are both there.
        const bool vsmLevel = inputs.VsmClipLevel >= 0 && inputs.VsmHPB.IsValid() && m_GroupCullVsmShader;
        if (inputs.VsmClipLevel >= 0 && !vsmLevel && !m_WarnedVsmGroupCullUnavailable)
        {
            OLO_CORE_WARN("FoliageGPUCuller: clip level {} is culled without the dirty-page test (pyramid {}, variant "
                          "{}) -- its cached foliage pages are redrawn. Further levels not logged.",
                          inputs.VsmClipLevel, inputs.VsmHPB.IsValid(), m_GroupCullVsmShader != nullptr);
            m_WarnedVsmGroupCullUnavailable = true;
        }
        if (vsmLevel)
        {
            state.VsmLocalToClip = inputs.VsmLocalToClip;
            state.VsmLevel = glm::ivec4(inputs.VsmClipLevel + 1, inputs.VsmPageOffset.x, inputs.VsmPageOffset.y, 0);
        }
        // Up to the demand pair, never over it -- see kFoliageCullSeededHeaderBytes.
        view.State->SetData(&state, kFoliageCullSeededHeaderBytes, 0);
        if (vsmLevel)
        {
            view.State->SetData(reinterpret_cast<const u8*>(&state) + kFoliageCullVsmHeaderOffset,
                                static_cast<u32>(sizeof(state)) - kFoliageCullVsmHeaderOffset, kFoliageCullVsmHeaderOffset);
        }

        // ── 3. Bind and dispatch ──
        layer.LayerBuffer->Bind(); // 18
        view.State->Bind();        // 19
        view.DrawArgs->Bind();     // 17
        RenderCommand::BindStorageBuffer(ShaderBindingLayout::SSBO_INSTANCE_CULL_INPUT, sourceInstances);
        RenderCommand::BindStorageBuffer(ShaderBindingLayout::SSBO_INSTANCE_DATA, view.Compacted->GetRHIHandle());

        if (vsmLevel)
        {
            RenderCommand::BindStorageBuffer(ShaderBindingLayout::SSBO_VSM_HPB, inputs.VsmHPB);
            m_GroupCullVsmShader->Bind();
        }
        else
        {
            m_GroupCullShader->Bind();
        }
        RenderCommand::DispatchCompute(DispatchGroups(layer.GroupCount, kGroupWorkgroupSize), 1, 1);

        // The instance kernel READS the group bits this dispatch wrote. Without
        // this barrier it reads whatever the previous frame left there, which is
        // the plausible-but-wrong result this whole class is written to avoid.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);

        m_InstanceCullShader->Bind();
        RenderCommand::DispatchCompute(DispatchGroups(layer.InstanceCount, kInstanceWorkgroupSize), 1, 1);

        // Three consumers downstream, three barrier classes: the draw reads the
        // compacted buffer as a VERTEX stream, reads the args as an INDIRECT
        // command, and a later cull into the same scratch slot must not start
        // before this one's draw has read it.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::Command |
                                     MemoryBarrierFlags::VertexAttribArray);

        view.PartCount = partCount;
        return true;
    }

    bool FoliageGPUCuller::ReadStaged(RHI::ResourceHandle source, u32 offset, u32 size, void* dest) const
    {
        if (size == 0u)
            return true;
        // Through the staging copy, never off the live buffer: a CPU read of a
        // buffer the cull writes with atomics and the draws read as a vertex
        // stream migrates it out of video memory for good. #1391 measured it:
        // one direct readback left the main view's cull 0.09 -> 1.9 ms and its
        // forward draw 3 -> 21 ms for the rest of the process. See
        // StagedBufferReadback and docs/agent-rules/gpu-readback-stats-channel.md.
        m_Readback.Stage(source, offset, size, MemoryBarrierFlags::ShaderStorage);
        return m_Readback.Read(dest, size);
    }

    bool FoliageGPUCuller::ReadbackResult(const LayerResources& layer, const ViewResources& view,
                                          Readback& out) const
    {
        OLO_PROFILE_FUNCTION();

        out = {};
        if (!layer.IsValid() || !view.IsValid() || view.PartCount == 0)
        {
            return false;
        }

        // The staging buffer grows to the whole compacted stream; a census is
        // occasional, so it is handed back after every read rather than held
        // as host-visible memory the memory report does not count.
        struct ReleaseStaging
        {
            StagedBufferReadback& Staging;
            ~ReleaseStaging()
            {
                Staging.Release();
            }
        } releaseStaging{ m_Readback };

        // Everything the kernels wrote has to have landed before the read. This
        // is the one place a full barrier is the right instrument rather than a
        // targeted one: the call is a deliberate stall already, and getting the
        // class wrong here would read a number that is right most of the time,
        // which is the worst possible output for a diagnostic.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::BufferUpdate);

        FoliageCullStateHeader header{};
        if (!ReadStaged(view.State->GetRHIHandle(), 0, static_cast<u32>(sizeof(header)), &header))
            return false;

        // The all-survivor list's count is on any part that does NOT draw the
        // mesh region (#1533); a mesh part's count is the region's.
        std::array<FoliageCullDrawArgs, kMaxParts> partArgs{};
        const u32 readParts = std::min(view.PartCount, kMaxParts);
        if (!ReadStaged(view.DrawArgs->GetRHIHandle(), 0, readParts * kDrawArgsStride, partArgs.data()))
            return false;
        FoliageCullDrawArgs args = partArgs[0];
        for (u32 i = 0; i < readParts; ++i)
        {
            if (partArgs[i].MeshRegion == 0u)
            {
                args = partArgs[i];
                break;
            }
        }

        out.GroupsVisible = header.GroupsVisible;
        out.Visible = header.VisibleCount;
        out.Reserved = header.ReserveCursor;
        out.Submitted = args.InstanceCount;

        if (out.Submitted > view.Capacity)
        {
            OLO_CORE_ERROR("FoliageGPUCuller: the indirect command claims {} instances but the compacted buffer "
                           "holds {} — refusing to read past it",
                           out.Submitted, view.Capacity);
            return false;
        }

        if (out.Submitted == 0)
        {
            return true;
        }

        out.SourceRows.SetNum(out.Submitted, EAllowShrinking::No);
        const u32 rowByteOffset =
            static_cast<u32>(sizeof(FoliageCullStateHeader)) + header.SourceRowOffset * 4u;
        if (!ReadStaged(view.State->GetRHIHandle(), rowByteOffset, out.Submitted * 4u, out.SourceRows.GetData()))
            return false;

        out.Compacted.SetNum(out.Submitted, EAllowShrinking::No);
        return ReadStaged(view.Compacted->GetRHIHandle(), 0,
                          out.Submitted * static_cast<u32>(sizeof(FoliageInstanceData)), out.Compacted.GetData());
    }

    // ── FoliageCullDemandReadback (#1581) ────────────────────────────────────

    FoliageCullDemandReadback::~FoliageCullDemandReadback()
    {
        Release();
    }

    void FoliageCullDemandReadback::Release()
    {
        for (auto& slot : m_Slots)
        {
            if (slot.Fence != 0)
                RenderCommand::DestroyFence(slot.Fence);
            if (slot.Buffer.IsValid())
                RenderCommand::DeleteBuffer(slot.Buffer);
            slot = {};
        }
        m_Next = 0;
    }

    void FoliageCullDemandReadback::DiscardInFlight()
    {
        for (auto& slot : m_Slots)
        {
            if (slot.Pending)
                slot.Discarded = true;
        }
    }

    u32 FoliageCullDemandReadback::GetSlotsInFlight() const
    {
        return static_cast<u32>(std::ranges::count_if(m_Slots, [](const Slot& slot)
                                                      { return slot.Pending; }));
    }

    bool FoliageCullDemandReadback::Capture(std::span<const RHI::ResourceHandle> states)
    {
        OLO_PROFILE_FUNCTION();

        if (states.empty() || std::ranges::none_of(states, [](const RHI::ResourceHandle& state)
                                                   { return state.IsValid(); }))
            return false;

        Slot& slot = m_Slots[m_Next];
        // Every slot still in flight: skip rather than wait. The demand pairs
        // are running maxima that stay in the State buffers, so the next
        // capture still carries whatever this one would have.
        if (slot.Pending)
            return false;

        const u64 bytes = static_cast<u64>(states.size()) * sizeof(FoliageCullDemand);
        if (!slot.Buffer.IsValid() || slot.Bytes < bytes)
        {
            if (slot.Buffer.IsValid())
                RenderCommand::DeleteBuffer(slot.Buffer);
            slot.Buffer = RenderCommand::CreateBufferHandle();
            if (!slot.Buffer.IsValid())
            {
                slot.Bytes = 0;
                return false;
            }
            RenderCommand::AllocateBufferStorage(slot.Buffer, bytes, RHI::MemoryResidency::DeviceToHost);
            slot.Bytes = bytes;
        }

        // The pairs were written by the cull kernels' atomics (ShaderStorage),
        // and a buffer copy is a BufferUpdate client: both, or the copy reads a
        // value that is right most of the time.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::BufferUpdate);
        slot.Valid.SetNumZeroed(static_cast<i32>(states.size()));
        for (sizet i = 0; i < states.size(); ++i)
        {
            if (!states[i].IsValid())
                continue;
            RenderCommand::CopyBufferSubData(states[i], slot.Buffer, kFoliageCullSeededHeaderBytes,
                                             i * sizeof(FoliageCullDemand), sizeof(FoliageCullDemand));
            slot.Valid[static_cast<i32>(i)] = 1u;
        }

        slot.Fence = RenderCommand::CreateFence();
        if (slot.Fence == 0)
        {
            OLO_CORE_WARN("FoliageCullDemandReadback: fence creation failed; this capture is dropped (the demand "
                          "stays in the State buffers for the next one).");
            return false;
        }
        slot.Pending = true;
        slot.Discarded = false;
        m_Next = (m_Next + 1u) % kRingSlots;
        return true;
    }

    bool FoliageCullDemandReadback::Poll(TArray<FoliageCullDemand>& out)
    {
        OLO_PROFILE_FUNCTION();

        out.Reset();
        bool retired = false;
        TArray<FoliageCullDemand> pairs;
        for (auto& slot : m_Slots)
        {
            if (!slot.Pending || !RenderCommand::IsFenceSignaled(slot.Fence))
                continue;

            RenderCommand::DestroyFence(slot.Fence);
            slot.Fence = 0;
            slot.Pending = false;
            if (slot.Discarded)
            {
                slot.Discarded = false;
                continue;
            }
            const auto entries = static_cast<i32>(slot.Valid.Num());
            pairs.SetNumZeroed(entries);
            RenderCommand::ReadBufferSubData(slot.Buffer, 0, static_cast<u64>(entries) * sizeof(FoliageCullDemand),
                                             pairs.GetData());

            if (out.Num() < entries)
                out.SetNumZeroed(entries);
            for (i32 i = 0; i < entries; ++i)
            {
                if (slot.Valid[i] == 0u)
                    continue;
                out[i].List = std::max(out[i].List, pairs[i].List);
                out[i].Mesh = std::max(out[i].Mesh, pairs[i].Mesh);
            }
            retired = true;
        }
        return retired;
    }
} // namespace OloEngine
