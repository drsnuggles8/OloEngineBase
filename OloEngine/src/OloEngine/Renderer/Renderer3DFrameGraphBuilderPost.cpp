#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3DFrameGraphBuilderInternal.h"

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

namespace OloEngine::Renderer3DFrameGraphBuilderInternal
{
    [[nodiscard]] auto CreateBloomNodeSetup(PostProcessSettings* postProcess,
                                            SceneRenderPass* scenePass) -> PassGraphNode::SetupCallback
    {
        return [postProcess, scenePass](RGBuilder& builder, FrameBlackboard& board)
        {
            if (!postProcess || !postProcess->BloomEnabled)
                return;

            ReadFirstValidFramebuffer(builder,
                                      board.AOApplyColor,
                                      board.SSSColor,
                                      board.PostProcessColor,
                                      board.SceneColor);
            if (board.BloomColor.IsValid())
                builder.Write(board.BloomColor, RGWriteUsage::RenderTarget);

            if (const auto target = scenePass ? scenePass->GetTarget() : nullptr; target != nullptr)
            {
                auto mipW = target->GetSpecification().Width / 2;
                auto mipH = target->GetSpecification().Height / 2;
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

    [[nodiscard]] auto CreateDOFNodeSetup(PostProcessSettings* postProcess) -> PassGraphNode::SetupCallback
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

    [[nodiscard]] auto CreateMotionBlurNodeSetup(PostProcessSettings* postProcess) -> PassGraphNode::SetupCallback
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

    [[nodiscard]] auto CreateTAANodeSetup(PostProcessSettings* postProcess) -> PassGraphNode::SetupCallback
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

    [[nodiscard]] auto CreatePrecipitationNodeSetup(PrecipitationSettings* precipitation) -> PassGraphNode::SetupCallback
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

    [[nodiscard]] auto CreateFogNodeSetup(FogSettings* fog) -> PassGraphNode::SetupCallback
    {
        return [fog](RGBuilder& builder, FrameBlackboard& board)
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
            if (board.FogColor.IsValid())
                builder.Write(board.FogColor, RGWriteUsage::RenderTarget);
        };
    }

    [[nodiscard]] auto CreateChromAberrationNodeSetup(PostProcessSettings* postProcess) -> PassGraphNode::SetupCallback
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

    [[nodiscard]] auto CreateColorGradingNodeSetup(PostProcessSettings* postProcess) -> PassGraphNode::SetupCallback
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

    [[nodiscard]] auto CreateToneMapNodeSetup() -> PassGraphNode::SetupCallback
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

    [[nodiscard]] auto CreateVignetteNodeSetup(PostProcessSettings* postProcess) -> PassGraphNode::SetupCallback
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

    [[nodiscard]] auto CreateFXAANodeSetup(PostProcessSettings* postProcess) -> PassGraphNode::SetupCallback
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

    [[nodiscard]] auto CreateSelectionOutlineNodeSetup(const bool* enableSelectionOutline,
                                                       SelectionOutlineRenderPass* selectionOutlinePass) -> PassGraphNode::SetupCallback
    {
        return [enableSelectionOutline, selectionOutlinePass](RGBuilder& builder, FrameBlackboard& board)
        {
            if (!IsSelectionOutlineEnabled(enableSelectionOutline))
                return;

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

            if (const auto target = selectionOutlinePass ? selectionOutlinePass->GetTarget() : nullptr; target != nullptr)
            {
                const auto w = target->GetSpecification().Width;
                const auto h = target->GetSpecification().Height;
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

    [[nodiscard]] auto CreateUICompositeNodeSetup(const bool* enableSelectionOutline) -> PassGraphNode::SetupCallback
    {
        return [enableSelectionOutline](RGBuilder& builder, FrameBlackboard& board)
        {
            if (board.UIComposite.IsValid())
                builder.Write(board.UIComposite, RGWriteUsage::RenderTarget);

            if (IsSelectionOutlineEnabled(enableSelectionOutline))
            {
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
            }
            else
            {
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
                                          board.PostProcessColor,
                                          board.SceneColor);
            }
        };
    }

    [[nodiscard]] auto CreateFinalNodeSetup() -> PassGraphNode::SetupCallback
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
                                  const Renderer3DFrameGraphInputs& inputs)
    {
        graph.AddNode(MakePassNode("BloomPass",
                                   inputs.BloomPass,
                                   CreateBloomNodeSetup(inputs.PostProcess, inputs.ScenePass)));
        graph.AddNode(MakePassNode("DOFPass",
                                   inputs.DOFPass,
                                   CreateDOFNodeSetup(inputs.PostProcess)));
        graph.AddNode(MakePassNode("MotionBlurPass",
                                   inputs.MotionBlurPass,
                                   CreateMotionBlurNodeSetup(inputs.PostProcess)));
        graph.AddNode(MakePassNode("TAAPass",
                                   inputs.TAAPass,
                                   CreateTAANodeSetup(inputs.PostProcess)));
        graph.AddNode(MakePassNode("PrecipitationPass",
                                   inputs.PrecipitationPass,
                                   CreatePrecipitationNodeSetup(inputs.Precipitation)));
        graph.AddNode(MakePassNode("FogPass",
                                   inputs.FogPass,
                                   CreateFogNodeSetup(inputs.Fog)));
        graph.AddNode(MakePassNode("ChromAberrationPass",
                                   inputs.ChromAberrationPass,
                                   CreateChromAberrationNodeSetup(inputs.PostProcess)));
        graph.AddNode(MakePassNode("ColorGradingPass",
                                   inputs.ColorGradingPass,
                                   CreateColorGradingNodeSetup(inputs.PostProcess)));
        graph.AddNode(MakePassNode("ToneMapPass",
                                   inputs.ToneMapPass,
                                   CreateToneMapNodeSetup()));
        graph.AddNode(MakePassNode("VignettePass",
                                   inputs.VignettePass,
                                   CreateVignetteNodeSetup(inputs.PostProcess)));
        if (inputs.FXAAPass)
        {
            graph.AddNode(MakePassNode("FXAAPass",
                                       inputs.FXAAPass,
                                       CreateFXAANodeSetup(inputs.PostProcess)));
        }
        if (IsSelectionOutlineEnabled(inputs.EnableSelectionOutline) && inputs.SelectionOutlinePass)
        {
            graph.AddNode(MakePassNode("SelectionOutlinePass",
                                       inputs.SelectionOutlinePass,
                                       CreateSelectionOutlineNodeSetup(inputs.EnableSelectionOutline,
                                                                       inputs.SelectionOutlinePass)));
        }
        graph.AddNode(MakePassNode("UICompositePass",
                                   inputs.UICompositePass,
                                   CreateUICompositeNodeSetup(inputs.EnableSelectionOutline)));
        graph.AddNode(MakePassNode("FinalPass",
                                   inputs.FinalPass,
                                   CreateFinalNodeSetup(),
                                   RenderGraphNodeFlags::Graphics | RenderGraphNodeFlags::Present));
    }
} // namespace OloEngine::Renderer3DFrameGraphBuilderInternal
