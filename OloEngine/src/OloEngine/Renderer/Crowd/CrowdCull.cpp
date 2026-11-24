#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Crowd/CrowdCull.h"

#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Renderer/Debug/RendererMemoryReport.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

namespace OloEngine
{
    namespace
    {
        // The compute binds everything it uses on each dispatch. The ordinary
        // instance cull owns 15/16/17 while it runs; 21..25 are private to
        // the fluid solver while its kernels run. Neither shares this shader.
        constexpr std::array<u32, 3> kOutputSlots{ ShaderBindingLayout::SSBO_INSTANCE_DATA, 21u, 22u };
        constexpr std::array<u32, 3> kArgSlots{ ShaderBindingLayout::SSBO_INSTANCE_DRAW_INDIRECT, 23u, 24u };
        constexpr u32 kHistorySlot = 25;
        constexpr u32 kMaxRecords = (128u * 1024u * 1024u) / sizeof(InstanceData);
        constexpr u32 kGroupSize = 128;
        constexpr u32 kCoarseIntervals = 8;

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
            u32 CoarseIntervals = kCoarseIntervals;
            std::array<u32, 2> Pad{};
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

    bool CrowdCull::PrepareBuffers(u32 count, const CrowdClip& clip)
    {
        if (m_AtlasData == clip.Atlas.GetData() && count <= m_Capacity)
            return true;

        const RendererMemoryOwnerScope allocationOwner("Crowd cull buffers", MemoryLifetime::Pooled);
        const u32 atlasRecords = static_cast<u32>(clip.Atlas.Num());
        const u32 stride = clip.RecordsPerFrame();
        const u32 coarseRecords = (kCoarseIntervals + 1u) * stride;
        const u32 capacity = std::clamp(m_Capacity * 2u, count, kMaxRecords - atlasRecords - coarseRecords);
        const u32 inputBytes = capacity * static_cast<u32>(sizeof(InstanceData));
        const u32 atlasBytes = atlasRecords * static_cast<u32>(sizeof(InstanceData));
        const u32 coarseBytes = coarseRecords * static_cast<u32>(sizeof(InstanceData));
        auto input = MakeStorage(inputBytes);
        auto history = MakeStorage(capacity * static_cast<u32>(sizeof(glm::uvec4)));
        if (!input || !history)
            return false;
        // Contiguous coarse frames let the vertex shader blend the next
        // sample with the same stride it already uses for the full clip.
        TArray<InstanceData> coarse;
        coarse.Reserve(coarseRecords);
        for (u32 i = 0; i <= kCoarseIntervals; ++i)
        {
            const u32 frame = (clip.FrameCount - 1u) * i / kCoarseIntervals;
            coarse.Append(clip.Atlas.GetData() + frame * stride, stride);
        }
        std::array<Ref<StorageBuffer>, 3> outputs;
        for (auto& out : outputs)
        {
            out = MakeStorage(inputBytes + atlasBytes + coarseBytes);
            if (!out || !RenderCommand::TryUploadBufferSubData(out->GetRHIHandle(), inputBytes, atlasBytes, clip.Atlas.GetData()) ||
                !RenderCommand::TryUploadBufferSubData(out->GetRHIHandle(), inputBytes + atlasBytes, coarseBytes, coarse.GetData()))
                return false;
        }
        for (auto& args : m_Indirect)
        {
            if (!args)
                args = MakeStorage(sizeof(DrawArgs));
            if (!args)
                return false;
        }
        if (m_AtlasData == clip.Atlas.GetData() && m_PreviousCount > 0u)
        {
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::BufferUpdate);
            RenderCommand::CopyBufferSubData(m_History->GetRHIHandle(), history->GetRHIHandle(), 0u, 0u,
                                             m_PreviousCount * sizeof(glm::uvec4));
        }
        else
            m_PreviousCount = 0;
        m_AtlasData = clip.Atlas.GetData();
        m_Capacity = capacity;
        m_Input = std::move(input);
        m_History = std::move(history);
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
        const u64 tailRecords = atlasRecords + (kCoarseIntervals + 1u) * clip.RecordsPerFrame();
        if (tailRecords >= kMaxRecords || instances.size() > kMaxRecords - tailRecords)
            return false;

        if (!m_Shader)
            m_Shader = ComputeShader::Create("assets/shaders/compute/CrowdCull.comp");
        if (!m_Shader || !m_Shader->IsValid())
            return false;
        if (!m_Parameters)
            m_Parameters = UniformBuffer::Create(sizeof(CullParams), ShaderBindingLayout::UBO_INSTANCE_CULL);
        const u32 count = static_cast<u32>(instances.size());
        if (!m_Parameters || !PrepareBuffers(count, clip))
            return false;

        if (!RenderCommand::TryUploadBufferSubData(m_Input->GetRHIHandle(), 0, instances.size_bytes(), instances.data()))
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
        params.Clock = { now, previous, loop ? 1.0f : 0.0f, 0.0f };
        params.Layout = { count, m_Capacity, clip.FrameCount, clip.BoneCount };
        const u64 frame = Renderer3D::GetPhysicalFrameSerial();
        params.PreviousCount = (!resetHistory && frame == m_Frame + 1) ? m_PreviousCount : 0u;
        m_Parameters->SetData(&params, sizeof(params));
        m_Parameters->Bind();
        RenderCommand::BindStorageBuffer(ShaderBindingLayout::SSBO_INSTANCE_CULL_INPUT, m_Input->GetRHIHandle());
        RenderCommand::BindStorageBuffer(kHistorySlot, m_History->GetRHIHandle());
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
        m_PreviousCount = count;
        m_Frame = frame;
        return true;
    }
} // namespace OloEngine
