#pragma once

#include "OloEngine/Core/Assert.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/PassGraphNode.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/Renderer3DFrameGraphBuilder.h"

#include <string>
#include <string_view>
#include <utility>

namespace OloEngine::Renderer3DFrameGraphBuilderInternal
{
    template<typename T>
    [[nodiscard]] auto BorrowRef(T* instance) -> Ref<T>
    {
        return instance ? Ref<T>(instance) : Ref<T>{};
    }

    [[nodiscard]] inline auto IsSelectionOutlineEnabled(const bool* enabled) -> bool
    {
        return enabled != nullptr && *enabled;
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

    inline void AddExistingNode(RenderGraph& graph, PassGraphNode* node)
    {
        OLO_CORE_ASSERT(node, "BuildRenderer3DFrameGraph requires a valid render-stream node");
        graph.AddNode(BorrowRef(node));
    }

    template<typename TPass>
    [[nodiscard]] auto MakePassNode(std::string_view name,
                                    TPass* pass,
                                    PassGraphNode::SetupCallback setup,
                                    const RenderGraphNodeFlags flags = RenderGraphNodeFlags::Graphics) -> Ref<PassGraphNode>
    {
        const auto passRef = BorrowRef(pass);
        OLO_CORE_ASSERT(passRef, "BuildRenderer3DFrameGraph requires a valid render pass");
        return Ref<PassGraphNode>::Create(std::string(name),
                                          passRef.template As<RenderPass>(),
                                          std::move(setup),
                                          flags);
    }

    [[nodiscard]] auto CreateShadowNodeSetup() -> PassGraphNode::SetupCallback;
    [[nodiscard]] auto CreateSSAONodeSetup(PostProcessSettings* postProcess,
                                           SSAORenderPass* ssaoPass) -> PassGraphNode::SetupCallback;
    [[nodiscard]] auto CreateGTAONodeSetup(PostProcessSettings* postProcess,
                                           SceneRenderPass* scenePass) -> PassGraphNode::SetupCallback;
    [[nodiscard]] auto CreateDeferredOpaqueDecalNodeSetup(RendererSettings* settings,
                                                          DecalRenderPass* decalPass) -> PassGraphNode::SetupCallback;
    [[nodiscard]] auto CreateDeferredLightingNodeSetup(RendererSettings* settings) -> PassGraphNode::SetupCallback;
    [[nodiscard]] auto CreateParticleNodeSetup(RendererSettings* settings,
                                               ParticleRenderPass* particlePass) -> PassGraphNode::SetupCallback;
    [[nodiscard]] auto CreateOITResolveNodeSetup(RendererSettings* settings,
                                                 ParticleRenderPass* particlePass,
                                                 WaterRenderPass* waterPass,
                                                 DecalRenderPass* decalPass) -> PassGraphNode::SetupCallback;
    [[nodiscard]] auto CreateSSSNodeSetup(SnowSettings* snow,
                                          SSSRenderPass* sssPass) -> PassGraphNode::SetupCallback;
    [[nodiscard]] auto CreateAOApplyNodeSetup(PostProcessSettings* postProcess) -> PassGraphNode::SetupCallback;
    [[nodiscard]] auto CreateBloomNodeSetup(PostProcessSettings* postProcess,
                                            SceneRenderPass* scenePass) -> PassGraphNode::SetupCallback;
    [[nodiscard]] auto CreateDOFNodeSetup(PostProcessSettings* postProcess) -> PassGraphNode::SetupCallback;
    [[nodiscard]] auto CreateMotionBlurNodeSetup(PostProcessSettings* postProcess) -> PassGraphNode::SetupCallback;
    [[nodiscard]] auto CreateTAANodeSetup(PostProcessSettings* postProcess) -> PassGraphNode::SetupCallback;
    [[nodiscard]] auto CreatePrecipitationNodeSetup(PrecipitationSettings* precipitation) -> PassGraphNode::SetupCallback;
    [[nodiscard]] auto CreateFogNodeSetup(FogSettings* fog) -> PassGraphNode::SetupCallback;
    [[nodiscard]] auto CreateChromAberrationNodeSetup(PostProcessSettings* postProcess) -> PassGraphNode::SetupCallback;
    [[nodiscard]] auto CreateColorGradingNodeSetup(PostProcessSettings* postProcess) -> PassGraphNode::SetupCallback;
    [[nodiscard]] auto CreateToneMapNodeSetup() -> PassGraphNode::SetupCallback;
    [[nodiscard]] auto CreateVignetteNodeSetup(PostProcessSettings* postProcess) -> PassGraphNode::SetupCallback;
    [[nodiscard]] auto CreateFXAANodeSetup(PostProcessSettings* postProcess) -> PassGraphNode::SetupCallback;
    [[nodiscard]] auto CreateSelectionOutlineNodeSetup(const bool* enableSelectionOutline,
                                                       SelectionOutlineRenderPass* selectionOutlinePass) -> PassGraphNode::SetupCallback;
    [[nodiscard]] auto CreateUICompositeNodeSetup(const bool* enableSelectionOutline) -> PassGraphNode::SetupCallback;
    [[nodiscard]] auto CreateFinalNodeSetup() -> PassGraphNode::SetupCallback;

    void RegisterRenderStreamNodes(RenderGraph& graph,
                                   const Renderer3DFrameGraphInputs& inputs,
                                   bool deferred);
    void RegisterSceneAndLightingNodes(RenderGraph& graph,
                                       const Renderer3DFrameGraphInputs& inputs,
                                       bool deferred);
    void RegisterTransparencyAndAONodes(RenderGraph& graph,
                                        const Renderer3DFrameGraphInputs& inputs);
    void RegisterPostProcessNodes(RenderGraph& graph,
                                  const Renderer3DFrameGraphInputs& inputs);
} // namespace OloEngine::Renderer3DFrameGraphBuilderInternal
