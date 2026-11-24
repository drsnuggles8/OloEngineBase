#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Crowd/CrowdCull.h"

#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Renderer/Debug/RendererMemoryReport.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <unordered_map>

namespace OloEngine
{
    namespace
    {
        // The compute binds everything it uses on each dispatch. The ordinary
        // instance cull owns 15/16/17 while it runs; 21..26 are private to
        // the fluid solver while its kernels run. Neither shares this shader.
        constexpr std::array<u32, 3> kOutputSlots{ ShaderBindingLayout::SSBO_INSTANCE_DATA, 21u, 22u };
        constexpr std::array<u32, 3> kArgSlots{ ShaderBindingLayout::SSBO_INSTANCE_DRAW_INDIRECT, 23u, 24u };
        constexpr u32 kPreviousHistorySlot = 25;
        constexpr u32 kNextHistorySlot = 26;
        constexpr u32 kMaxRecords = (128u * 1024u * 1024u) / sizeof(InstanceData);
        constexpr u32 kGroupSize = 128;

        struct DrawArgs
        {
            u32 Count, InstanceCount, FirstIndex, BaseVertex, BaseInstance;
        };
        static_assert(sizeof(DrawArgs) == 20);

        struct alignas(16) CullParams
        {
            std::array<glm::vec4, 6> Planes;
            glm::vec4 VisualSphere, AnimatedSphere, Origin, EyeScale, Lod, Clock;
            glm::uvec4 Layout;
            u32 PreviousCount;
            u32 CoarseIntervals = CrowdCoarseIntervals;
            u32 Remapped;
            u32 Pad{};
        };
        static_assert(offsetof(CullParams, Layout) == 192);
        static_assert(sizeof(CullParams) == 224);

        Ref<StorageBuffer> MakeStorage(u32 bytes)
        {
            // GPU-written buffers must retain their persistent addresses on
            // Vulkan. An upload snapshot of the palette tail or indirect seed
            // would hide the compute writes from the subsequent draw.
            return StorageBuffer::Create(bytes, StorageBuffer::kNoBinding, StorageBufferUsage::DynamicCopy);
        }
    } // namespace

    CrowdClip SampleCoarseCrowdClip(const CrowdClip& full)
    {
        const u32 stride = full.RecordsPerFrame();
        if (full.FrameCount < 2u || full.BoneCount == 0u || full.BoneCount > UBOStructures::AnimationConstants::MAX_BONES ||
            static_cast<u64>(full.FrameCount) * stride != static_cast<u64>(full.Atlas.Num()))
            return {};

        CrowdClip coarse;
        coarse.Duration = full.Duration;
        coarse.BoneCount = full.BoneCount;
        coarse.FrameCount = CrowdCoarseIntervals + 1u;
        coarse.Atlas.Reserve(coarse.FrameCount * stride);
        for (u32 i = 0; i < coarse.FrameCount; ++i)
        {
            const u64 position = static_cast<u64>(full.FrameCount - 1u) * i;
            const u32 sourceFrame = static_cast<u32>(position / CrowdCoarseIntervals);
            const InstanceData* first = full.Atlas.GetData() + sourceFrame * stride;
            coarse.Atlas.Append(first, stride);
            const u32 fraction = static_cast<u32>(position % CrowdCoarseIntervals);
            if (fraction == 0u)
                continue; // Exact sample, including the endpoint: no next frame required.
            const f32 weight = static_cast<f32>(fraction) / CrowdCoarseIntervals;
            for (u32 record = 0; record < stride; ++record)
            {
                // The three matrix lanes hold successive bones. Use the same
                // elementwise interpolation as OloCrowdBone in the vertex shader.
                auto& sample = coarse.Atlas[i * stride + record];
                const auto& next = first[stride + record];
                sample.Transform += (next.Transform - sample.Transform) * weight;
                sample.Normal += (next.Normal - sample.Normal) * weight;
                sample.PrevTransform += (next.PrevTransform - sample.PrevTransform) * weight;
            }
        }
        return coarse;
    }

    bool CrowdCull::PrepareBuffers(u32 count, const CrowdClip& clip)
    {
        if (m_AtlasData == clip.Atlas.GetData() && count <= m_Capacity)
            return true;

        const RendererMemoryOwnerScope allocationOwner("Crowd cull buffers", MemoryLifetime::Pooled);
        const u32 atlasRecords = static_cast<u32>(clip.Atlas.Num());
        const CrowdClip coarse = SampleCoarseCrowdClip(clip);
        const u32 coarseRecords = static_cast<u32>(coarse.Atlas.Num());
        const u32 capacity = std::clamp(m_Capacity * 2u, count, kMaxRecords - atlasRecords - coarseRecords);
        const u32 inputBytes = capacity * static_cast<u32>(sizeof(InstanceData));
        const u32 atlasBytes = atlasRecords * static_cast<u32>(sizeof(InstanceData));
        const u32 coarseBytes = coarseRecords * static_cast<u32>(sizeof(InstanceData));
        auto input = MakeStorage(inputBytes);
        const u32 historyBytes = capacity * static_cast<u32>(sizeof(glm::uvec4));
        auto previousHistory = MakeStorage(historyBytes);
        auto nextHistory = MakeStorage(historyBytes);
        if (!input || !previousHistory || !nextHistory)
            return false;
        std::array<Ref<StorageBuffer>, 3> outputs;
        for (auto& out : outputs)
        {
            out = MakeStorage(inputBytes + atlasBytes + coarseBytes);
            if (!out || !RenderCommand::TryUploadBufferSubData(out->GetRHIHandle(), inputBytes, atlasBytes, clip.Atlas.GetData()) ||
                !RenderCommand::TryUploadBufferSubData(out->GetRHIHandle(), inputBytes + atlasBytes, coarseBytes, coarse.Atlas.GetData()))
                return false;
        }
        for (auto& args : m_Indirect)
        {
            if (!args)
                args = MakeStorage(sizeof(DrawArgs));
            if (!args)
                return false;
        }
        if (m_AtlasData == clip.Atlas.GetData() && !m_PreviousIDs.IsEmpty())
        {
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::BufferUpdate);
            RenderCommand::CopyBufferSubData(m_PreviousHistory->GetRHIHandle(), previousHistory->GetRHIHandle(), 0u, 0u,
                                             m_PreviousIDs.Num() * sizeof(glm::uvec4));
        }
        else
            m_PreviousIDs.Reset();
        m_AtlasData = clip.Atlas.GetData();
        m_Capacity = capacity;
        m_Input = std::move(input);
        m_PreviousHistory = std::move(previousHistory);
        m_NextHistory = std::move(nextHistory);
        m_Output = std::move(outputs);
        return true;
    }

    bool CrowdCull::Dispatch(std::span<const InstanceData> instances, const CrowdClip& clip,
                             f32 now, f32 previous, bool loop, bool resetHistory,
                             const CrowdAnimationComponent& settings, glm::vec4 localSphere, glm::vec4 cullSphere,
                             const std::array<CrowdDrawRange, 3>& ranges)
    {
        const u64 atlasRecords = static_cast<u64>(clip.Atlas.Num());
        if (instances.empty() || clip.FrameCount < 2u || clip.BoneCount == 0u || clip.BoneCount > UBOStructures::AnimationConstants::MAX_BONES ||
            static_cast<u64>(clip.FrameCount) * clip.RecordsPerFrame() != atlasRecords ||
            !std::isfinite(now) || !std::isfinite(previous))
            return false;
        const u64 tailRecords = atlasRecords + (CrowdCoarseIntervals + 1u) * clip.RecordsPerFrame();
        if (tailRecords >= kMaxRecords || instances.size() > kMaxRecords - tailRecords)
            return false;

        if (!m_Shader)
            m_Shader = ComputeShader::Create("assets/shaders/compute/CrowdCull.comp");
        if (!m_Shader || !m_Shader->IsValid())
            return false;
        if (!m_Parameters)
            m_Parameters = UniformBuffer::Create(sizeof(CullParams), ShaderBindingLayout::UBO_INSTANCE_CULL, "CrowdCullParams");
        const u32 count = static_cast<u32>(instances.size());
        if (!m_Parameters || !PrepareBuffers(count, clip))
            return false;

        const u64 frame = Renderer3D::GetPhysicalFrameSerial();
        const u32 previousCount = !resetHistory && frame == m_Frame + 1 ? static_cast<u32>(m_PreviousIDs.Num()) : 0u;
        const bool sameOrder = static_cast<u32>(m_PreviousIDs.Num()) == count &&
                               std::equal(instances.begin(), instances.end(), m_PreviousIDs.GetData(),
                                          [](const InstanceData& member, u64 id)
                                          { return member.StableID == id; });
        const bool remap = previousCount != 0u && !sameOrder;
        TArray<InstanceData> remapped;
        if (remap)
        {
            // The common unchanged group uploads its authored records directly.
            // On a membership change, temporarily use CrowdPose.x for the old
            // row plus one. The compute replaces it with the draw's pose.
            std::unordered_map<u64, u32> previousRows;
            previousRows.reserve(previousCount);
            for (u32 row = 0; row < previousCount; ++row)
                previousRows.emplace(m_PreviousIDs[row], row + 1u);
            remapped.Append(instances.data(), count);
            for (auto& member : remapped)
            {
                const auto it = previousRows.find(member.StableID);
                member.CrowdPose.x = it != previousRows.end() ? it->second : 0u;
            }
        }
        if (!RenderCommand::TryUploadBufferSubData(m_Input->GetRHIHandle(), 0, instances.size_bytes(),
                                                   remap ? remapped.GetData() : instances.data()))
            return false;
        for (sizet tier = 0; tier < ranges.size(); ++tier)
        {
            const DrawArgs seed{ ranges[tier].Count, 0u, ranges[tier].FirstIndex, 0u, 0u };
            if (!RenderCommand::TryUploadBufferSubData(m_Indirect[tier]->GetRHIHandle(), 0, sizeof(seed), &seed))
                return false;
        }

        CullParams params{};
        // CPU clip convention (-w..w in z). Use the pre-backend cull matrix
        // so the same relative planes work on GL, Vulkan and a frozen camera.
        const Frustum frustum(Renderer3D::GetCullViewProjectionRelative());
        for (sizet i = 0; i < params.Planes.size(); ++i)
        {
            const auto& plane = frustum.GetPlane(static_cast<Frustum::Planes>(i));
            params.Planes[i] = glm::vec4(plane.Normal, plane.Distance);
        }
        const auto& projection = Renderer3D::GetCullProjectionMatrix();
        const auto& view = Renderer3D::GetLODViewParams();
        const f32 farPixels = std::max(1.0f, settings.m_FarPixels);
        params.VisualSphere = localSphere;
        params.AnimatedSphere = cullSphere;
        params.Origin = glm::vec4(Renderer3D::GetRenderOrigin(), Renderer3D::IsFrustumCullingEnabled() ? 1.0f : 0.0f);
        params.EyeScale = glm::vec4(Renderer3D::GetCullViewPositionRelative(), std::abs(projection[1][1]) * static_cast<f32>(view.ScreenHeight));
        params.Lod = { std::max(farPixels, settings.m_NearPixels), farPixels, std::clamp(settings.m_Hysteresis, 0.0f, 0.45f),
                       std::abs(projection[3][3]) > 0.5f ? 1.0f : 0.0f };
        params.Clock = { now, previous, loop ? 1.0f : 0.0f, m_PreviousLoop ? 1.0f : 0.0f };
        params.Layout = { count, m_Capacity, clip.FrameCount, clip.BoneCount };
        params.PreviousCount = previousCount;
        params.Remapped = remap ? 1u : 0u;
        m_Parameters->SetData(&params, sizeof(params));
        m_Parameters->Bind();
        RenderCommand::BindStorageBuffer(ShaderBindingLayout::SSBO_INSTANCE_CULL_INPUT, m_Input->GetRHIHandle());
        RenderCommand::BindStorageBuffer(kPreviousHistorySlot, m_PreviousHistory->GetRHIHandle());
        RenderCommand::BindStorageBuffer(kNextHistorySlot, m_NextHistory->GetRHIHandle());
        for (sizet tier = 0; tier < ranges.size(); ++tier)
        {
            RenderCommand::BindStorageBuffer(kOutputSlots[tier], m_Output[tier]->GetRHIHandle());
            RenderCommand::BindStorageBuffer(kArgSlots[tier], m_Indirect[tier]->GetRHIHandle());
        }
        CommandDispatch::InvalidateGPUSceneMaterialBinding(); // material table also uses slot 17
        m_Shader->Bind();
        const u64 recorded = m_Shader->GetRecordedDispatchCount();
        RenderCommand::DispatchCompute((count + kGroupSize - 1u) / kGroupSize, 1u, 1u);
        if (RendererAPI::GetAPI() == RendererAPI::API::Vulkan && m_Shader->GetRecordedDispatchCount() == recorded)
            return false;
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::Command | MemoryBarrierFlags::BufferUpdate);
        std::swap(m_PreviousHistory, m_NextHistory);
        if (!sameOrder)
        {
            m_PreviousIDs.Reset();
            m_PreviousIDs.Reserve(count);
            for (const auto& member : instances)
                m_PreviousIDs.Add(member.StableID);
        }
        m_Frame = frame;
        m_PreviousLoop = loop;
        return true;
    }
} // namespace OloEngine
