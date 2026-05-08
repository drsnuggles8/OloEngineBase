#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"

#include "OloEngine/Renderer/Passes/AOApplyRenderPass.h"
#include "OloEngine/Renderer/Passes/DecalRenderPass.h"
#include "OloEngine/Renderer/Passes/GTAORenderPass.h"
#include "OloEngine/Renderer/Passes/OITPrepareRenderPass.h"
#include "OloEngine/Renderer/Passes/OITResolveRenderPass.h"
#include "OloEngine/Renderer/Passes/ParticleRenderPass.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/Passes/SSAORenderPass.h"
#include "OloEngine/Renderer/Passes/SSSRenderPass.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/ResourceHandle.h"

#include <algorithm>
#include <cmath>

namespace OloEngine::RenderPipelineBuilderInternal
{
    namespace
    {
        [[nodiscard]] bool HasOITContributor(ParticleRenderPass* particlePass,
                                             DecalRenderPass* decalPass)
        {
            return (particlePass && particlePass->HasRenderCallback()) ||
                   (decalPass && decalPass->GetCommandBucket().GetCommandCount() > 0);
        }
    } // namespace

    [[nodiscard]] auto CreateSSAONodeSetup(PostProcessSettings* postProcess,
                                           SSAORenderPass* ssaoPass) -> RenderPass::SetupCallback
    {
        return [postProcess, ssaoPass](RGBuilder& builder, FrameBlackboard& board)
        {
            if (!postProcess || postProcess->ActiveAOTechnique != AOTechnique::SSAO ||
                !postProcess->SSAOEnabled)
            {
                return;
            }

            if (board.SceneDepth.IsValid())
            {
                [[maybe_unused]] const auto sceneDepthRead = builder.Read(board.SceneDepth, RGReadUsage::ShaderSample);
            }
            if (board.SceneNormals.IsValid())
            {
                [[maybe_unused]] const auto sceneNormalsRead = builder.Read(board.SceneNormals, RGReadUsage::ShaderSample);
            }
            if (board.AOBuffer.IsValid())
                builder.Write(board.AOBuffer, RGWriteUsage::RenderTarget);

            if (const auto target = ssaoPass ? ssaoPass->GetTarget() : nullptr; target != nullptr)
            {
                const auto halfW = target->GetSpecification().Width;
                const auto halfH = target->GetSpecification().Height;
                if (halfW > 0 && halfH > 0)
                {
                    RGResourceDesc rawDesc;
                    rawDesc.Kind = ResourceHandle::Kind::Framebuffer;
                    rawDesc.Format = RGResourceFormat::RG16Float;
                    rawDesc.Width = halfW;
                    rawDesc.Height = halfH;
                    const auto rawHandle = builder.CreateFramebuffer("SSAORaw", rawDesc);
                    builder.Write(rawHandle, RGWriteUsage::RenderTarget);
                    [[maybe_unused]] const auto rawRead = builder.Read(rawHandle, RGReadUsage::RenderTargetRead);
                }
            }
        };
    }

    [[nodiscard]] auto CreateGTAONodeSetup(PostProcessSettings* postProcess,
                                           SceneRenderPass* scenePass) -> RenderPass::SetupCallback
    {
        return [postProcess, scenePass](RGBuilder& builder, FrameBlackboard& board)
        {
            if (!postProcess || postProcess->ActiveAOTechnique != AOTechnique::GTAO ||
                !postProcess->GTAOEnabled)
            {
                return;
            }

            if (board.SceneDepth.IsValid())
            {
                [[maybe_unused]] const auto sceneDepthRead = builder.Read(board.SceneDepth, RGReadUsage::ShaderSample);
            }
            if (board.SceneNormals.IsValid())
            {
                [[maybe_unused]] const auto sceneNormalsRead = builder.Read(board.SceneNormals, RGReadUsage::ShaderSample);
            }
            if (board.AOBuffer.IsValid())
                builder.Write(board.AOBuffer, RGWriteUsage::ShaderImage);

            if (const auto target = scenePass ? scenePass->GetTarget() : nullptr; target != nullptr)
            {
                const auto& targetSpec = target->GetSpecification();
                if (targetSpec.Width > 0 && targetSpec.Height > 0)
                {
                    const auto nextPow2 = [](u32 value)
                    {
                        u32 result = 1;
                        while (result < value)
                            result <<= 1u;
                        return result;
                    };

                    const auto hzbW = nextPow2(targetSpec.Width);
                    const auto hzbH = nextPow2(targetSpec.Height);
                    const auto mipCount = static_cast<u32>(std::floor(std::log2(static_cast<f64>(std::max(hzbW, hzbH))))) + 1u;

                    RGResourceDesc hzbDesc;
                    hzbDesc.Kind = ResourceHandle::Kind::Texture2D;
                    hzbDesc.Format = RGResourceFormat::R32Float;
                    hzbDesc.Width = hzbW;
                    hzbDesc.Height = hzbH;
                    hzbDesc.MipLevels = mipCount;

                    const auto hzbHandle = builder.CreateTexture(ResourceNames::HZBDepth, hzbDesc);
                    builder.Write(hzbHandle, RGWriteUsage::ShaderImage, RGSubresourceRange::Mip(0));
                    for (u32 mip = 1; mip < mipCount; ++mip)
                    {
                        [[maybe_unused]] const auto hzbMipRead =
                            builder.Read(hzbHandle, RGReadUsage::ShaderSample, RGSubresourceRange::Mip(mip - 1u));
                        builder.Write(hzbHandle, RGWriteUsage::ShaderImage, RGSubresourceRange::Mip(mip));
                    }
                    [[maybe_unused]] const auto hzbRead = builder.Read(hzbHandle, RGReadUsage::ShaderSample);
                }

                if (targetSpec.Width > 0 && targetSpec.Height > 0)
                {
                    RGResourceDesc edgeDesc;
                    edgeDesc.Kind = ResourceHandle::Kind::Texture2D;
                    edgeDesc.Format = RGResourceFormat::R8UNorm;
                    edgeDesc.Width = targetSpec.Width;
                    edgeDesc.Height = targetSpec.Height;
                    const auto edgeHandle = builder.CreateTexture("GTAOEdge", edgeDesc);
                    builder.Write(edgeHandle, RGWriteUsage::ShaderImage);
                    [[maybe_unused]] const auto edgeRead = builder.Read(edgeHandle, RGReadUsage::ShaderImage);
                }
            }
        };
    }

    [[nodiscard]] auto CreateParticleNodeSetup(RendererSettings* settings,
                                               ParticleRenderPass* particlePass) -> RenderPass::SetupCallback
    {
        return [settings, particlePass](RGBuilder& builder, FrameBlackboard& board)
        {
            if (!particlePass || !particlePass->HasRenderCallback())
                return;

            const bool oitEnabled = settings && (settings->Path == RenderingPath::Deferred) &&
                                    settings->Deferred.OITEnabled;

            if (oitEnabled)
            {
                if (board.OITAccum.IsValid())
                {
                    [[maybe_unused]] const auto oitAccumRead = builder.Read(board.OITAccum, RGReadUsage::RenderTargetRead);
                    builder.Write(board.OITAccum, RGWriteUsage::RenderTarget);
                }
                if (board.OITRevealage.IsValid())
                {
                    [[maybe_unused]] const auto oitRevealageRead = builder.Read(board.OITRevealage, RGReadUsage::RenderTargetRead);
                    builder.Write(board.OITRevealage, RGWriteUsage::RenderTarget);
                }
            }
            else if (board.SceneColor.IsValid())
            {
                builder.Write(board.SceneColor, RGWriteUsage::RenderTarget);
            }
        };
    }

    [[nodiscard]] auto CreateOITPrepareNodeSetup(RendererSettings* settings,
                                                 ParticleRenderPass* particlePass,
                                                 DecalRenderPass* decalPass) -> RenderPass::SetupCallback
    {
        return [settings, particlePass, decalPass](RGBuilder& builder, FrameBlackboard& board)
        {
            const bool oitEnabled = settings && (settings->Path == RenderingPath::Deferred) &&
                                    settings->Deferred.OITEnabled;
            if (!oitEnabled || !HasOITContributor(particlePass, decalPass))
                return;

            if (board.SceneColor.IsValid())
            {
                [[maybe_unused]] const auto sceneColorRead = builder.Read(board.SceneColor, RGReadUsage::RenderTargetRead);
            }

            if (board.OITAccum.IsValid())
                builder.Write(board.OITAccum, RGWriteUsage::Clear);
            if (board.OITRevealage.IsValid())
                builder.Write(board.OITRevealage, RGWriteUsage::Clear);
        };
    }

    [[nodiscard]] auto CreateOITResolveNodeSetup(RendererSettings* settings,
                                                 ParticleRenderPass* particlePass,
                                                 DecalRenderPass* decalPass) -> RenderPass::SetupCallback
    {
        return [settings, particlePass, decalPass](RGBuilder& builder, FrameBlackboard& board)
        {
            const bool oitEnabled = settings && (settings->Path == RenderingPath::Deferred) &&
                                    settings->Deferred.OITEnabled;
            if (!oitEnabled || !HasOITContributor(particlePass, decalPass))
                return;

            if (board.OITAccum.IsValid())
            {
                [[maybe_unused]] const auto oitAccumRead = builder.Read(board.OITAccum, RGReadUsage::RenderTargetRead);
            }
            if (board.OITRevealage.IsValid())
            {
                [[maybe_unused]] const auto oitRevealageRead = builder.Read(board.OITRevealage, RGReadUsage::RenderTargetRead);
            }

            if (board.SceneColor.IsValid())
            {
                [[maybe_unused]] const auto sceneColorRead = builder.Read(board.SceneColor, RGReadUsage::RenderTargetRead);
                builder.Write(board.SceneColor, RGWriteUsage::RenderTarget);
            }
        };
    }

    [[nodiscard]] auto CreateSSSNodeSetup(SnowSettings* snow,
                                          SSSRenderPass* sssPass) -> RenderPass::SetupCallback
    {
        return [snow, sssPass](RGBuilder& builder, FrameBlackboard& board)
        {
            if (!snow || !snow->SSSBlurEnabled || !sssPass || !sssPass->IsReadyForExecution() || !board.SSSColor.IsValid())
                return;

            if (board.SceneColor.IsValid())
            {
                [[maybe_unused]] const auto sceneColorRead = builder.Read(board.SceneColor, RGReadUsage::RenderTargetRead);
            }
            builder.Write(board.SSSColor, RGWriteUsage::RenderTarget);
        };
    }

    [[nodiscard]] auto CreateAOApplyNodeSetup(PostProcessSettings* postProcess) -> RenderPass::SetupCallback
    {
        return [postProcess](RGBuilder& builder, FrameBlackboard& board)
        {
            if (!postProcess)
                return;

            const bool aoApplyEnabled =
                (postProcess->ActiveAOTechnique == AOTechnique::SSAO && postProcess->SSAOEnabled) ||
                (postProcess->ActiveAOTechnique == AOTechnique::GTAO && postProcess->GTAOEnabled);
            if (!aoApplyEnabled || !board.AOApplyColor.IsValid() || !board.AOBuffer.IsValid() || !board.SceneDepth.IsValid())
                return;

            ReadFirstValidFramebuffer(builder, board.SSSColor, board.SceneColor);
            [[maybe_unused]] const auto aoBufferRead = builder.Read(board.AOBuffer, RGReadUsage::ShaderSample);
            [[maybe_unused]] const auto sceneDepthRead = builder.Read(board.SceneDepth, RGReadUsage::ShaderSample);
            builder.Write(board.AOApplyColor, RGWriteUsage::RenderTarget);
        };
    }

    void RegisterTransparencyAndAONodes(RenderGraph& graph,
                                        const TransparencyAOStageInputs& inputs)
    {
        OLO_CORE_ASSERT(inputs.Passes, "RegisterTransparencyAndAONodes requires pass inputs");
        OLO_CORE_ASSERT(inputs.Runtime, "RegisterTransparencyAndAONodes requires runtime inputs");
        graph.AddNode(PrepareGraphPass("ParticlePass",
                           inputs.Passes->Particle,
                           CreateParticleNodeSetup(inputs.Runtime->Renderer, inputs.Passes->Particle)));
        graph.AddNode(PrepareGraphPass("OITPreparePass",
                           inputs.Passes->OITPrepare,
                           CreateOITPrepareNodeSetup(inputs.Runtime->Renderer,
                                     inputs.Passes->Particle,
                                     inputs.Passes->Decal)));
        graph.AddNode(PrepareGraphPass("OITResolvePass",
                           inputs.Passes->OITResolve,
                           CreateOITResolveNodeSetup(inputs.Runtime->Renderer,
                                     inputs.Passes->Particle,
                                     inputs.Passes->Decal)));
        graph.AddNode(PrepareGraphPass("SSSPass",
                           inputs.Passes->SSS,
                           CreateSSSNodeSetup(inputs.Runtime->Snow, inputs.Passes->SSS)));

        if (inputs.Runtime->PostProcess)
        {
            switch (inputs.Runtime->PostProcess->ActiveAOTechnique)
            {
                case AOTechnique::SSAO:
                    graph.AddNode(PrepareGraphPass("SSAOPass",
                                                   inputs.Passes->SSAO,
                                                   CreateSSAONodeSetup(inputs.Runtime->PostProcess, inputs.Passes->SSAO)));
                    break;
                case AOTechnique::GTAO:
                    graph.AddNode(PrepareGraphPass("GTAOPass",
                                                   inputs.Passes->GTAO,
                                                   CreateGTAONodeSetup(inputs.Runtime->PostProcess, inputs.Passes->Scene)));
                    break;
                case AOTechnique::None:
                    break;
            }
        }

        graph.AddNode(PrepareGraphPass("AOApplyPass",
                                       inputs.Passes->AOApply,
                                       CreateAOApplyNodeSetup(inputs.Runtime->PostProcess)));
    }
} // namespace OloEngine::RenderPipelineBuilderInternal
