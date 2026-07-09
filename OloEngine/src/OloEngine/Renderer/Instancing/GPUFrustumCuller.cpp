#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Instancing/GPUFrustumCuller.h"

#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/CameraRelative.h"
#include "OloEngine/Renderer/Renderer3D.h"

#include <glad/gl.h>

#include <vector>

namespace OloEngine
{
    namespace
    {
        constexpr u32 kCullWorkgroupSize = 256;
        // DrawElementsIndirectCommand padded to 32 bytes to match the std430
        // shader-side struct (5 active fields + 3 pad uints).
        constexpr u32 kIndirectBufferSize = 32;

        // Two-phase reject-list SSBO bindings (#431 Stage 2). These overlap the
        // nominal SSBO_FPLUS_SPHERE_AREA_LIGHTS (18) / SSBO_AUTO_EXPOSURE_HISTOGRAM
        // (19) slots, but the cull is a STANDALONE compute dispatch where neither
        // of those systems is bound, so the reuse is conflict-free. Must match
        // InstanceOcclusionCull.comp bindings 18 / 19.
        constexpr u32 kRejectedBinding = 18;
        constexpr u32 kRejectedCountBinding = 19;

        // 5 u32 fields + 3 padding uints. The padding must be present even
        // though the GL spec only reads the first 5 — the SSBO declares
        // them so the C++ side has to match.
        struct IndirectCommandPOD
        {
            u32 Count;
            u32 InstanceCount;
            u32 FirstIndex;
            u32 BaseVertex;
            u32 BaseInstance;
            u32 _Pad0;
            u32 _Pad1;
            u32 _Pad2;
        };
        static_assert(sizeof(IndirectCommandPOD) == kIndirectBufferSize,
                      "IndirectCommandPOD size must match the std430 InstanceCullIndirect layout");
    } // namespace

    GPUFrustumCuller::GPUFrustumCuller() = default;
    GPUFrustumCuller::~GPUFrustumCuller() = default;

    void GPUFrustumCuller::EnsureInitialised()
    {
        if (m_Initialised)
            return;

        m_CullShader = ComputeShader::Create("assets/shaders/compute/InstanceFrustumCull.comp");
        // Frustum + Hi-Z occlusion variant (#431). A load failure is non-fatal:
        // m_OcclusionCullShader stays null and Cull() keeps using the
        // frustum-only path even when occlusion inputs are supplied.
        m_OcclusionCullShader = ComputeShader::Create("assets/shaders/compute/InstanceOcclusionCull.comp");
        m_Initialised = true;
    }

    void GPUFrustumCuller::BeginFrame()
    {
        m_NextSlot = 0;
        // Drop last frame's occlusion inputs; the owner re-supplies them via
        // SetOcclusion() only when a valid retained HZB exists this frame.
        m_Occlusion = {};
    }

    GPUFrustumCuller::PoolSlot& GPUFrustumCuller::AcquireSlot(u32 requiredCapacity)
    {
        // Either reuse an existing pool slot or grow the pool by one.
        if (m_NextSlot >= m_Pool.size())
        {
            PoolSlot slot;
            slot.OutputBuffer = Ref<InstanceBuffer>::Create(std::max(requiredCapacity, 64u));
            slot.InputBuffer = StorageBuffer::Create(
                std::max(requiredCapacity, 64u) * static_cast<u32>(sizeof(InstanceData)),
                ShaderBindingLayout::SSBO_INSTANCE_CULL_INPUT,
                StorageBufferUsage::DynamicDraw);
            slot.IndirectBuffer = StorageBuffer::Create(
                kIndirectBufferSize,
                ShaderBindingLayout::SSBO_INSTANCE_DRAW_INDIRECT,
                StorageBufferUsage::DynamicCopy);
            slot.Capacity = std::max(requiredCapacity, 64u);
            m_Pool.push_back(std::move(slot));
        }

        PoolSlot& slot = m_Pool[m_NextSlot++];
        EnsureSlotCapacity(slot, requiredCapacity);
        return slot;
    }

    void GPUFrustumCuller::EnsureSlotCapacity(PoolSlot& slot, u32 requiredCapacity) const
    {
        if (requiredCapacity <= slot.Capacity)
            return;

        // Geometric growth to amortise reallocations across frames.
        u32 newCapacity = slot.Capacity * 2;
        if (newCapacity < requiredCapacity)
            newCapacity = requiredCapacity;

        slot.OutputBuffer->EnsureCapacity(newCapacity);
        slot.InputBuffer->Resize(newCapacity * static_cast<u32>(sizeof(InstanceData)));
        slot.Capacity = newCapacity;
    }

    GPUFrustumCuller::CullResult GPUFrustumCuller::Cull(std::span<const InstanceData> instances,
                                                        u32 indexCount, u32 baseIndex,
                                                        const glm::vec4& localBoundingSphere,
                                                        f32 radiusExpansion)
    {
        OLO_PROFILE_FUNCTION();

        EnsureInitialised();

        const u32 inputCount = static_cast<u32>(instances.size());
        PoolSlot& slot = AcquireSlot(inputCount);

        // Grow the output buffer to the worst case (all instances survive) so
        // the compute shader can write without a bounds check. Bigger than
        // strictly necessary on average, but predictable and avoids per-frame
        // resizes when the survivor count fluctuates.
        slot.OutputBuffer->EnsureCapacity(inputCount);

        // ── 1. Upload the full input list ────────────────────────────────
        // Camera-relative (issue #429): the compute shader culls these instances
        // against world-space frustum planes it extracts from u_ViewProjection
        // (now the *relative* view-projection) and writes the survivors straight
        // into the model instance buffer the draw reads, so the transforms must
        // be render-relative. Shift each instance's transform (and prev
        // transform) into a scratch copy before upload. No-op at the origin.
        if (inputCount > 0)
        {
            const glm::vec3 origin = Renderer3D::GetRenderOrigin();
            thread_local std::vector<InstanceData> scratch;
            scratch.assign(instances.begin(), instances.end());
            for (InstanceData& inst : scratch)
            {
                inst.Transform = MakeModelRelative(inst.Transform, origin);
                inst.PrevTransform = MakeModelRelative(inst.PrevTransform, origin);
            }
            slot.InputBuffer->SetData(scratch.data(),
                                      inputCount * static_cast<u32>(sizeof(InstanceData)),
                                      0);
        }

        // ── 2. Seed the indirect command (instanceCount = 0; compute atomic-adds)
        // The indices count / firstIndex / baseVertex come from the mesh; the
        // compute only touches `instanceCount`.
        IndirectCommandPOD seed{};
        seed.Count = indexCount;
        seed.InstanceCount = 0;
        seed.FirstIndex = baseIndex;
        seed.BaseVertex = 0;
        seed.BaseInstance = 0;
        slot.IndirectBuffer->SetData(&seed, sizeof(seed), 0);

        // ── 3. Bind SSBOs and dispatch ───────────────────────────────────
        slot.InputBuffer->Bind();    // SSBO_INSTANCE_CULL_INPUT (16)
        slot.OutputBuffer->Bind();   // SSBO_INSTANCE_DATA (15)
        slot.IndirectBuffer->Bind(); // SSBO_INSTANCE_DRAW_INDIRECT (17)

        // Pick the frustum-only or frustum + Hi-Z occlusion compute (#431). The
        // occlusion variant rejects, in addition to the frustum test, instances
        // whose screen-space bounds are fully behind the retained depth pyramid.
        const bool useOcclusion = IsOcclusionActive();
        const Ref<ComputeShader>& cullShader = useOcclusion ? m_OcclusionCullShader : m_CullShader;

        cullShader->Bind();
        cullShader->SetUint("u_InstanceCount", inputCount);
        cullShader->SetFloat4("u_LocalBoundingSphere", localBoundingSphere);
        cullShader->SetFloat("u_RadiusExpansion", radiusExpansion);

        if (useOcclusion)
        {
            // Bind the previous-frame HZB at texture unit 0 (matches
            // `layout(binding = 0) uniform sampler2D u_HZB` in the shader). The
            // cull runs at submission time on scratch GL texture state, so unit
            // 0 is safe to clobber — the real draws rebind their own textures at
            // graph-execute time.
            RenderCommand::BindTexture(0, m_Occlusion.HZBTextureID);
            cullShader->SetInt("u_OcclusionEnabled", 1);
            cullShader->SetMat4("u_PrevViewProjection", m_Occlusion.PrevViewProjection);
            cullShader->SetFloat2("u_HZBSize", m_Occlusion.HZBSize);
            cullShader->SetFloat2("u_HZBUVFactor", m_Occlusion.HZBUVFactor);
            cullShader->SetInt("u_HZBMipCount", static_cast<int>(m_Occlusion.MipCount));
            cullShader->SetFloat("u_OcclusionDepthBias", m_Occlusion.DepthBias);
        }

        if (const u32 groups = (inputCount + kCullWorkgroupSize - 1) / kCullWorkgroupSize; groups > 0)
            RenderCommand::DispatchCompute(groups, 1, 1);

        // ── 4. Barrier — the draw must see the compacted output AND the
        // updated indirect command before it reads them. SHADER_STORAGE for
        // the InstanceData SSBO read by the vertex shader; COMMAND for the
        // indirect-args buffer read by glDrawElementsIndirect.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::Command);

        CullResult result;
        result.OutputBuffer = slot.OutputBuffer;
        result.IndirectBuffer = slot.IndirectBuffer;
        result.InputCount = inputCount;
        return result;
    }

    void GPUFrustumCuller::EnsureTwoPhaseBuffers(PoolSlot& slot, u32 requiredCapacity) const
    {
        const u32 cap = std::max(requiredCapacity, 64u);
        if (slot.RejectedBuffer && slot.RejectedCounter && slot.Phase2Output && slot.Phase2Indirect &&
            cap <= slot.TwoPhaseCapacity)
            return;

        u32 newCapacity = slot.TwoPhaseCapacity ? slot.TwoPhaseCapacity * 2 : cap;
        if (newCapacity < cap)
            newCapacity = cap;

        const u32 instanceBytes = newCapacity * static_cast<u32>(sizeof(InstanceData));
        if (!slot.RejectedBuffer)
            slot.RejectedBuffer = StorageBuffer::Create(instanceBytes, kRejectedBinding, StorageBufferUsage::DynamicCopy);
        else
            slot.RejectedBuffer->Resize(instanceBytes);

        if (!slot.RejectedCounter)
            slot.RejectedCounter = StorageBuffer::Create(static_cast<u32>(sizeof(u32)), kRejectedCountBinding, StorageBufferUsage::DynamicCopy);

        if (!slot.Phase2Output)
            slot.Phase2Output = Ref<InstanceBuffer>::Create(newCapacity);
        else
            slot.Phase2Output->EnsureCapacity(newCapacity);

        if (!slot.Phase2Indirect)
            slot.Phase2Indirect = StorageBuffer::Create(kIndirectBufferSize, ShaderBindingLayout::SSBO_INSTANCE_DRAW_INDIRECT, StorageBufferUsage::DynamicCopy);

        slot.TwoPhaseCapacity = newCapacity;
    }

    GPUFrustumCuller::TwoPhaseCullResult GPUFrustumCuller::CullTwoPhasePhase1(std::span<const InstanceData> instances,
                                                                              u32 indexCount, u32 baseIndex,
                                                                              const glm::vec4& localBoundingSphere,
                                                                              f32 radiusExpansion)
    {
        OLO_PROFILE_FUNCTION();
        EnsureInitialised();

        const u32 inputCount = static_cast<u32>(instances.size());
        PoolSlot& slot = AcquireSlot(inputCount);
        EnsureTwoPhaseBuffers(slot, inputCount);
        slot.OutputBuffer->EnsureCapacity(inputCount);
        slot.Phase2Output->EnsureCapacity(inputCount);

        // Camera-relative (issue #429): the phase-1/phase-2 survivors are written
        // straight into the model instance buffer the draw reads (binding 15) and
        // the cull tests against the *relative* view-projection, so the uploaded
        // transforms must be render-relative — mirror the single-phase Cull()
        // shift. No-op at the origin.
        if (inputCount > 0)
        {
            const glm::vec3 origin = Renderer3D::GetRenderOrigin();
            thread_local std::vector<InstanceData> scratch;
            scratch.assign(instances.begin(), instances.end());
            for (InstanceData& inst : scratch)
            {
                inst.Transform = MakeModelRelative(inst.Transform, origin);
                inst.PrevTransform = MakeModelRelative(inst.PrevTransform, origin);
            }
            slot.InputBuffer->SetData(scratch.data(), inputCount * static_cast<u32>(sizeof(InstanceData)), 0);
        }

        // Seed the phase-1 indirect command (compute atomic-adds survivors) and
        // zero the reject counter (compute atomic-adds rejects).
        IndirectCommandPOD seed{};
        seed.Count = indexCount;
        seed.FirstIndex = baseIndex;
        slot.IndirectBuffer->SetData(&seed, sizeof(seed), 0);
        const u32 zero = 0;
        slot.RejectedCounter->SetData(&zero, static_cast<u32>(sizeof(u32)), 0);

        slot.InputBuffer->Bind();    // 16 — full input
        slot.OutputBuffer->Bind();   // 15 — phase-1 survivors
        slot.IndirectBuffer->Bind(); // 17 — phase-1 indirect
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kRejectedBinding, slot.RejectedBuffer->GetRendererID());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kRejectedCountBinding, slot.RejectedCounter->GetRendererID());

        // Use the occlusion variant when a previous-frame HZB is available; else
        // (frame 0 / no HZB) fall back to the frustum-only shader — no rejects
        // are produced and every frustum survivor draws in phase 1.
        const bool occlusionActive = IsOcclusionActive();
        const Ref<ComputeShader>& shader = occlusionActive ? m_OcclusionCullShader : m_CullShader;

        shader->Bind();
        shader->SetUint("u_InstanceCount", inputCount);
        shader->SetFloat4("u_LocalBoundingSphere", localBoundingSphere);
        shader->SetFloat("u_RadiusExpansion", radiusExpansion);
        if (occlusionActive)
        {
            RenderCommand::BindTexture(0, m_Occlusion.HZBTextureID); // previous-frame HZB
            shader->SetInt("u_OcclusionEnabled", 1);
            shader->SetMat4("u_PrevViewProjection", m_Occlusion.PrevViewProjection);
            shader->SetFloat2("u_HZBSize", m_Occlusion.HZBSize);
            shader->SetFloat2("u_HZBUVFactor", m_Occlusion.HZBUVFactor);
            shader->SetInt("u_HZBMipCount", static_cast<int>(m_Occlusion.MipCount));
            shader->SetFloat("u_OcclusionDepthBias", m_Occlusion.DepthBias);
            shader->SetInt("u_WriteRejected", 1); // defer occluded instances to phase 2
            shader->SetInt("u_Phase2", 0);
        }

        if (const u32 groups = (inputCount + kCullWorkgroupSize - 1) / kCullWorkgroupSize; groups > 0)
            RenderCommand::DispatchCompute(groups, 1, 1);

        // The phase-1 draw reads survivors + indirect; phase 2 reads the reject
        // buffer + counter. Make all of them visible.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::Command);

        TwoPhaseCullResult result;
        result.Phase1Output = slot.OutputBuffer;
        result.Phase1Indirect = slot.IndirectBuffer;
        result.Phase2Output = slot.Phase2Output;
        result.Phase2Indirect = slot.Phase2Indirect;
        result.RejectedBuffer = slot.RejectedBuffer;
        result.RejectedCounter = slot.RejectedCounter;
        result.InputCount = inputCount;
        result.IndexCount = indexCount;
        result.BaseIndex = baseIndex;
        result.LocalBoundingSphere = localBoundingSphere;
        result.RadiusExpansion = radiusExpansion;
        return result;
    }

    void GPUFrustumCuller::DispatchPhase2(const TwoPhaseCullResult& result, const HZBOcclusionInputs& currentHZB)
    {
        OLO_PROFILE_FUNCTION();

        // Nothing to recover without a current-frame HZB, the phase-2 shader, or
        // any input. (Frame 0 falls here: phase 1 ran frustum-only, no rejects.)
        if (!m_OcclusionCullShader || !currentHZB.IsUsable() || result.InputCount == 0 ||
            !result.RejectedBuffer || !result.Phase2Output || !result.Phase2Indirect || !result.RejectedCounter)
            return;

        IndirectCommandPOD seed{};
        seed.Count = result.IndexCount;
        seed.FirstIndex = result.BaseIndex;
        // Copy the Ref to a non-const handle so SetData() (non-const) is callable
        // even though `result` is const — the underlying GPU buffer is shared.
        Ref<StorageBuffer> phase2Indirect = result.Phase2Indirect;
        phase2Indirect->SetData(&seed, sizeof(seed), 0);

        // Bind the reject buffer AS the input (16); phase-2 survivors (15) and
        // indirect (17) are this slot's phase-2 buffers; the reject counter (19)
        // bounds the dispatch in-shader.
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ShaderBindingLayout::SSBO_INSTANCE_CULL_INPUT,
                         result.RejectedBuffer->GetRendererID());
        result.Phase2Output->Bind(); // 15
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ShaderBindingLayout::SSBO_INSTANCE_DRAW_INDIRECT,
                         result.Phase2Indirect->GetRendererID());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kRejectedCountBinding, result.RejectedCounter->GetRendererID());
        RenderCommand::BindTexture(0, currentHZB.HZBTextureID); // current-frame HZB

        m_OcclusionCullShader->Bind();
        // Dispatch the worst case (whole batch); the shader clamps to the live
        // reject count read from binding 19.
        m_OcclusionCullShader->SetUint("u_InstanceCount", result.InputCount);
        m_OcclusionCullShader->SetFloat4("u_LocalBoundingSphere", result.LocalBoundingSphere);
        m_OcclusionCullShader->SetFloat("u_RadiusExpansion", result.RadiusExpansion);
        m_OcclusionCullShader->SetInt("u_OcclusionEnabled", 1);
        // Current-frame HZB is in CURRENT screen space → reproject with the
        // current VP (the caller stores it in PrevViewProjection).
        m_OcclusionCullShader->SetMat4("u_PrevViewProjection", currentHZB.PrevViewProjection);
        m_OcclusionCullShader->SetFloat2("u_HZBSize", currentHZB.HZBSize);
        m_OcclusionCullShader->SetFloat2("u_HZBUVFactor", currentHZB.HZBUVFactor);
        m_OcclusionCullShader->SetInt("u_HZBMipCount", static_cast<int>(currentHZB.MipCount));
        m_OcclusionCullShader->SetFloat("u_OcclusionDepthBias", currentHZB.DepthBias);
        m_OcclusionCullShader->SetInt("u_WriteRejected", 0);
        m_OcclusionCullShader->SetInt("u_Phase2", 1);

        if (const u32 groups = (result.InputCount + kCullWorkgroupSize - 1) / kCullWorkgroupSize; groups > 0)
            RenderCommand::DispatchCompute(groups, 1, 1);

        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::Command);
    }
} // namespace OloEngine
