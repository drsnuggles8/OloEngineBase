#pragma once

#include "OloEngine/Core/Assert.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Passes/CommandBufferRenderPass.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/RenderPipelineBuilder.h"

#include <string>
#include <string_view>
#include <utility>

namespace OloEngine::RenderPipelineBuilderInternal
{
    struct RenderStreamStageInputs
    {
        const RenderPipelineNodeInputs* Nodes = nullptr;
        bool Deferred = false;
    };

    struct SceneLightingStageInputs
    {
        const RenderPipelinePassInputs* Passes = nullptr;
        RendererSettings* Renderer = nullptr;
        bool Deferred = false;
    };

    struct TransparencyAOStageInputs
    {
        const RenderPipelinePassInputs* Passes = nullptr;
        const RenderPipelineRuntimeInputs* Runtime = nullptr;
    };

    struct PostProcessStageInputs
    {
        const RenderPipelinePassInputs* Passes = nullptr;
        const RenderPipelineRuntimeInputs* Runtime = nullptr;
    };

    template<typename T>
    [[nodiscard]] auto BorrowRef(T* instance) -> Ref<T>
    {
        return instance ? Ref<T>(instance) : Ref<T>{};
    }

    [[nodiscard]] inline auto MakeRenderStreamStageInputs(const RenderPipelineInputs& inputs,
                                                          const bool deferred) -> RenderStreamStageInputs
    {
        return RenderStreamStageInputs{ .Nodes = &inputs.Nodes, .Deferred = deferred };
    }

    [[nodiscard]] inline auto MakeSceneLightingStageInputs(const RenderPipelineInputs& inputs,
                                                           const bool deferred) -> SceneLightingStageInputs
    {
        return SceneLightingStageInputs{ .Passes = &inputs.Passes, .Renderer = inputs.Runtime.Renderer, .Deferred = deferred };
    }

    [[nodiscard]] inline auto MakeTransparencyAOStageInputs(const RenderPipelineInputs& inputs) -> TransparencyAOStageInputs
    {
        return TransparencyAOStageInputs{ .Passes = &inputs.Passes, .Runtime = &inputs.Runtime };
    }

    [[nodiscard]] inline auto MakePostProcessStageInputs(const RenderPipelineInputs& inputs) -> PostProcessStageInputs
    {
        return PostProcessStageInputs{ .Passes = &inputs.Passes, .Runtime = &inputs.Runtime };
    }

    // Keep post-chain source selection compact and consistent: each
    // setup callback lists its preferred upstream framebuffers in order,
    // and this helper emits exactly one dependency read from the first
    // valid handle. That mirrors the runtime fallback policy used by the
    // fullscreen passes without open-coding the same if/else ladder in
    // every setup lambda.
    template<typename... THandles>
    inline void ReadFirstValidFramebuffer(RGBuilder& builder, const THandles&... handles)
    {
        bool readIssued = false;
        const auto tryRead = [&builder, &readIssued](const RGFramebufferHandle& handle)
        {
            if (readIssued || !handle.IsValid())
                return;

            [[maybe_unused]] const auto framebufferRead =
                builder.Read(handle, RGReadUsage::RenderTargetRead);
            readIssued = true;
        };

        (tryRead(handles), ...);
    }

    inline void AddExistingNode(RenderGraph& graph, CommandBufferRenderPass* node)
    {
        OLO_CORE_ASSERT(node, "BuildRenderPipelineGraph requires a valid render-stream pass");
        graph.AddNode(BorrowRef(node));
    }

    template<typename TPass>
    [[nodiscard]] auto PrepareGraphPass(std::string_view name,
                                        TPass* pass,
                                        RenderPass::SetupCallback setup) -> Ref<TPass>
    {
        auto passRef = BorrowRef(pass);
        OLO_CORE_ASSERT(passRef, "BuildRenderPipelineGraph requires a valid render pass");
        passRef->SetName(name);
        passRef->SetSetupCallback(std::move(setup));
        return passRef;
    }

    [[nodiscard]] auto CreateShadowNodeSetup() -> RenderPass::SetupCallback;
    [[nodiscard]] auto CreateSSAONodeSetup(PostProcessSettings* postProcess,
                                           SSAORenderPass* ssaoPass) -> RenderPass::SetupCallback;
    [[nodiscard]] auto CreateGTAONodeSetup(PostProcessSettings* postProcess,
                                           SceneRenderPass* scenePass) -> RenderPass::SetupCallback;
    [[nodiscard]] auto CreateDeferredOpaqueDecalNodeSetup(RendererSettings* settings,
                                                          DecalRenderPass* decalPass) -> RenderPass::SetupCallback;
    [[nodiscard]] auto CreateDeferredLightingNodeSetup(RendererSettings* settings) -> RenderPass::SetupCallback;
    [[nodiscard]] auto CreateParticleNodeSetup(RendererSettings* settings,
                                               ParticleRenderPass* particlePass) -> RenderPass::SetupCallback;
    [[nodiscard]] auto CreateOITPrepareNodeSetup(RendererSettings* settings,
                                                 ParticleRenderPass* particlePass,
                                                 DecalRenderPass* decalPass) -> RenderPass::SetupCallback;
    [[nodiscard]] auto CreateOITResolveNodeSetup(RendererSettings* settings,
                                                 ParticleRenderPass* particlePass,
                                                 DecalRenderPass* decalPass) -> RenderPass::SetupCallback;
    [[nodiscard]] auto CreateSSSNodeSetup(SnowSettings* snow,
                                          SSSRenderPass* sssPass) -> RenderPass::SetupCallback;
    [[nodiscard]] auto CreateAOApplyNodeSetup(PostProcessSettings* postProcess) -> RenderPass::SetupCallback;
    [[nodiscard]] auto CreateBloomNodeSetup(PostProcessSettings* postProcess,
                                            SceneRenderPass* scenePass) -> RenderPass::SetupCallback;
    [[nodiscard]] auto CreateDOFNodeSetup(PostProcessSettings* postProcess) -> RenderPass::SetupCallback;
    [[nodiscard]] auto CreateMotionBlurNodeSetup(PostProcessSettings* postProcess) -> RenderPass::SetupCallback;
    [[nodiscard]] auto CreateTAANodeSetup(PostProcessSettings* postProcess) -> RenderPass::SetupCallback;
    [[nodiscard]] auto CreatePrecipitationNodeSetup(PrecipitationSettings* precipitation) -> RenderPass::SetupCallback;
    [[nodiscard]] auto CreateFogNodeSetup(FogSettings* fog) -> RenderPass::SetupCallback;
    [[nodiscard]] auto CreateChromAberrationNodeSetup(PostProcessSettings* postProcess) -> RenderPass::SetupCallback;
    [[nodiscard]] auto CreateColorGradingNodeSetup(PostProcessSettings* postProcess) -> RenderPass::SetupCallback;
    [[nodiscard]] auto CreateToneMapNodeSetup() -> RenderPass::SetupCallback;
    [[nodiscard]] auto CreateVignetteNodeSetup(PostProcessSettings* postProcess) -> RenderPass::SetupCallback;
    [[nodiscard]] auto CreateFXAANodeSetup(PostProcessSettings* postProcess) -> RenderPass::SetupCallback;
    [[nodiscard]] auto CreateSelectionOutlineNodeSetup(SelectionOutlineRenderPass* selectionOutlinePass) -> RenderPass::SetupCallback;
    [[nodiscard]] auto CreateUICompositeNodeSetup() -> RenderPass::SetupCallback;
    [[nodiscard]] auto CreateFinalNodeSetup() -> RenderPass::SetupCallback;

    void RegisterRenderStreamNodes(RenderGraph& graph,
                                   const RenderStreamStageInputs& inputs);
    void RegisterSceneAndLightingNodes(RenderGraph& graph,
                                       const SceneLightingStageInputs& inputs);
    void RegisterTransparencyAndAONodes(RenderGraph& graph,
                                        const TransparencyAOStageInputs& inputs);
    void RegisterPostProcessNodes(RenderGraph& graph,
                                  const PostProcessStageInputs& inputs);
} // namespace OloEngine::RenderPipelineBuilderInternal
