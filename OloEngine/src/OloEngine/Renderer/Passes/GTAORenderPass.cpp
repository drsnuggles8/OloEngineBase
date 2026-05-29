#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/Passes/GTAORenderPass.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glad/gl.h>

namespace OloEngine
{
    // Hilbert curve LUT: maps (x,y) in a 64×64 grid to a 1D index.
    // Used for spatiotemporal noise in GTAO to decorrelate samples.
    static constexpr u32 HILBERT_SIZE = 64;
    static constexpr u32 GTAO_HZB_TEXTURE_SLOT = 3;
    static constexpr u32 GTAO_NORMALS_TEXTURE_SLOT = 4;
    static constexpr u32 GTAO_HILBERT_TEXTURE_SLOT = 5;

    // Generate Hilbert curve index for a given (x, y) coordinate
    static u16 HilbertIndex(u32 x, u32 y)
    {
        u32 d = 0;
        for (u32 s = HILBERT_SIZE / 2; s > 0; s /= 2)
        {
            u32 rx = (x & s) > 0 ? 1 : 0;
            u32 ry = (y & s) > 0 ? 1 : 0;
            d += s * s * ((3 * rx) ^ ry);
            // Rotate
            if (ry == 0)
            {
                if (rx == 1)
                {
                    x = s - 1 - x;
                    y = s - 1 - y;
                }
                std::swap(x, y);
            }
        }
        return static_cast<u16>(d & 0xFFFF);
    }

    GTAORenderPass::GTAORenderPass()
    {
        SetName("GTAOPass");
        // Phase G slice 1 — compute-only; HZB + GTAO main pass + denoise all dispatch compute.
        // Candidate for async-compute overlap once multi-queue scheduling is added (Phase G.2).
        SetPassWorkType(PassWorkType::Compute);
        SetAsyncComputeCandidate(true);
    }

    void GTAORenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedSceneDepthTexture = {};
        m_SelectedSceneNormalsTexture = {};
        m_SelectedAOOutputTexture = {};
        m_SelectedEdgeTexture = {};
        m_SelectedHZBDepthTexture = {};
        m_SelectedDenoisePingTexture = {};
        m_SelectedDenoisePongTexture = {};

        if (!m_Settings.GTAOEnabled || m_Settings.ActiveAOTechnique != AOTechnique::GTAO)
            return;

        const bool willDispatchDenoise = m_Settings.GTAODenoiseEnabled && m_Settings.GTAODenoisePasses > 0;

        if (blackboard.Scene.SceneDepth.IsValid())
        {
            m_SelectedSceneDepthTexture = blackboard.Scene.SceneDepth;
            [[maybe_unused]] const auto sceneDepthRead = builder.Read(blackboard.Scene.SceneDepth, RGReadUsage::ShaderSample);
        }
        if (blackboard.Scene.SceneNormals.IsValid())
        {
            m_SelectedSceneNormalsTexture = blackboard.Scene.SceneNormals;
            [[maybe_unused]] const auto sceneNormalsRead = builder.Read(blackboard.Scene.SceneNormals, RGReadUsage::ShaderSample);
        }
        if (blackboard.AO.AOBuffer.IsValid())
        {
            m_SelectedAOOutputTexture = blackboard.AO.AOBuffer;
            // AOBuffer is written via glCopyImageSubData from the final denoise
            // ping-pong slot, not a compute image-store. The dispatch path
            // writes to GTAODenoisePing/Pong (declared as ShaderImage above).
            builder.Write(blackboard.AO.AOBuffer, RGWriteUsage::TransferDest);
        }

        if (m_Width == 0u || m_Height == 0u)
            return;

        const auto nextPow2 = [](u32 value)
        {
            u32 result = 1u;
            while (result < value)
                result <<= 1u;
            return result;
        };

        const auto hzbW = nextPow2(m_Width);
        const auto hzbH = nextPow2(m_Height);
        u32 mipCount = 1u;
        for (u32 mipW = hzbW, mipH = hzbH; mipW > 1u || mipH > 1u; ++mipCount)
        {
            mipW = mipW > 1u ? (mipW / 2u) : 1u;
            mipH = mipH > 1u ? (mipH / 2u) : 1u;
        }

        if (blackboard.Scratch.HZBDepth.IsValid())
        {
            m_SelectedHZBDepthTexture = blackboard.Scratch.HZBDepth;
            // Intra-pass HZB mip-chain reduction: each compute dispatch reads
            // mip[i-1] and writes mip[i] of the same texture in sequence.
            builder.AllowSamePassReadWrite(blackboard.Scratch.HZBDepth);

            bool canUseMipViews = mipCount <= static_cast<u32>(blackboard.Scratch.HZBDepthMipViews.size());
            if (canUseMipViews)
            {
                for (u32 mip = 0u; mip < mipCount; ++mip)
                {
                    if (!blackboard.Scratch.HZBDepthMipViews[mip].IsValid())
                    {
                        canUseMipViews = false;
                        break;
                    }
                }
            }

            if (canUseMipViews)
            {
                builder.Write(blackboard.Scratch.HZBDepthMipViews[0u], RGWriteUsage::ShaderImage);
                for (u32 mip = 1u; mip < mipCount; ++mip)
                {
                    [[maybe_unused]] const auto hzbMipRead =
                        builder.Read(blackboard.Scratch.HZBDepthMipViews[mip - 1u], RGReadUsage::ShaderSample);
                    builder.Write(blackboard.Scratch.HZBDepthMipViews[mip], RGWriteUsage::ShaderImage);
                }
            }
            else
            {
                builder.Write(blackboard.Scratch.HZBDepth, RGWriteUsage::ShaderImage, RGSubresourceRange::Mip(0u));
                for (u32 mip = 1u; mip < mipCount; ++mip)
                {
                    [[maybe_unused]] const auto hzbMipRead =
                        builder.Read(blackboard.Scratch.HZBDepth, RGReadUsage::ShaderSample, RGSubresourceRange::Mip(mip - 1u));
                    builder.Write(blackboard.Scratch.HZBDepth, RGWriteUsage::ShaderImage, RGSubresourceRange::Mip(mip));
                }
            }

            [[maybe_unused]] const auto hzbRead = builder.Read(blackboard.Scratch.HZBDepth, RGReadUsage::ShaderSample);
        }

        if (blackboard.Scratch.GTAOEdge.IsValid())
        {
            m_SelectedEdgeTexture = blackboard.Scratch.GTAOEdge;
            // Intra-pass write-then-imageLoad: the GTAO main dispatch writes
            // GTAOEdge, then the denoise dispatch reads it back via imageLoad.
            builder.AllowSamePassReadWrite(blackboard.Scratch.GTAOEdge);
            builder.Write(blackboard.Scratch.GTAOEdge, RGWriteUsage::ShaderImage);
            [[maybe_unused]] const auto edgeRead = builder.Read(blackboard.Scratch.GTAOEdge, RGReadUsage::ShaderImage);
        }

        if (blackboard.Scratch.GTAODenoisePing.IsValid())
        {
            m_SelectedDenoisePingTexture = blackboard.Scratch.GTAODenoisePing;
            if (willDispatchDenoise)
            {
                // Intra-pass denoise ping-pong: each denoise iteration reads
                // one and writes the other; the chain alternates inside this
                // single Execute.
                builder.AllowSamePassReadWrite(blackboard.Scratch.GTAODenoisePing);
                builder.Write(blackboard.Scratch.GTAODenoisePing, RGWriteUsage::ShaderImage);
                [[maybe_unused]] const auto pingRead = builder.Read(blackboard.Scratch.GTAODenoisePing, RGReadUsage::ShaderImage);
            }
            else
            {
                builder.Write(blackboard.Scratch.GTAODenoisePing, RGWriteUsage::ShaderImage);
            }
        }

        if (willDispatchDenoise && blackboard.Scratch.GTAODenoisePong.IsValid())
        {
            m_SelectedDenoisePongTexture = blackboard.Scratch.GTAODenoisePong;
            // Intra-pass denoise ping-pong (see GTAODenoisePing above).
            builder.AllowSamePassReadWrite(blackboard.Scratch.GTAODenoisePong);
            builder.Write(blackboard.Scratch.GTAODenoisePong, RGWriteUsage::ShaderImage);
            [[maybe_unused]] const auto pongRead = builder.Read(blackboard.Scratch.GTAODenoisePong, RGReadUsage::ShaderImage);
        }
    }

    GTAORenderPass::~GTAORenderPass() = default;

    void GTAORenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;
        m_Width = spec.Width;
        m_Height = spec.Height;

        // Initialize HZB generator
        m_HZBGenerator.Initialize();

        // Load compute shaders
        m_GTAOShader = ComputeShader::Create("assets/shaders/compute/GTAO.comp");
        m_DenoiseShader = ComputeShader::Create("assets/shaders/compute/GTAO_Denoise.comp");

        // Generate Hilbert LUT
        GenerateHilbertLUT();

        // Initialize HZB for current viewport
        m_HZBGenerator.Resize(m_Width, m_Height);

        OLO_CORE_INFO("GTAORenderPass: Initialized at {}x{}", m_Width, m_Height);
    }

    void GTAORenderPass::GenerateHilbertLUT()
    {
        OLO_PROFILE_FUNCTION();

        TextureSpecification lutSpec;
        lutSpec.Width = HILBERT_SIZE;
        lutSpec.Height = HILBERT_SIZE;
        lutSpec.Format = ImageFormat::R16UI;
        lutSpec.GenerateMips = false;
        lutSpec.MipLevels = 1;

        m_HilbertLUT = Texture2D::Create(lutSpec);

        // Generate LUT data
        std::vector<u16> data(HILBERT_SIZE * HILBERT_SIZE);
        for (u32 y = 0; y < HILBERT_SIZE; ++y)
        {
            for (u32 x = 0; x < HILBERT_SIZE; ++x)
            {
                data[y * HILBERT_SIZE + x] = HilbertIndex(x, y);
            }
        }

        m_HilbertLUT->SetData(data.data(), static_cast<u32>(data.size() * sizeof(u16)));
    }

    void GTAORenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Settings.GTAOEnabled || m_Settings.ActiveAOTechnique != AOTechnique::GTAO || !IsReadyForExecution())
        {
            return;
        }

        // Phase F slice 37 — self-resolving SceneDepth and SceneNormals: look
        // up directly from the render graph blackboard so no per-frame
        // side-channel setter calls are needed from EndScene().
        u32 depthID = 0;
        u32 normalsID = 0;
        if (m_SelectedSceneDepthTexture.IsValid())
            depthID = context.ResolveTexture(m_SelectedSceneDepthTexture);
        if (m_SelectedSceneNormalsTexture.IsValid())
            normalsID = context.ResolveTexture(m_SelectedSceneNormalsTexture);
        if (depthID == 0 || normalsID == 0)
        {
            return;
        }

        // Phase D / H follow-up: resolve the GTAO edge scratch texture from
        // the transient pool only. The execute path no longer falls back to
        // an owned edge texture.
        u32 edgeTexID = 0;
        u32 aoOutputTexID = 0;
        u32 denoisePingTexID = 0;
        u32 denoisePongTexID = 0;
        if (m_SelectedAOOutputTexture.IsValid())
            aoOutputTexID = context.ResolveTexture(m_SelectedAOOutputTexture);
        if (m_SelectedEdgeTexture.IsValid())
            edgeTexID = context.ResolveTexture(m_SelectedEdgeTexture);
        if (m_SelectedDenoisePingTexture.IsValid())
            denoisePingTexID = context.ResolveTexture(m_SelectedDenoisePingTexture);

        const bool willDispatchDenoise = m_Settings.GTAODenoiseEnabled && m_Settings.GTAODenoisePasses > 0;
        if (willDispatchDenoise && m_SelectedDenoisePongTexture.IsValid())
            denoisePongTexID = context.ResolveTexture(m_SelectedDenoisePongTexture);

        if (edgeTexID == 0 || aoOutputTexID == 0 || denoisePingTexID == 0)
            return;
        if (willDispatchDenoise && denoisePongTexID == 0)
            return;

        // Phase D / H follow-up: resolve transient HZB scratch from the render
        // graph and require it to exist for execution.
        u32 transientHZBID = 0;
        if (m_SelectedHZBDepthTexture.IsValid())
            transientHZBID = context.ResolveTexture(m_SelectedHZBDepthTexture);
        if (transientHZBID == 0)
        {
            m_HZBGenerator.ClearExternalHZBTexture();
            return;
        }
        m_HZBGenerator.SetExternalHZBTexture(transientHZBID, m_HZBGenerator.GetMipCount());

        // (The previous "log on input change" diagnostic was dropped: the AO
        // output is double-buffered, so the texture ID flips every frame and
        // the dedup never holds — it fired ~60 times per second. If you need
        // to trace AO inputs again, drop a one-shot OLO_CORE_TRACE here.)

        // Step 1: Generate HZB from scene depth
        m_HZBGenerator.Generate(depthID);

        // Step 2: Upload GTAO uniforms
        UploadGTAOUniforms();

        // Step 3: Dispatch GTAO main pass
        DispatchGTAO(denoisePingTexID, normalsID, edgeTexID);

        // Step 4: Denoise (if enabled)
        if (willDispatchDenoise)
        {
            DispatchDenoise(edgeTexID, denoisePingTexID, denoisePongTexID);
        }

        const u32 finalAOTextureID = (willDispatchDenoise && (m_Settings.GTAODenoisePasses % 2 != 0))
                                         ? denoisePongTexID
                                         : denoisePingTexID;
        if (finalAOTextureID != 0 && finalAOTextureID != aoOutputTexID)
        {
            RenderCommand::MemoryBarrier(
                MemoryBarrierFlags::ShaderImageAccess |
                MemoryBarrierFlags::TextureFetch |
                MemoryBarrierFlags::TextureUpdate);

            glCopyImageSubData(
                finalAOTextureID, GL_TEXTURE_2D, 0, 0, 0, 0,
                aoOutputTexID, GL_TEXTURE_2D, 0, 0, 0, 0,
                static_cast<GLsizei>(m_Width), static_cast<GLsizei>(m_Height), 1);
        }
    }

    void GTAORenderPass::UploadGTAOUniforms()
    {
        if (!m_GTAOUBO || !m_GPUData)
        {
            return;
        }

        f32 projScale00 = m_Projection[0][0];
        f32 projScale11 = m_Projection[1][1];

        // NDCToView: unproject from normalized screen [0,1] to view-space XY
        m_GPUData->NDCToViewMul = glm::vec2(2.0f / projScale00, -2.0f / projScale11);
        m_GPUData->NDCToViewAdd = glm::vec2(-1.0f / projScale00, 1.0f / projScale11);

        f32 pixelSizeX = 1.0f / static_cast<f32>(m_Width);
        f32 pixelSizeY = 1.0f / static_cast<f32>(m_Height);
        m_GPUData->NDCToViewMul_x_PixelSize = m_GPUData->NDCToViewMul * glm::vec2(pixelSizeX, pixelSizeY);

        m_GPUData->EffectRadius = m_Settings.GTAORadius;
        m_GPUData->FinalValuePower = m_Settings.GTAOPower;
        m_GPUData->EffectFalloffRange = m_Settings.GTAOFalloffRange;
        m_GPUData->SampleDistributionPower = m_Settings.GTAOSampleDistribution;
        m_GPUData->ThinOccluderCompensation = m_Settings.GTAOThinCompensation;
        m_GPUData->DepthMIPSamplingOffset = m_Settings.GTAODepthMipOffset;
        m_GPUData->DenoiseBlurBeta = m_Settings.GTAODenoiseBeta;

        // Depth linearization: proj[2][2] and proj[3][2]
        m_GPUData->DepthLinearizeA = m_Projection[2][2];
        m_GPUData->DepthLinearizeB = m_Projection[3][2];

        m_GPUData->HZBUVFactor = m_HZBGenerator.GetUVFactor();
        m_GPUData->ScreenWidth = static_cast<i32>(m_Width);
        m_GPUData->ScreenHeight = static_cast<i32>(m_Height);

        m_GPUData->DenoiseEnabled = m_Settings.GTAODenoiseEnabled ? 1 : 0;
        m_GPUData->DenoisePasses = m_Settings.GTAODenoisePasses;
        m_GPUData->DebugView = m_Settings.GTAODebugView ? 1 : 0;

        // View matrix: transforms world-space GBuffer normals to view-space
        m_GPUData->ViewMatrix = m_ViewMatrix;

        m_GTAOUBO->SetData(m_GPUData, UBOStructures::GTAOUBO::GetSize());
        m_GTAOUBO->Bind();
    }

    void GTAORenderPass::DispatchGTAO(u32 aoOutputTextureID, u32 normalsTextureID, u32 edgeTexID)
    {
        OLO_PROFILE_FUNCTION();

        m_GTAOShader->Bind();

        // Bind output images
        RenderCommand::BindImageTexture(0, aoOutputTextureID, 0, false, 0, GL_WRITE_ONLY, GL_R8);
        RenderCommand::BindImageTexture(1, edgeTexID, 0, false, 0, GL_WRITE_ONLY, GL_R8);

        // Bind inputs
        u32 hzbID = m_HZBGenerator.GetHZBTextureID();
        RenderCommand::BindTexture(GTAO_HZB_TEXTURE_SLOT, hzbID);

        RenderCommand::BindTexture(GTAO_NORMALS_TEXTURE_SLOT, normalsTextureID);

        RenderCommand::BindTexture(GTAO_HILBERT_TEXTURE_SLOT, m_HilbertLUT->GetRendererID());

        // Dispatch 16×16 workgroups
        u32 groupsX = (m_Width + 15) / 16;
        u32 groupsY = (m_Height + 15) / 16;
        RenderCommand::DispatchCompute(groupsX, groupsY, 1);

        // Barrier: AO + edges must be visible for denoise
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureFetch);

        m_GTAOShader->Unbind();
    }

    void GTAORenderPass::DispatchDenoise(u32 edgeTexID, u32 pingTextureID, u32 pongTextureID)
    {
        OLO_PROFILE_FUNCTION();

        m_DenoiseShader->Bind();

        u32 groupsX = (m_Width + 7) / 8;
        u32 groupsY = (m_Height + 7) / 8;

        // Edge texture is always read-only
        RenderCommand::BindImageTexture(2, edgeTexID, 0, false, 0, GL_READ_ONLY, GL_R8);

        i32 passes = m_Settings.GTAODenoisePasses;
        bool readFromTex0 = true;

        for (i32 pass = 0; pass < passes; ++pass)
        {
            // Alternate horizontal/vertical
            m_DenoiseShader->SetInt("u_BlurHorizontal", (pass % 2 == 0) ? 1 : 0);

            if (readFromTex0)
            {
                RenderCommand::BindImageTexture(0, pingTextureID, 0, false, 0, GL_READ_ONLY, GL_R8);
                RenderCommand::BindImageTexture(1, pongTextureID, 0, false, 0, GL_WRITE_ONLY, GL_R8);
            }
            else
            {
                RenderCommand::BindImageTexture(0, pongTextureID, 0, false, 0, GL_READ_ONLY, GL_R8);
                RenderCommand::BindImageTexture(1, pingTextureID, 0, false, 0, GL_WRITE_ONLY, GL_R8);
            }

            RenderCommand::DispatchCompute(groupsX, groupsY, 1);
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureFetch);

            readFromTex0 = !readFromTex0;
        }

        m_DenoiseShader->Unbind();
    }
    void GTAORenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            return;
        }

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        m_Width = width;
        m_Height = height;

        m_HZBGenerator.Resize(width, height);
    }

    void GTAORenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            return;
        }

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        m_Width = width;
        m_Height = height;

        m_HZBGenerator.Resize(width, height);

        OLO_CORE_INFO("GTAORenderPass: Resized to {}x{}", width, height);
    }

    void GTAORenderPass::OnReset()
    {
        // Increment temporal noise index
        if (m_GPUData)
        {
            m_GPUData->NoiseIndex = (m_GPUData->NoiseIndex + 1) % 256;
        }
        m_SelectedSceneDepthTexture = {};
        m_SelectedSceneNormalsTexture = {};
        m_SelectedAOOutputTexture = {};
        m_SelectedEdgeTexture = {};
        m_SelectedHZBDepthTexture = {};
        m_SelectedDenoisePingTexture = {};
        m_SelectedDenoisePongTexture = {};
    }
} // namespace OloEngine
