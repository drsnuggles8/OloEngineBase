#include "OloEnginePCH.h"
#include "OloEngine/Renderer/GPUPrefixSum.h"

#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    namespace
    {
        // Number of work groups needed to cover `count` elements.
        [[nodiscard]] constexpr u32 GroupCountFor(u32 count)
        {
            return (count + GPUPrefixSum::kGroupSize - 1) / GPUPrefixSum::kGroupSize;
        }
    } // namespace

    GPUPrefixSum::GPUPrefixSum() = default;
    GPUPrefixSum::~GPUPrefixSum() = default;

    void GPUPrefixSum::EnsureInitialised()
    {
        if (m_Initialised)
            return;

        m_ScanShader = ComputeShader::Create("assets/shaders/compute/PrefixSum_Scan.comp");
        m_AddOffsetsShader = ComputeShader::Create("assets/shaders/compute/PrefixSum_AddBlockOffsets.comp");
        m_Initialised = true;

        if (!IsAvailable())
        {
            OLO_CORE_ERROR("GPUPrefixSum: scan shaders failed to load — every caller loses its compaction");
        }
    }

    bool GPUPrefixSum::IsAvailable() const
    {
        return m_ScanShader && m_ScanShader->IsValid() && m_AddOffsetsShader && m_AddOffsetsShader->IsValid();
    }

    RHI::ResourceHandle GPUPrefixSum::DummyBuffer()
    {
        if (!m_Dummy)
        {
            m_Dummy = StorageBuffer::Create(sizeof(u32), ShaderBindingLayout::SSBO_PREFIX_SUM_TOTAL,
                                            StorageBufferUsage::DynamicCopy);
        }
        return m_Dummy->GetRHIHandle();
    }

    RHI::ResourceHandle GPUPrefixSum::AcquireBlockSums(u32 depth, u32 elementCount)
    {
        OLO_CORE_ASSERT(depth < kMaxDepth, "GPUPrefixSum recursion deeper than kMaxElements allows");

        if (m_BlockSums.size() <= depth)
            m_BlockSums.resize(depth + 1);

        const u32 requiredBytes = elementCount * static_cast<u32>(sizeof(u32));
        Ref<StorageBuffer>& slot = m_BlockSums[depth];
        if (!slot)
        {
            slot = StorageBuffer::Create(requiredBytes, ShaderBindingLayout::SSBO_PREFIX_SUM_BLOCK_SUMS,
                                         StorageBufferUsage::DynamicCopy);
        }
        else if (slot->GetSize() < requiredBytes)
        {
            // Grow monotonically — a scan never shrinks its scratch, so a
            // steady-state caller stops reallocating after its first big frame.
            slot->Resize(requiredBytes);
        }
        return slot->GetRHIHandle();
    }

    void GPUPrefixSum::UploadParams(u32 count, ScanEmit emit)
    {
        if (!m_ParamsUBO)
        {
            m_ParamsUBO = UniformBuffer::Create(UBOStructures::PrefixSumUBO::GetSize(),
                                                ShaderBindingLayout::UBO_PREFIX_SUM);
        }

        UBOStructures::PrefixSumUBO params{};
        params.Count = count;
        params.WriteBlockSums = (emit == ScanEmit::BlockSums) ? 1u : 0u;
        params.WriteTotal = (emit == ScanEmit::GrandTotal) ? 1u : 0u;

        m_ParamsUBO->SetData(&params, sizeof(params));
        // Upload THEN bind: on the Vulkan route every SetData mints a fresh
        // arena address, so a Bind() before the upload publishes the old one
        // (ADR 0011 §4 — the same ordering GPUFrustumCuller::UploadCullParams
        // documents).
        m_ParamsUBO->Bind();
    }

    void GPUPrefixSum::ScanRecursive(RHI::ResourceHandle values, u32 count,
                                     RHI::ResourceHandle totalOut, u32 depth)
    {
        // `count == 0` still dispatches ONE group. Every lane is then
        // out of range and contributes 0, nothing is written to `values`, and
        // lane 0 writes a grand total of 0 — which is exactly the right answer
        // and saves the caller a special case that would otherwise have to
        // zero `totalOut` some other way.
        const u32 groupCount = (count == 0) ? 1u : GroupCountFor(count);
        const bool singleGroup = (groupCount <= 1);

        // Block sums are only needed when there is more than one block to
        // stitch together. At the bottom level the group total IS the grand
        // total, which is where `totalOut` gets written.
        const RHI::ResourceHandle blockSums =
            singleGroup ? DummyBuffer() : AcquireBlockSums(depth, groupCount);

        RenderCommand::BindStorageBuffer(ShaderBindingLayout::SSBO_PREFIX_SUM_VALUES, values);
        RenderCommand::BindStorageBuffer(ShaderBindingLayout::SSBO_PREFIX_SUM_BLOCK_SUMS, blockSums);
        RenderCommand::BindStorageBuffer(ShaderBindingLayout::SSBO_PREFIX_SUM_TOTAL, totalOut);

        m_ScanShader->Bind();
        UploadParams(count, singleGroup ? ScanEmit::GrandTotal : ScanEmit::BlockSums);
        RenderCommand::DispatchCompute(groupCount, 1, 1);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);

        if (singleGroup)
            return;

        // Scan this level's block totals, then fold them back in. The block
        // totals are scanned EXCLUSIVELY, so block 0's offset comes out 0 and
        // the fold-back shader needs no first-block special case.
        ScanRecursive(blockSums, groupCount, totalOut, depth + 1);

        RenderCommand::BindStorageBuffer(ShaderBindingLayout::SSBO_PREFIX_SUM_VALUES, values);
        RenderCommand::BindStorageBuffer(ShaderBindingLayout::SSBO_PREFIX_SUM_BLOCK_SUMS, blockSums);

        m_AddOffsetsShader->Bind();
        UploadParams(count, ScanEmit::Nothing);
        RenderCommand::DispatchCompute(groupCount, 1, 1);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
    }

    bool GPUPrefixSum::ExclusiveScanInPlace(const Ref<StorageBuffer>& buffer, u32 count,
                                            const Ref<StorageBuffer>& totalOut)
    {
        OLO_PROFILE_FUNCTION();

        EnsureInitialised();
        if (!IsAvailable() || !buffer)
            return false;

        if (count > kMaxElements)
        {
            OLO_CORE_ERROR("GPUPrefixSum: count {0} exceeds kMaxElements {1}", count, kMaxElements);
            return false;
        }

        if (const u32 requiredBytes = count * static_cast<u32>(sizeof(u32)); buffer->GetSize() < requiredBytes)
        {
            OLO_CORE_ERROR("GPUPrefixSum: buffer holds {0} B, need {1} B for {2} elements",
                           buffer->GetSize(), requiredBytes, count);
            return false;
        }

        // `PrefixSum_Scan.comp` writes total[0] whenever u_WriteTotal is set, so
        // a totalOut smaller than one u32 is an out-of-range storage-buffer write.
        if (totalOut && totalOut->GetSize() < sizeof(u32))
        {
            OLO_CORE_ERROR("GPUPrefixSum: totalOut holds {0} B, need {1} B for the grand total",
                           totalOut->GetSize(), sizeof(u32));
            return false;
        }

        // Release slots 54/55/56 when the scan is done. Not hygiene theatre: the
        // caller's buffer is bound at 54, and a caller that frees it while it is
        // still bound leaves a dangling indexed binding for whatever runs next —
        // the buffer-shaped version of the deferred-deletion landmine in
        // testing-architecture.md §6.6. It also makes ShaderBindingLayout.h's
        // "bound immediately before each dispatch and never left bound" true
        // rather than aspirational.
        const auto releaseBindings = []()
        {
            RenderCommand::BindStorageBuffer(ShaderBindingLayout::SSBO_PREFIX_SUM_VALUES, {});
            RenderCommand::BindStorageBuffer(ShaderBindingLayout::SSBO_PREFIX_SUM_BLOCK_SUMS, {});
            RenderCommand::BindStorageBuffer(ShaderBindingLayout::SSBO_PREFIX_SUM_TOTAL, {});
        };

        ScanRecursive(buffer->GetRHIHandle(), count,
                      totalOut ? totalOut->GetRHIHandle() : DummyBuffer(), 0);
        releaseBindings();
        return true;
    }
} // namespace OloEngine
