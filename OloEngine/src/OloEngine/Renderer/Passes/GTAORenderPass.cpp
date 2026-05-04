#include "OloEnginePCH.h"
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

        // Create AO output textures
        CreateGTAOTextures(m_Width, m_Height);

        // Generate Hilbert LUT
        GenerateHilbertLUT();

        // Initialize HZB for current viewport
        m_HZBGenerator.Resize(m_Width, m_Height);

        // Resource-aware RDG: GTAO reads scene depth/normals and writes AOBuffer
        // consumed later by AOApplyPass. This allows the validator to derive
        // GTAOPass -> AOApplyPass ordering via RAW on AOBuffer.
        DeclareRead(ResourceNames::SceneDepth, ResourceHandle::Kind::Framebuffer);
        DeclareRead(ResourceNames::SceneNormals, ResourceHandle::Kind::Framebuffer);
        DeclareWrite(ResourceNames::AOBuffer, ResourceHandle::Kind::Texture2D);

        OLO_CORE_INFO("GTAORenderPass: Initialized at {}x{}", m_Width, m_Height);
    }

    void GTAORenderPass::CreateGTAOTextures(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            return;
        }

        // R8 AO textures (ping-pong for denoise)
        TextureSpecification aoSpec;
        aoSpec.Width = width;
        aoSpec.Height = height;
        aoSpec.Format = ImageFormat::R8;
        aoSpec.GenerateMips = false;
        aoSpec.MipLevels = 1;

        m_AOTexture0 = Texture2D::Create(aoSpec);
        m_AOTexture1 = Texture2D::Create(aoSpec);

        // R8 edge texture
        m_EdgeTexture = Texture2D::Create(aoSpec);
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
        for (u32 y = 0; y < HILBERT_SIZE; y++)
        {
            for (u32 x = 0; x < HILBERT_SIZE; x++)
            {
                data[y * HILBERT_SIZE + x] = HilbertIndex(x, y);
            }
        }

        m_HilbertLUT->SetData(data.data(), static_cast<u32>(data.size() * sizeof(u16)));
    }

    void GTAORenderPass::Execute()
    {
        RGCommandContext context;
        Execute(context);
    }

    void GTAORenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Settings.GTAOEnabled || m_Settings.ActiveAOTechnique != AOTechnique::GTAO || !m_GTAOShader || !m_GTAOShader->IsValid())
        {
            return;
        }

        // Phase F slice 37 — self-resolving SceneDepth and SceneNormals: look
        // up directly from the render graph blackboard so no per-frame
        // side-channel setter calls are needed from EndScene().
        u32 depthID = 0;
        u32 normalsID = 0;
        if (const auto* board = context.GetBlackboard())
        {
            depthID = context.ResolveTexture(board->SceneDepth);
            normalsID = context.ResolveTexture(board->SceneNormals);
        }
        if (depthID == 0 || normalsID == 0)
        {
            return;
        }

        {
            static u32 s_PrevDepthID = 0;
            static u32 s_PrevNormalsID = 0;
            static u32 s_PrevAOID = 0;
            const u32 aoID = GetGTAOTextureID();
            if (depthID != s_PrevDepthID || normalsID != s_PrevNormalsID || aoID != s_PrevAOID)
            {
                OLO_CORE_INFO("GTAORenderPass: inputs depthTex={}, normalsTex={}, outputAOTex={} ({}x{})",
                              depthID, normalsID, aoID, m_Width, m_Height);
                s_PrevDepthID = depthID;
                s_PrevNormalsID = normalsID;
                s_PrevAOID = aoID;
            }
        }

        // Step 1: Generate HZB from scene depth
        m_HZBGenerator.Generate(depthID);

        // Step 2: Upload GTAO uniforms
        UploadGTAOUniforms();

        // Step 3: Dispatch GTAO main pass
        DispatchGTAO(normalsID);

        // Step 4: Denoise (if enabled)
        if (m_Settings.GTAODenoiseEnabled && m_DenoiseShader && m_DenoiseShader->IsValid())
        {
            DispatchDenoise();
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

    void GTAORenderPass::DispatchGTAO(u32 normalsTextureID)
    {
        OLO_PROFILE_FUNCTION();

        m_GTAOShader->Bind();

        // Bind output images
        RenderCommand::BindImageTexture(0, m_AOTexture0->GetRendererID(), 0, false, 0, GL_WRITE_ONLY, GL_R8);
        RenderCommand::BindImageTexture(1, m_EdgeTexture->GetRendererID(), 0, false, 0, GL_WRITE_ONLY, GL_R8);

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

    void GTAORenderPass::DispatchDenoise()
    {
        OLO_PROFILE_FUNCTION();

        m_DenoiseShader->Bind();

        u32 groupsX = (m_Width + 7) / 8;
        u32 groupsY = (m_Height + 7) / 8;

        // Edge texture is always read-only
        RenderCommand::BindImageTexture(2, m_EdgeTexture->GetRendererID(), 0, false, 0, GL_READ_ONLY, GL_R8);

        i32 passes = m_Settings.GTAODenoisePasses;
        bool readFromTex0 = true;

        for (i32 pass = 0; pass < passes; pass++)
        {
            // Alternate horizontal/vertical
            m_DenoiseShader->SetInt("u_BlurHorizontal", (pass % 2 == 0) ? 1 : 0);

            if (readFromTex0)
            {
                RenderCommand::BindImageTexture(0, m_AOTexture0->GetRendererID(), 0, false, 0, GL_READ_ONLY, GL_R8);
                RenderCommand::BindImageTexture(1, m_AOTexture1->GetRendererID(), 0, false, 0, GL_WRITE_ONLY, GL_R8);
            }
            else
            {
                RenderCommand::BindImageTexture(0, m_AOTexture1->GetRendererID(), 0, false, 0, GL_READ_ONLY, GL_R8);
                RenderCommand::BindImageTexture(1, m_AOTexture0->GetRendererID(), 0, false, 0, GL_WRITE_ONLY, GL_R8);
            }

            RenderCommand::DispatchCompute(groupsX, groupsY, 1);
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureFetch);

            readFromTex0 = !readFromTex0;
        }

        m_DenoiseShader->Unbind();
    }

    u32 GTAORenderPass::GetGTAOTextureID() const
    {
        if (!m_AOTexture0)
        {
            return 0;
        }

        // After even number of denoise passes, result is in Texture0; odd → Texture1
        if (m_Settings.GTAODenoiseEnabled && (m_Settings.GTAODenoisePasses % 2 != 0))
        {
            return m_AOTexture1 ? m_AOTexture1->GetRendererID() : 0;
        }
        return m_AOTexture0->GetRendererID();
    }

    Ref<Framebuffer> GTAORenderPass::GetTarget() const
    {
        // Compute-only pass — no framebuffer target.
        return nullptr;
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

        CreateGTAOTextures(width, height);
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

        // Recreate textures at new size
        if (m_AOTexture0)
        {
            m_AOTexture0->Resize(width, height);
        }
        if (m_AOTexture1)
        {
            m_AOTexture1->Resize(width, height);
        }
        if (m_EdgeTexture)
        {
            m_EdgeTexture->Resize(width, height);
        }

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
    }
} // namespace OloEngine
