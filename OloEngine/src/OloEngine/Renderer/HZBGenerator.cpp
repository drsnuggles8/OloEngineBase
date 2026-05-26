#include "OloEnginePCH.h"
#include "OloEngine/Renderer/HZBGenerator.h"
#include "OloEngine/Renderer/RenderCommand.h"

#include <glad/gl.h>
#include <glm/glm.hpp>

namespace OloEngine
{
    static constexpr u32 MAX_MIP_BATCH_SIZE = 4;
    static constexpr u32 LOCAL_SIZE = 8;

    u32 HZBGenerator::NextPowerOfTwo(u32 v)
    {
        v--;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        v++;
        return v;
    }

    void HZBGenerator::Initialize()
    {
        OLO_PROFILE_FUNCTION();

        m_HZBShader = ComputeShader::Create("assets/shaders/compute/HZB.comp");
        OLO_CORE_INFO("HZBGenerator: Initialized");
    }

    void HZBGenerator::Shutdown()
    {
        m_HZBShader.Reset();
        m_HZBTexture.Reset();
        m_ExternalHZBTextureID = 0;
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

        // HZB must be power-of-2 for clean mip halving
        u32 hzbW = NextPowerOfTwo(viewportWidth);
        u32 hzbH = NextPowerOfTwo(viewportHeight);

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
        m_UVFactor = glm::vec2(static_cast<f32>(viewportWidth) / static_cast<f32>(hzbW),
                               static_cast<f32>(viewportHeight) / static_cast<f32>(hzbH));

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

        // Compute full mip chain count
        u32 mipCount = static_cast<u32>(std::floor(std::log2(static_cast<f64>(std::max(hzbW, hzbH))))) + 1;
        spec.MipLevels = mipCount;

        // Always recreate — mip count may change when viewport changes
        m_HZBTexture = Texture2D::Create(spec);

        m_MipCount = mipCount;

        OLO_CORE_INFO("HZBGenerator: Resized to {}x{} ({} mips), viewport {}x{}, UVFactor ({:.3f}, {:.3f})",
                      hzbW, hzbH, m_MipCount, viewportWidth, viewportHeight, m_UVFactor.x, m_UVFactor.y);
    }

    void HZBGenerator::Generate(u32 sceneDepthTextureID)
    {
        OLO_PROFILE_FUNCTION();

        const u32 activeMipCount = (m_ExternalHZBTextureID != 0 && m_ExternalMipCount > 0) ? m_ExternalMipCount : m_MipCount;
        if (!m_HZBShader || !m_HZBShader->IsValid() || GetHZBTextureID() == 0 || activeMipCount == 0)
        {
            return;
        }

        m_HZBShader->Bind();

        // Process mips in batches of 4
        for (u32 startMip = 0; startMip < activeMipCount; startMip += MAX_MIP_BATCH_SIZE)
        {
            DispatchMipBatch(startMip, activeMipCount, sceneDepthTextureID);
        }

        m_HZBShader->Unbind();

        // Ensure HZB writes are visible before GTAO reads
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::TextureFetch | MemoryBarrierFlags::ShaderImageAccess);
    }

    void HZBGenerator::SetExternalHZBTexture(u32 textureID, u32 mipCount)
    {
        m_ExternalHZBTextureID = textureID;
        m_ExternalMipCount = mipCount;
    }

    void HZBGenerator::ClearExternalHZBTexture()
    {
        m_ExternalHZBTextureID = 0;
        m_ExternalMipCount = 0;
    }

    void HZBGenerator::DispatchMipBatch(u32 startMip, u32 mipCount, u32 sceneDepthTextureID)
    {
        u32 hzbTexID = GetHZBTextureID();
        bool isFirstPass = (startMip == 0);

        // Bind output image mips (up to 4 per batch)
        u32 endMip = std::min(startMip + MAX_MIP_BATCH_SIZE, mipCount);
        for (u32 mip = startMip; mip < endMip; mip++)
        {
            u32 localIdx = mip - startMip;
            RenderCommand::BindImageTexture(localIdx, hzbTexID, mip, false, 0, GL_WRITE_ONLY, GL_R32F);
        }
        // Fill remaining image slots with the last valid mip to avoid undefined bindings
        for (u32 localIdx = endMip - startMip; localIdx < MAX_MIP_BATCH_SIZE; localIdx++)
        {
            RenderCommand::BindImageTexture(localIdx, hzbTexID, endMip - 1, false, 0, GL_WRITE_ONLY, GL_R32F);
        }

        // Bind input: scene depth for first pass, HZB itself for subsequent passes
        if (isFirstPass)
        {
            RenderCommand::BindTexture(4, sceneDepthTextureID);
        }
        else
        {
            // Need a barrier so previous batch writes are visible as texture fetches
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::TextureFetch | MemoryBarrierFlags::ShaderImageAccess);
            RenderCommand::BindTexture(4, hzbTexID);
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
        if (isFirstPass)
        {
            // bufferUV must map HZB texel coords -> scene-depth UVs.
            // bufferUV = (threadId + 0.5) * (1/vw, 1/vh) so that an HZB
            // texel at threadId in [0, vw) lands on scene-depth pixel
            // threadId. Threads at threadId >= vw produce bufferUV > 1
            // and are clamped via u_InputViewportMaxBound to the viewport
            // edge; the resulting outside-viewport HZB region is never
            // sampled by GTAO (which uses u_HZBUVFactor = vw/hzbW).
            m_HZBShader->SetFloat2("u_DispatchThreadIdToBufferUV",
                                   glm::vec2(1.0f / static_cast<f32>(srcW), 1.0f / static_cast<f32>(srcH)));
            m_HZBShader->SetFloat2("u_InputViewportMaxBound",
                                   glm::vec2((static_cast<f32>(srcW) - 0.5f) / static_cast<f32>(srcW),
                                             (static_cast<f32>(srcH) - 0.5f) / static_cast<f32>(srcH)));
        }
        else
        {
            m_HZBShader->SetFloat2("u_DispatchThreadIdToBufferUV",
                                   glm::vec2(2.0f / static_cast<f32>(srcW), 2.0f / static_cast<f32>(srcH)));
            m_HZBShader->SetFloat2("u_InputViewportMaxBound", glm::vec2(1.0f));
        }

        m_HZBShader->SetFloat2("u_InvSize",
                               glm::vec2(1.0f / static_cast<f32>(srcW), 1.0f / static_cast<f32>(srcH)));
        m_HZBShader->SetInt("u_FirstLod", static_cast<int>(startMip));
        m_HZBShader->SetInt("u_IsFirstPass", isFirstPass ? 1 : 0);

        // Dispatch: one workgroup per LOCAL_SIZE x LOCAL_SIZE block of the destination mip
        u32 groupsX = (dstW + LOCAL_SIZE - 1) / LOCAL_SIZE;
        u32 groupsY = (dstH + LOCAL_SIZE - 1) / LOCAL_SIZE;
        RenderCommand::DispatchCompute(groupsX, groupsY, 1);
    }

    bool HZBGenerator::IsValid() const
    {
        return m_HZBShader && m_HZBShader->IsValid() && GetHZBTextureID() != 0 && m_MipCount > 0;
    }

    u32 HZBGenerator::GetHZBTextureID() const
    {
        if (m_ExternalHZBTextureID != 0)
            return m_ExternalHZBTextureID;
        return m_HZBTexture ? m_HZBTexture->GetRendererID() : 0;
    }

    u32 HZBGenerator::GetMipCount() const
    {
        return m_MipCount;
    }
} // namespace OloEngine
