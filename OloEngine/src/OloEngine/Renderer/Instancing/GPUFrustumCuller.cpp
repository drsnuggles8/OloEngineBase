#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Instancing/GPUFrustumCuller.h"

#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glad/gl.h>

namespace OloEngine
{
    namespace
    {
        constexpr u32 kCullWorkgroupSize = 256;
        // DrawElementsIndirectCommand padded to 32 bytes to match the std430
        // shader-side struct (5 active fields + 3 pad uints).
        constexpr u32 kIndirectBufferSize = 32;

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
        m_Initialised = true;
    }

    void GPUFrustumCuller::BeginFrame()
    {
        m_NextSlot = 0;
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

    void GPUFrustumCuller::EnsureSlotCapacity(PoolSlot& slot, u32 requiredCapacity)
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
        if (inputCount > 0)
        {
            slot.InputBuffer->SetData(instances.data(),
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

        m_CullShader->Bind();
        m_CullShader->SetUint("u_InstanceCount", inputCount);
        m_CullShader->SetFloat4("u_LocalBoundingSphere", localBoundingSphere);
        m_CullShader->SetFloat("u_RadiusExpansion", radiusExpansion);

        const u32 groups = (inputCount + kCullWorkgroupSize - 1) / kCullWorkgroupSize;
        if (groups > 0)
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
} // namespace OloEngine
