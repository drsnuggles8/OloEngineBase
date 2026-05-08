#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"

#include "OloEngine/Renderer/Passes/BloomRenderPass.h"
#include "OloEngine/Renderer/Passes/ChromaticAberrationRenderPass.h"
#include "OloEngine/Renderer/Passes/ColorGradingRenderPass.h"
#include "OloEngine/Renderer/Passes/DOFRenderPass.h"
#include "OloEngine/Renderer/Passes/FinalRenderPass.h"
#include "OloEngine/Renderer/Passes/FogRenderPass.h"
#include "OloEngine/Renderer/Passes/FXAARenderPass.h"
#include "OloEngine/Renderer/Passes/MotionBlurRenderPass.h"
#include "OloEngine/Renderer/Passes/PrecipitationRenderPass.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/Passes/SelectionOutlineRenderPass.h"
#include "OloEngine/Renderer/Passes/TAARenderPass.h"
#include "OloEngine/Renderer/Passes/ToneMapRenderPass.h"
#include "OloEngine/Renderer/Passes/UICompositeRenderPass.h"
#include "OloEngine/Renderer/Passes/VignetteRenderPass.h"
#include "OloEngine/Renderer/PostProcessSettings.h"

namespace OloEngine::RenderPipelineBuilderInternal
{
    [[nodiscard]] auto CreateBloomNodeSetup(PostProcessSettings* postProcess,
                                            SceneRenderPass* scenePass) -> RenderPass::SetupCallback
    {
        return [postProcess, scenePass](RGBuilder& builder, FrameBlackboard& board)
        {
            if (!postProcess || !postProcess->BloomEnabled || !board.BloomColor.IsValid())
                return;

            ReadFirstValidFramebuffer(builder,
                                      board.AOApplyColor,
                                      board.SSSColor,
                                      board.PostProcessColor,
                                      board.SceneColor);
            if (board.BloomColor.IsValid())
                builder.Write(board.BloomColor, RGWriteUsage::RenderTarget);

            if (scenePass)
            {
                const auto& sceneSpec = scenePass->GetFramebufferSpecification();
                auto mipW = sceneSpec.Width / 2;
                auto mipH = sceneSpec.Height / 2;
                for (u32 i = 0; i < 5u; ++i)
                {
                    if (mipW < 2 || mipH < 2)
                        break;
                    RGResourceDesc mipDesc;
                    mipDesc.Kind = ResourceHandle::Kind::Framebuffer;
                    mipDesc.Format = RGResourceFormat::RGBA16Float;
                    mipDesc.Width = mipW;
                    mipDesc.Height = mipH;
                    const auto mipName = std::string("BloomMip") + std::to_string(i);
                    const auto mipHandle = builder.CreateFramebuffer(mipName, mipDesc);
                    builder.Write(mipHandle, RGWriteUsage::RenderTarget);
                    [[maybe_unused]] const auto mipRead = builder.Read(mipHandle, RGReadUsage::RenderTargetRead);
                    mipW /= 2;
                    mipH /= 2;
                }
            }
        };
    }

    [[nodiscard]] auto CreateDOFNodeSetup(PostProcessSettings* postProcess) -> RenderPass::SetupCallback
    {
        return [postProcess](RGBuilder& builder, FrameBlackboard& board)
        {
            if (!postProcess || !postProcess->DOFEnabled)
                return;

            ReadFirstValidFramebuffer(builder, board.BloomColor, board.PostProcessColor);
            if (board.SceneDepth.IsValid())
            {
                [[maybe_unused]] const auto sceneDepthRead = builder.Read(board.SceneDepth, RGReadUsage::ShaderSample);
            }
            if (board.DOFColor.IsValid())
                builder.Write(board.DOFColor, RGWriteUsage::RenderTarget);
        };
    }

    [[nodiscard]] auto CreateMotionBlurNodeSetup(PostProcessSettings* postProcess) -> RenderPass::SetupCallback
    {
        return [postProcess](RGBuilder& builder, FrameBlackboard& board)
        {
            if (!postProcess || !postProcess->MotionBlurEnabled)
                return;

            ReadFirstValidFramebuffer(builder,
                                      board.DOFColor,
                                      board.BloomColor,
                                      board.PostProcessColor);
            if (board.SceneDepth.IsValid())
            {
                [[maybe_unused]] const auto sceneDepthRead = builder.Read(board.SceneDepth, RGReadUsage::ShaderSample);
            }
            if (board.MotionBlurColor.IsValid())
                builder.Write(board.MotionBlurColor, RGWriteUsage::RenderTarget);
        };
    }

    [[nodiscard]] auto CreateTAANodeSetup(PostProcessSettings* postProcess) -> RenderPass::SetupCallback
    {
        return [postProcess](RGBuilder& builder, FrameBlackboard& board)
        {
            if (!postProcess || !postProcess->TAAEnabled)
                return;

            ReadFirstValidFramebuffer(builder,
                                      board.MotionBlurColor,
                                      board.DOFColor,
                                      board.BloomColor,
                                      board.PostProcessColor);
            if (board.SceneDepth.IsValid())
            {
                [[maybe_unused]] const auto sceneDepthRead = builder.Read(board.SceneDepth, RGReadUsage::ShaderSample);
            }
            if (board.Velocity.IsValid())
            {
                [[maybe_unused]] const auto velocityRead = builder.Read(board.Velocity, RGReadUsage::ShaderSample);
            }
            if (board.TAAHistory.IsValid())
            {
                [[maybe_unused]] const auto taaHistoryRead = builder.Read(board.TAAHistory, RGReadUsage::ShaderSample);
            }
            if (board.TAAColor.IsValid())
                builder.Write(board.TAAColor, RGWriteUsage::RenderTarget);
        };
    }

    [[nodiscard]] auto CreatePrecipitationNodeSetup(PrecipitationSettings* precipitation) -> RenderPass::SetupCallback
    {
        return [precipitation](RGBuilder& builder, FrameBlackboard& board)
        {
            const bool precipEnabled = precipitation && precipitation->Enabled &&
                                       (precipitation->ScreenStreaksEnabled ||
                                        precipitation->LensImpactsEnabled);
            if (!precipEnabled)
                return;

            ReadFirstValidFramebuffer(builder,
                                      board.TAAColor,
                                      board.MotionBlurColor,
                                      board.DOFColor,
                                      board.BloomColor,
                                      board.PostProcessColor);
            if (board.PrecipitationColor.IsValid())
                builder.Write(board.PrecipitationColor, RGWriteUsage::RenderTarget);
        };
    }

    [[nodiscard]] auto CreateFogNodeSetup(FogSettings* fog,
                                          FogRenderPass* fogPass) -> RenderPass::SetupCallback
    {
        return [fog, fogPass](RGBuilder& builder, FrameBlackboard& board)
        {
            if (!fog || !fog->Enabled)
                return;

            ReadFirstValidFramebuffer(builder,
                                      board.PrecipitationColor,
                                      board.TAAColor,
                                      board.MotionBlurColor,
                                      board.DOFColor,
                                      board.BloomColor,
                                      board.PostProcessColor);

            if (board.SceneDepth.IsValid())
            {
                [[maybe_unused]] const auto sceneDepthRead = builder.Read(board.SceneDepth, RGReadUsage::ShaderSample);
            }
            if (board.FogHistory.IsValid())
            {
                [[maybe_unused]] const auto fogHistoryRead = builder.Read(board.FogHistory, RGReadUsage::ShaderSample);
            }
            if (board.ShadowMapCSM.IsValid())
            {
                [[maybe_unused]] const auto shadowMapRead = builder.Read(board.ShadowMapCSM, RGReadUsage::ShaderSample);
            }

            if (fogPass)
            {
                const auto& targetSpec = fogPass->GetFramebufferSpecification();
                if (targetSpec.Width > 0 && targetSpec.Height > 0)
                {
                    RGResourceDesc fogHalfDesc;
                    fogHalfDesc.Kind = ResourceHandle::Kind::Framebuffer;
                    fogHalfDesc.Format = RGResourceFormat::RGBA16Float;
                    fogHalfDesc.Width = (targetSpec.Width + 1u) / 2u;
                    fogHalfDesc.Height = (targetSpec.Height + 1u) / 2u;

                    const auto fogHalfHandle = builder.CreateFramebuffer(ResourceNames::FogHalfRes, fogHalfDesc);
                    builder.Write(fogHalfHandle, RGWriteUsage::RenderTarget);
                    [[maybe_unused]] const auto fogHalfRead = builder.Read(fogHalfHandle, RGReadUsage::ShaderSample);
                }
            }

            if (board.FogColor.IsValid())
                builder.Write(board.FogColor, RGWriteUsage::RenderTarget);
        };
    }

    [[nodiscard]] auto CreateChromAberrationNodeSetup(PostProcessSettings* postProcess) -> RenderPass::SetupCallback
    {
        return [postProcess](RGBuilder& builder, FrameBlackboard& board)
        {
            if (!postProcess || !postProcess->ChromaticAberrationEnabled)
                return;

            ReadFirstValidFramebuffer(builder,
                                      board.FogColor,
                                      board.PrecipitationColor,
                                      board.TAAColor,
                                      board.MotionBlurColor,
                                      board.DOFColor,
                                      board.BloomColor,
                                      board.PostProcessColor);
            if (board.ChromAbColor.IsValid())
                builder.Write(board.ChromAbColor, RGWriteUsage::RenderTarget);
        };
    }

    [[nodiscard]] auto CreateColorGradingNodeSetup(PostProcessSettings* postProcess) -> RenderPass::SetupCallback
    {
        return [postProcess](RGBuilder& builder, FrameBlackboard& board)
        {
            if (!postProcess || !postProcess->ColorGradingEnabled)
                return;

            ReadFirstValidFramebuffer(builder,
                                      board.ChromAbColor,
                                      board.FogColor,
                                      board.PrecipitationColor,
                                      board.TAAColor,
                                      board.MotionBlurColor,
                                      board.DOFColor,
                                      board.BloomColor,
                                      board.PostProcessColor);
            if (board.ColorGradingColor.IsValid())
                builder.Write(board.ColorGradingColor, RGWriteUsage::RenderTarget);
        };
    }

    [[nodiscard]] auto CreateToneMapNodeSetup() -> RenderPass::SetupCallback
    {
        return [](RGBuilder& builder, FrameBlackboard& board)
        {
            ReadFirstValidFramebuffer(builder,
                                      board.ColorGradingColor,
                                      board.ChromAbColor,
                                      board.FogColor,
                                      board.PrecipitationColor,
                                      board.TAAColor,
                                      board.MotionBlurColor,
                                      board.DOFColor,
                                      board.BloomColor,
                                      board.PostProcessColor,
                                      board.SceneColor);
            if (board.ToneMapColor.IsValid())
                builder.Write(board.ToneMapColor, RGWriteUsage::RenderTarget);
        };
    }

    [[nodiscard]] auto CreateVignetteNodeSetup(PostProcessSettings* postProcess) -> RenderPass::SetupCallback
    {
        return [postProcess](RGBuilder& builder, FrameBlackboard& board)
        {
            if (!postProcess || !postProcess->VignetteEnabled)
                return;

            ReadFirstValidFramebuffer(builder,
                                      board.ToneMapColor,
                                      board.ColorGradingColor,
                                      board.ChromAbColor,
                                      board.FogColor,
                                      board.PrecipitationColor,
                                      board.TAAColor,
                                      board.MotionBlurColor,
                                      board.DOFColor,
                                      board.BloomColor,
                                      board.PostProcessColor);
            if (board.VignetteColor.IsValid())
                builder.Write(board.VignetteColor, RGWriteUsage::RenderTarget);
        };
    }

    [[nodiscard]] auto CreateFXAANodeSetup(PostProcessSettings* postProcess) -> RenderPass::SetupCallback
    {
        return [postProcess](RGBuilder& builder, FrameBlackboard& board)
        {
            if (!postProcess || !postProcess->FXAAEnabled)
                return;

            ReadFirstValidFramebuffer(builder,
                                      board.VignetteColor,
                                      board.ToneMapColor,
                                      board.ColorGradingColor,
                                      board.ChromAbColor,
                                      board.FogColor,
                                      board.PrecipitationColor,
                                      board.TAAColor,
                                      board.MotionBlurColor,
                                      board.DOFColor,
                                      board.BloomColor,
                                      board.PostProcessColor);

            if (board.FXAAColor.IsValid())
                builder.Write(board.FXAAColor, RGWriteUsage::RenderTarget);
        };
    }

    [[nodiscard]] auto CreateSelectionOutlineNodeSetup(SelectionOutlineRenderPass* selectionOutlinePass) -> RenderPass::SetupCallback
    {
        return [selectionOutlinePass](RGBuilder& builder, FrameBlackboard& board)
        {
            if (!selectionOutlinePass ||
                !selectionOutlinePass->IsReadyForExecution() ||
                !board.SelectionOutlineColor.IsValid())
                return;

            if (board.SceneColor.IsValid())
            {
                [[maybe_unused]] const auto sceneColorRead = builder.Read(board.SceneColor, RGReadUsage::RenderTargetRead);
            }

            ReadFirstValidFramebuffer(builder,
                                      board.FXAAColor,
                                      board.VignetteColor,
                                      board.ToneMapColor,
                                      board.ColorGradingColor,
                                      board.ChromAbColor,
                                      board.FogColor,
                                      board.PrecipitationColor,
                                      board.TAAColor,
                                      board.MotionBlurColor,
                                      board.DOFColor,
                                      board.BloomColor,
                                      board.PostProcessColor);

            if (board.SelectionOutlineColor.IsValid())
                builder.Write(board.SelectionOutlineColor, RGWriteUsage::RenderTarget);

            if (selectionOutlinePass)
            {
                const auto& outlineSpec = selectionOutlinePass->GetFramebufferSpecification();
                const auto w = outlineSpec.Width;
                const auto h = outlineSpec.Height;
                if (w > 0 && h > 0)
                {
                    RGResourceDesc jfaDesc;
                    jfaDesc.Kind = ResourceHandle::Kind::Framebuffer;
                    jfaDesc.Format = RGResourceFormat::RGBA32Float;
                    jfaDesc.Width = w;
                    jfaDesc.Height = h;

                    const auto pingHandle = builder.CreateFramebuffer("JFAPing", jfaDesc);
                    builder.Write(pingHandle, RGWriteUsage::RenderTarget);
                    [[maybe_unused]] const auto pingRead = builder.Read(pingHandle, RGReadUsage::RenderTargetRead);

                    const auto pongHandle = builder.CreateFramebuffer("JFAPong", jfaDesc);
                    builder.Write(pongHandle, RGWriteUsage::RenderTarget);
                    [[maybe_unused]] const auto pongRead = builder.Read(pongHandle, RGReadUsage::RenderTargetRead);
                }
            }
        };
    }

    [[nodiscard]] auto CreateUICompositeNodeSetup() -> RenderPass::SetupCallback
    {
        return [](RGBuilder& builder, FrameBlackboard& board)
        {
            if (board.UIComposite.IsValid())
                builder.Write(board.UIComposite, RGWriteUsage::RenderTarget);

            ReadFirstValidFramebuffer(builder,
                                      board.SelectionOutlineColor,
                                      board.FXAAColor,
                                      board.VignetteColor,
                                      board.ToneMapColor,
                                      board.ColorGradingColor,
                                      board.ChromAbColor,
                                      board.FogColor,
                                      board.PrecipitationColor,
                                      board.TAAColor,
                                      board.MotionBlurColor,
                                      board.DOFColor,
                                      board.BloomColor,
                                      board.PostProcessColor,
                                      board.SceneColor);
        };
    }

    [[nodiscard]] auto CreateFinalNodeSetup() -> RenderPass::SetupCallback
    {
        return [](RGBuilder& builder, FrameBlackboard& board)
        {
            ReadFirstValidFramebuffer(builder,
                                      board.UIComposite,
                                      board.SelectionOutlineColor,
                                      board.FXAAColor,
                                      board.VignetteColor,
                                      board.ToneMapColor,
                                      board.ColorGradingColor,
                                      board.ChromAbColor,
                                      board.FogColor,
                                      board.PrecipitationColor,
                                      board.TAAColor,
                                      board.MotionBlurColor,
                                      board.DOFColor,
                                      board.BloomColor,
                                      board.PostProcessColor,
                                      board.SceneColor);

            if (board.Backbuffer.IsValid())
                builder.Write(board.Backbuffer, RGWriteUsage::RenderTarget);
        };
    }

    void RegisterPostProcessNodes(RenderGraph& graph,
                                  const PostProcessStageInputs& inputs)
    {
        OLO_CORE_ASSERT(inputs.Passes, "RegisterPostProcessNodes requires pass inputs");
        OLO_CORE_ASSERT(inputs.Runtime, "RegisterPostProcessNodes requires runtime inputs");
        graph.AddNode(PrepareGraphPass("BloomPass",
                                       inputs.Passes->Bloom,
                                       CreateBloomNodeSetup(inputs.Runtime->PostProcess, inputs.Passes->Scene)));
        graph.AddNode(PrepareGraphPass("DOFPass",
                                       inputs.Passes->DOF,
                                       CreateDOFNodeSetup(inputs.Runtime->PostProcess)));
        graph.AddNode(PrepareGraphPass("MotionBlurPass",
                                       inputs.Passes->MotionBlur,
                                       CreateMotionBlurNodeSetup(inputs.Runtime->PostProcess)));
        graph.AddNode(PrepareGraphPass("TAAPass",
                                       inputs.Passes->TAA,
                                       CreateTAANodeSetup(inputs.Runtime->PostProcess)));
        graph.AddNode(PrepareGraphPass("PrecipitationPass",
                                       inputs.Passes->Precipitation,
                                       CreatePrecipitationNodeSetup(inputs.Runtime->Precipitation)));
        graph.AddNode(PrepareGraphPass("FogPass",
                                       inputs.Passes->Fog,
                                       CreateFogNodeSetup(inputs.Runtime->Fog,
                                                          inputs.Passes->Fog)));
        graph.AddNode(PrepareGraphPass("ChromAberrationPass",
                                       inputs.Passes->ChromAberration,
                                       CreateChromAberrationNodeSetup(inputs.Runtime->PostProcess)));
        graph.AddNode(PrepareGraphPass("ColorGradingPass",
                                       inputs.Passes->ColorGrading,
                                       CreateColorGradingNodeSetup(inputs.Runtime->PostProcess)));
        graph.AddNode(PrepareGraphPass("ToneMapPass",
                                       inputs.Passes->ToneMap,
                                       CreateToneMapNodeSetup()));
        graph.AddNode(PrepareGraphPass("VignettePass",
                                       inputs.Passes->Vignette,
                                       CreateVignetteNodeSetup(inputs.Runtime->PostProcess)));
        if (inputs.Passes->FXAA)
        {
            graph.AddNode(PrepareGraphPass("FXAAPass",
                                           inputs.Passes->FXAA,
                                           CreateFXAANodeSetup(inputs.Runtime->PostProcess)));
        }
        if (inputs.Passes->SelectionOutline)
        {
            graph.AddNode(PrepareGraphPass("SelectionOutlinePass",
                                           inputs.Passes->SelectionOutline,
                                           CreateSelectionOutlineNodeSetup(inputs.Passes->SelectionOutline)));
        }
        graph.AddNode(PrepareGraphPass("UICompositePass",
                                       inputs.Passes->UIComposite,
                                       CreateUICompositeNodeSetup()));
        graph.AddNode(PrepareGraphPass("FinalPass",
                                       inputs.Passes->Final,
                                       CreateFinalNodeSetup()));
    }
} // namespace OloEngine::RenderPipelineBuilderInternal
