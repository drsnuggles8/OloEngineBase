#include "OloEnginePCH.h"
#include "OloEngine/Terrain/Foliage/FoliageGPUCuller.h"

#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderCommand.h"
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
            return refuse();
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
            return refuse();
        }

        // Tail: 8 uints of group AABB per group (min.xyz + pad, max.xyz + pad),
        // then one group index per row.
        const u32 boundsWords = groupCount * 8u;
        const u32 tailWords = boundsWords + instanceCount;
        const u32 sizeBytes = static_cast<u32>(sizeof(FoliageCullLayerHeader)) + tailWords * 4u;

        TArray<u32> scratch(sizeBytes / 4u, 0u);

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

        if (!out.LayerBuffer || out.LayerBuffer->GetSize() != sizeBytes)
        {
            out.LayerBuffer = StorageBuffer::Create(sizeBytes, kLayerBinding, StorageBufferUsage::DynamicDraw);
        }
        if (!out.LayerBuffer)
        {
            OLO_CORE_ERROR("FoliageGPUCuller: could not allocate the {} byte layer buffer for layer {}", sizeBytes,
                           layerIndex);
            return refuse();
        }

        out.LayerBuffer->SetData(scratch.GetData(), sizeBytes, 0);
        out.LocalBounds = BoundingBox(unionMin, unionMax);
        out.GroupCount = groupCount;
        out.InstanceCount = instanceCount;
        out.BuiltGeneration = generation;
        return true;
    }

    bool FoliageGPUCuller::EnsureViewCapacity(ViewResources& view, u32 capacity, u32 groupCount) const
    {
        OLO_PROFILE_FUNCTION();

        bool compactedRecreated = false;

        if (!view.Compacted || view.Capacity < capacity)
        {
            // Grown with slack for the same reason FoliageRenderer's instance
            // VBO is: a sculpt that adds a handful of plants must not reallocate
            // and rebuild every vertex array that streams this buffer.
            const u32 newCapacity = std::max(capacity * 2u, 256u);
            const u32 bytes = newCapacity * static_cast<u32>(sizeof(FoliageInstanceData));
            view.Compacted = VertexBuffer::Create(bytes);
            if (!view.Compacted)
            {
                OLO_CORE_ERROR("FoliageGPUCuller: could not allocate a {} byte compacted instance buffer", bytes);
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
            view.Capacity = newCapacity;
            compactedRecreated = true;
        }

        const u32 stateBytes =
            static_cast<u32>(sizeof(FoliageCullStateHeader)) + (groupCount + view.Capacity) * 4u;
        if (!view.State || view.State->GetSize() != stateBytes)
        {
            view.State = StorageBuffer::Create(stateBytes, kStateBinding, StorageBufferUsage::DynamicCopy);
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
        if (parts.size() > kMaxParts || view.Capacity < layer.InstanceCount ||
            view.GroupCapacity < layer.GroupCount)
        {
            return false;
        }

        const auto partCount = static_cast<u32>(parts.size());

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
            args[i].Count = parts[i].IndexCount;
            args[i].InstanceCount = 0;
            args[i].FirstIndex = parts[i].BaseIndex;
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
        const u32 capacity = (m_DebugOutputCapacity != 0)
                                 ? std::min(m_DebugOutputCapacity, view.Capacity)
                                 : view.Capacity;

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
        view.State->SetData(&state, static_cast<u32>(sizeof(state)), 0);

        // ── 3. Bind and dispatch ──
        layer.LayerBuffer->Bind(); // 18
        view.State->Bind();        // 19
        view.DrawArgs->Bind();     // 17
        RenderCommand::BindStorageBuffer(ShaderBindingLayout::SSBO_INSTANCE_CULL_INPUT, sourceInstances);
        RenderCommand::BindStorageBuffer(ShaderBindingLayout::SSBO_INSTANCE_DATA, view.Compacted->GetRHIHandle());

        m_GroupCullShader->Bind();
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

    bool FoliageGPUCuller::ReadbackResult(const LayerResources& layer, const ViewResources& view,
                                          Readback& out) const
    {
        OLO_PROFILE_FUNCTION();

        out = {};
        if (!layer.IsValid() || !view.IsValid() || view.PartCount == 0)
        {
            return false;
        }

        // Everything the kernels wrote has to have landed before the read. This
        // is the one place a full barrier is the right instrument rather than a
        // targeted one: the call is a deliberate stall already, and getting the
        // class wrong here would read a number that is right most of the time,
        // which is the worst possible output for a diagnostic.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::BufferUpdate);

        FoliageCullStateHeader header{};
        view.State->GetData(&header, static_cast<u32>(sizeof(header)), 0);

        FoliageCullDrawArgs args{};
        view.DrawArgs->GetData(&args, static_cast<u32>(sizeof(args)), 0);

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
        view.State->GetData(out.SourceRows.GetData(), out.Submitted * 4u, rowByteOffset);

        out.Compacted.SetNum(out.Submitted, EAllowShrinking::No);
        RenderCommand::ReadBufferSubData(view.Compacted->GetRHIHandle(), 0,
                                         out.Submitted * static_cast<u32>(sizeof(FoliageInstanceData)),
                                         out.Compacted.GetData());
        return true;
    }
} // namespace OloEngine
