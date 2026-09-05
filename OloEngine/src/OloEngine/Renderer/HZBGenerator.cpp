#include "OloEnginePCH.h"
#include "OloEngine/Renderer/HZBGenerator.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    static constexpr u32 MAX_MIP_BATCH_SIZE = 4;
    static constexpr u32 LOCAL_SIZE = 8;

    u32 HZBGenerator::NextPowerOfTwo(u32 v)
    {
        --v;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        ++v;
        return v;
    }

    HZBGenerator::Dimensions HZBGenerator::ComputeDimensions(u32 viewportWidth, u32 viewportHeight)
    {
        Dimensions dims;
        if (viewportWidth == 0 || viewportHeight == 0)
            return dims;

        // HZB must be power-of-2 for clean mip halving.
        dims.Width = NextPowerOfTwo(viewportWidth);
        dims.Height = NextPowerOfTwo(viewportHeight);
        dims.MipCount = static_cast<u32>(std::floor(std::log2(static_cast<f64>(std::max(dims.Width, dims.Height))))) + 1;
        // UV factor: viewport → HZB texture coordinate mapping (hzbUV = screenUV * UVFactor).
        dims.UVFactor = glm::vec2(static_cast<f32>(viewportWidth) / static_cast<f32>(dims.Width),
                                  static_cast<f32>(viewportHeight) / static_cast<f32>(dims.Height));
        return dims;
    }

    void HZBGenerator::Initialize()
    {
        OLO_PROFILE_FUNCTION();

        m_HZBShader = ComputeShader::Create("assets/shaders/compute/HZB.comp");
        m_ParamsUBO = UniformBuffer::Create(UBOStructures::HZBParamsUBO::GetSize(),
                                            ShaderBindingLayout::UBO_HZB);
        OLO_CORE_INFO("HZBGenerator: Initialized");
    }

    void HZBGenerator::Shutdown()
    {
        m_HZBShader.Reset();
        m_ParamsUBO.Reset();
        m_HZBTexture.Reset();
        m_ExternalHZBTexture = RHI::NullResource;
        m_ExternalMipCount = 0;
        m_HZBWidth = 0;
        m_HZBHeight = 0;
        m_MipCount = 0;
    }

    void HZBGenerator::Reload()
    {
        if (m_HZBShader)
        {
            m_HZBShader->Reload();
        }
    }

    void HZBGenerator::Resize(u32 viewportWidth, u32 viewportHeight)
    {
        OLO_PROFILE_FUNCTION();

        if (viewportWidth == 0 || viewportHeight == 0)
        {
            return;
        }

        m_ViewportWidth = viewportWidth;
        m_ViewportHeight = viewportHeight;

        const Dimensions dims = ComputeDimensions(viewportWidth, viewportHeight);
        const u32 hzbW = dims.Width;
        const u32 hzbH = dims.Height;

        // UV factor: viewport → HZB texture coordinate mapping. MUST be
        // recomputed every Resize() even when the HZB texture itself is
        // reused: a viewport drag from e.g. 4584×2515 to 4578×2515 stays
        // inside the same power-of-2 bucket (8192×4096), so the texture
        // really doesn't need re-creating — but the UV scale viewport/hzb
        // *does* change. Leaving m_UVFactor at its previous value makes GTAO
        // sample the HZB at the *old* viewport-to-HZB ratio while normals
        // sample at the correct live viewport ratio. The mismatch lands the
        // AO mask offset from the geometry along the resized axis (resize Y
        // → AO offset in Y; resize X → offset in X) and persists for the
        // rest of the session because every subsequent same-bucket resize
        // hits the early-return below without refreshing the factor. This
        // is the post-resize "ghost halo" reported in the bug tracker.
        m_UVFactor = dims.UVFactor;

        if (hzbW == m_HZBWidth && hzbH == m_HZBHeight && m_HZBTexture)
        {
            return; // No change to HZB texture; UVFactor refresh above is sufficient.
        }

        m_HZBWidth = hzbW;
        m_HZBHeight = hzbH;

        // Create or resize HZB texture with full mip chain
        TextureSpecification spec;
        spec.Width = hzbW;
        spec.Height = hzbH;
        spec.Format = ImageFormat::R32F;
        spec.GenerateMips = false;
        spec.MipLevels = dims.MipCount;

        // Always recreate — mip count may change when viewport changes
        m_HZBTexture = Texture2D::Create(spec);

        m_MipCount = dims.MipCount;

        OLO_CORE_INFO("HZBGenerator: Resized to {}x{} ({} mips), viewport {}x{}, UVFactor ({:.3f}, {:.3f})",
                      hzbW, hzbH, m_MipCount, viewportWidth, viewportHeight, m_UVFactor.x, m_UVFactor.y);
    }

    void HZBGenerator::Generate(RHI::ResourceHandle sceneDepthTexture)
    {
        OLO_PROFILE_FUNCTION();

        const u32 activeMipCount = (m_ExternalHZBTexture.IsValid() && m_ExternalMipCount > 0) ? m_ExternalMipCount : m_MipCount;
        if (!m_HZBShader || !m_HZBShader->IsValid() || !GetHZBTexture().IsValid() || activeMipCount == 0)
        {
            return;
        }

        m_HZBShader->Bind();

        // Process mips in batches of 4
        for (u32 startMip = 0; startMip < activeMipCount; startMip += MAX_MIP_BATCH_SIZE)
        {
            DispatchMipBatch(startMip, activeMipCount, sceneDepthTexture);
        }

        m_HZBShader->Unbind();

        // Ensure HZB writes are visible before GTAO reads
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::TextureFetch | MemoryBarrierFlags::ShaderImageAccess);
    }

    void HZBGenerator::SetExternalHZBTexture(RHI::ResourceHandle texture, u32 mipCount)
    {
        m_ExternalHZBTexture = texture;
        m_ExternalMipCount = mipCount;
    }

    void HZBGenerator::ClearExternalHZBTexture()
    {
        m_ExternalHZBTexture = RHI::NullResource;
        m_ExternalMipCount = 0;
    }

    void HZBGenerator::DispatchMipBatch(u32 startMip, u32 mipCount, RHI::ResourceHandle sceneDepthTexture)
    {
        const RHI::ResourceHandle hzbTex = GetHZBTexture();
        const RHI::HeapSlotLifetime hzbLifetime = GetHZBLifetime();
        bool isFirstPass = (startMip == 0);

        // Bind output image mips (up to 4 per batch)
        u32 endMip = std::min(startMip + MAX_MIP_BATCH_SIZE, mipCount);
        // FOUR MIPS OF ONE TEXTURE, IN ONE DISPATCH — the case that forced the
        // descriptor-heap memo cache to carry the whole ViewDesc. Its old key was
        // (resource, samplerSlot, depthCompare), which cannot tell these four
        // apart; they would all have been served the first view and the entire
        // pyramid would have been written at level 0, in a frame that still looks
        // plausible until something reads the occlusion result.
        for (u32 mip = startMip; mip < endMip; ++mip)
        {
            u32 localIdx = mip - startMip;
            HeapBinding::BindImageOrOffset(localIdx, hzbTex, mip, false, 0, RHI::Access::StorageWrite,
                                           RHI::Format::R32Float, hzbLifetime);
        }
        // Fill remaining image slots with the last valid mip to avoid undefined bindings
        for (u32 localIdx = endMip - startMip; localIdx < MAX_MIP_BATCH_SIZE; ++localIdx)
        {
            HeapBinding::BindImageOrOffset(localIdx, hzbTex, endMip - 1, false, 0, RHI::Access::StorageWrite,
                                           RHI::Format::R32Float, hzbLifetime);
        }

        // Bind input: scene depth for first pass, HZB itself for subsequent passes.
        //
        // THE TWO BRANCHES TAKE DIFFERENT LIFETIMES for the same slot, and that is
        // correct rather than sloppy: the scene depth is a graph-pooled resource
        // (FrameTransient — a Persistent view would memoise an offset onto an
        // object the planner may reassign next frame), while the HZB pyramid is
        // this generator's own texture (Persistent, memoisable). The lifetime
        // follows the RESOURCE, never the slot (issue #691).
        if (isFirstPass)
        {
            HeapBinding::BindTextureOrOffset(4, sceneDepthTexture, RHI::HeapSlotLifetime::FrameTransient);
        }
        else
        {
            // Need a barrier so previous batch writes are visible as texture fetches
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::TextureFetch | MemoryBarrierFlags::ShaderImageAccess);
            HeapBinding::BindTextureOrOffset(4, hzbTex, hzbLifetime);
        }

        // Compute source and destination sizes.
        //
        // First pass reads from the SCENE DEPTH texture, which is sized to
        // the viewport (m_ViewportWidth x m_ViewportHeight) — NOT to the
        // power-of-two HZB. Using `m_HZBWidth >> 0` here would cause the
        // shader to sample scene depth at UVs in [0, 1] of the *HZB*, which
        // effectively stretches the viewport-sized depth across the full
        // HZB. Downstream (GTAO) compensates with u_HZBUVFactor = vw/hzbW
        // when sampling, so the net effect is that GTAO reads depth at
        // viewport UVs scaled by a factor of (vw/hzbW)² — phantom geometry
        // appears at mismatched positions because normals (sampled at the
        // correct viewport UV) and depth (effectively double-scaled)
        // disagree about where geometry is. SSAO is unaffected because it
        // reads scene depth directly without going through the HZB.
        //
        // Subsequent passes read from the HZB itself, where mip N really
        // is `m_HZBWidth >> N`, so the original computation is correct.
        u32 srcW;
        u32 srcH;
        if (isFirstPass)
        {
            srcW = std::max(1u, m_ViewportWidth);
            srcH = std::max(1u, m_ViewportHeight);
        }
        else
        {
            const u32 parentMip = startMip - 1;
            srcW = std::max(1u, m_HZBWidth >> parentMip);
            srcH = std::max(1u, m_HZBHeight >> parentMip);
        }
        u32 dstW = std::max(1u, m_HZBWidth >> startMip);
        u32 dstH = std::max(1u, m_HZBHeight >> startMip);

        // Set uniforms
        // One std140 refill per 4-mip batch — these were bare uniforms via
        // ComputeShader::Set*, a deliberate no-op on the Vulkan route (issue
        // #691). Per-batch SetData is legal on both backends (GL
        // re-uploads the bound buffer; Vulkan's arena-versioned UBOs mint a
        // fresh per-dispatch address on every SetData).

        UBOStructures::HZBParamsUBO hzbParams{};
        if (isFirstPass)
        {
            // bufferUV must map HZB texel coords -> scene-depth UVs.
            // bufferUV = (threadId + 0.5) * (1/vw, 1/vh) so that an HZB
            // texel at threadId in [0, vw) lands on scene-depth pixel
            // threadId. Threads at threadId >= vw produce bufferUV > 1
            // and are clamped via u_InputViewportMaxBound to the viewport
            // edge; the resulting outside-viewport HZB region is never
            // sampled by GTAO (which uses u_HZBUVFactor = vw/hzbW).
            hzbParams.DispatchThreadIdToBufferUV =
                glm::vec2(1.0f / static_cast<f32>(srcW), 1.0f / static_cast<f32>(srcH));
            hzbParams.InputViewportMaxBound =
                glm::vec2((static_cast<f32>(srcW) - 0.5f) / static_cast<f32>(srcW),
                          (static_cast<f32>(srcH) - 0.5f) / static_cast<f32>(srcH));
        }
        else
        {
            hzbParams.DispatchThreadIdToBufferUV =
                glm::vec2(2.0f / static_cast<f32>(srcW), 2.0f / static_cast<f32>(srcH));
            hzbParams.InputViewportMaxBound = glm::vec2(1.0f);
        }
        hzbParams.InvSize = glm::vec2(1.0f / static_cast<f32>(srcW), 1.0f / static_cast<f32>(srcH));
        hzbParams.FirstLod = static_cast<i32>(startMip);
        hzbParams.IsFirstPass = isFirstPass ? 1 : 0;
        hzbParams.ReduceOp = static_cast<i32>(m_ReduceMode);
        m_ParamsUBO->SetData(&hzbParams, sizeof(hzbParams));
        m_ParamsUBO->Bind();

        // Dispatch: one workgroup per LOCAL_SIZE x LOCAL_SIZE block of the destination mip
        u32 groupsX = (dstW + LOCAL_SIZE - 1) / LOCAL_SIZE;
        u32 groupsY = (dstH + LOCAL_SIZE - 1) / LOCAL_SIZE;
        // One flush per DISPATCH, not per batch: this function is called once per
        // 4-mip batch and each batch stages its own four offsets, so a flush hoisted
        // out to the caller would publish only the last batch's.
        HeapBinding::FlushOffsets();
        RenderCommand::DispatchCompute(groupsX, groupsY, 1);
    }

    bool HZBGenerator::IsValid() const
    {
        return m_HZBShader && m_HZBShader->IsValid() && GetHZBTexture().IsValid() && m_MipCount > 0;
    }

    RHI::ResourceHandle HZBGenerator::GetHZBTexture() const
    {
        if (m_ExternalHZBTexture.IsValid())
            return m_ExternalHZBTexture;
        return m_HZBTexture ? m_HZBTexture->GetRHIHandle() : RHI::NullResource;
    }

    u32 HZBGenerator::GetMipCount() const
    {
        return m_MipCount;
    }
} // namespace OloEngine
