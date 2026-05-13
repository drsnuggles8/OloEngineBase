#pragma once

#include "OloEngine/Core/Assert.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
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
        const RenderPipelinePassInputs* Passes = nullptr;
        bool Deferred = false;
    };

    struct SceneLightingStageInputs
    {
        const RenderPipelinePassInputs* Passes = nullptr;
        bool Deferred = false;
        // AO writer registered between Scene G-Buffer and DeferredLightingPass so the
        // builder's read-from-AOBuffer edge derivation discovers the producer in the
        // correct direction (registration order == intended execution order). The
        // technique selector mirrors the value passed to RegisterTransparencyAndAONodes.
        AOTechnique ActiveAOTechnique{};
    };

    struct TransparencyAOStageInputs
    {
        const RenderPipelinePassInputs* Passes = nullptr;
    };

    struct PostProcessStageInputs
    {
        const RenderPipelinePassInputs* Passes = nullptr;
    };

    template<typename T>
    [[nodiscard]] auto BorrowRef(T* instance) -> Ref<T>
    {
        return instance ? Ref<T>(instance) : Ref<T>{};
    }

    [[nodiscard]] inline auto MakeRenderStreamStageInputs(const RenderPipelineInputs& inputs,
                                                          const bool deferred) -> RenderStreamStageInputs
    {
        return RenderStreamStageInputs{ .Passes = &inputs.Passes, .Deferred = deferred };
    }

    [[nodiscard]] inline auto MakeSceneLightingStageInputs(const RenderPipelineInputs& inputs,
                                                           const bool deferred) -> SceneLightingStageInputs
    {
        return SceneLightingStageInputs{ .Passes = &inputs.Passes,
                                         .Deferred = deferred,
                                         .ActiveAOTechnique = inputs.ActiveAOTechnique };
    }

    [[nodiscard]] inline auto MakeTransparencyAOStageInputs(const RenderPipelineInputs& inputs) -> TransparencyAOStageInputs
    {
        return TransparencyAOStageInputs{ .Passes = &inputs.Passes };
    }

    [[nodiscard]] inline auto MakePostProcessStageInputs(const RenderPipelineInputs& inputs) -> PostProcessStageInputs
    {
        return PostProcessStageInputs{ .Passes = &inputs.Passes };
    }

    struct FramebufferTextureInput
    {
        RGFramebufferHandle Framebuffer{};
        RGTextureHandle Texture{};
    };

    [[nodiscard]] inline auto MakeFramebufferTextureInput(const RGFramebufferHandle framebuffer,
                                                          const RGTextureHandle texture) -> FramebufferTextureInput
    {
        return FramebufferTextureInput{ .Framebuffer = framebuffer, .Texture = texture };
    }

    // Keep post-chain source selection compact and consistent: setup chooses
    // one authoritative upstream framebuffer handle, and execute consumes
    // that exact choice instead of replaying the fallback chain.
    template<typename... THandles>
    [[nodiscard]] inline auto SelectFirstValidFramebuffer(const THandles&... handles) -> RGFramebufferHandle
    {
        RGFramebufferHandle selected;
        const auto trySelect = [&selected](const RGFramebufferHandle& handle)
        {
            if (selected.IsValid() || !handle.IsValid())
                return;

            selected = handle;
        };

        (trySelect(handles), ...);
        return selected;
    }

    template<typename... THandles>
    [[nodiscard]] inline auto SelectFirstValidFramebufferForPass(RenderGraphNode* pass, const THandles&... handles) -> RGFramebufferHandle
    {
        const auto selected = SelectFirstValidFramebuffer(handles...);
        if (pass)
            pass->SetPrimaryInputFramebufferHandle(selected);
        return selected;
    }

    template<typename... TInputs>
    [[nodiscard]] inline auto SelectFirstValidFramebufferTextureInput(const TInputs&... inputs) -> FramebufferTextureInput
    {
        FramebufferTextureInput selected{};
        const auto trySelect = [&selected](const FramebufferTextureInput& input)
        {
            if (selected.Framebuffer.IsValid() || selected.Texture.IsValid())
                return;
            if (!input.Framebuffer.IsValid() || !input.Texture.IsValid())
                return;

            selected = input;
        };

        (trySelect(inputs), ...);
        return selected;
    }

    template<typename... TInputs>
    [[nodiscard]] inline auto SelectFirstValidFramebufferTextureInputForPass(RenderGraphNode* pass,
                                                                             const TInputs&... inputs) -> FramebufferTextureInput
    {
        const auto selected = SelectFirstValidFramebufferTextureInput(inputs...);
        if (pass)
        {
            pass->SetPrimaryInputFramebufferHandle(selected.Framebuffer);
            pass->SetPrimaryInputTextureHandle(selected.Texture);
        }
        return selected;
    }

    template<typename... TInputs>
    [[nodiscard]] inline auto ReadFirstValidFramebufferTextureInputForPass(RGBuilder& builder,
                                                                           RenderGraphNode* pass,
                                                                           const TInputs&... inputs) -> FramebufferTextureInput
    {
        const auto selected = SelectFirstValidFramebufferTextureInputForPass(pass, inputs...);
        if (!selected.Texture.IsValid())
            return selected;

        [[maybe_unused]] const auto textureRead =
            builder.Read(selected.Texture, RGReadUsage::ShaderSample);
        return selected;
    }

    // Resource base name pair (framebuffer name + texture-view name) used by
    // the name-based post-process source selector. Each candidate is one
    // potential upstream producer; the selector picks the first whose latest
    // version (or canonical import) resolves to a valid handle pair.
    struct CandidateBaseNames
    {
        std::string_view FramebufferName;
        std::string_view TextureName;
    };

    [[nodiscard]] inline auto MakeCandidateBaseNames(std::string_view framebufferName,
                                                     std::string_view textureName) -> CandidateBaseNames
    {
        return CandidateBaseNames{ .FramebufferName = framebufferName, .TextureName = textureName };
    }

    [[nodiscard]] inline auto SelectFirstValidVersionedInput(const RGBuilder& builder,
                                                             std::initializer_list<CandidateBaseNames> candidates) -> FramebufferTextureInput
    {
        FramebufferTextureInput selected{};
        const auto& graph = builder.GetGraph();
        for (const auto& candidate : candidates)
        {
            const auto framebuffer = graph.GetFramebufferHandle(candidate.FramebufferName);
            const auto texture = graph.GetTextureHandle(candidate.TextureName);
            if (framebuffer.IsValid() && texture.IsValid())
            {
                selected = MakeFramebufferTextureInput(framebuffer, texture);
                break;
            }
        }
        return selected;
    }

    // Name-based equivalent of `ReadFirstValidFramebufferTextureInputForPass`.
    // Each candidate is a (framebufferBaseName, textureBaseName) pair; the
    // selector resolves each via the graph's latest-version map (falling back
    // to the canonical import) and picks the first valid pair. Records the
    // primary input handles on the pass and emits a ShaderSample Read on the
    // chosen texture, matching the prior typed-pointer setter pattern.
    inline auto ReadFirstValidVersionedInputForPass(RGBuilder& builder,
                                                    RenderGraphNode* pass,
                                                    std::initializer_list<CandidateBaseNames> candidates) -> FramebufferTextureInput
    {
        const auto selected = SelectFirstValidVersionedInput(builder, candidates);
        if (pass)
        {
            pass->SetPrimaryInputFramebufferHandle(selected.Framebuffer);
            pass->SetPrimaryInputTextureHandle(selected.Texture);
        }
        if (selected.Texture.IsValid())
        {
            [[maybe_unused]] const auto textureRead =
                builder.Read(selected.Texture, RGReadUsage::ShaderSample);
        }
        return selected;
    }

    template<typename... THandles>
    inline auto ReadFirstValidFramebuffer(RGBuilder& builder, const THandles&... handles) -> RGFramebufferHandle
    {
        const auto selected = SelectFirstValidFramebuffer(handles...);
        if (!selected.IsValid())
            return selected;

        [[maybe_unused]] const auto framebufferRead =
            builder.Read(selected, RGReadUsage::RenderTargetRead);
        return selected;
    }

    template<typename... THandles>
    inline auto ReadFirstValidFramebufferForPass(RGBuilder& builder, RenderGraphNode* pass, const THandles&... handles) -> RGFramebufferHandle
    {
        const auto selected = SelectFirstValidFramebufferForPass(pass, handles...);
        if (!selected.IsValid())
            return selected;

        [[maybe_unused]] const auto framebufferRead =
            builder.Read(selected, RGReadUsage::RenderTargetRead);
        return selected;
    }

    inline void AddExistingNode(RenderGraph& graph, RenderGraphNode* node)
    {
        OLO_CORE_ASSERT(node, "BuildRenderPipelineGraph requires a valid render-stream pass");
        graph.AddNode(BorrowRef(node));
    }

    template<typename TPass>
    [[nodiscard]] auto PrepareGraphNode(std::string_view name,
                                        TPass* pass) -> Ref<TPass>
    {
        auto passRef = BorrowRef(pass);
        OLO_CORE_ASSERT(passRef, "BuildRenderPipelineGraph requires a valid render node");
        // Production passes already implement RenderGraphNode directly.
        // This helper only synchronizes the graph-facing name before
        // registration; it does not wrap or adapt the node.
        passRef->SetName(name);
        return passRef;
    }

    void RegisterRenderStreamNodes(RenderGraph& graph,
                                   const RenderStreamStageInputs& inputs);
    void RegisterSceneAndLightingNodes(RenderGraph& graph,
                                       const SceneLightingStageInputs& inputs);
    void RegisterTransparencyAndAONodes(RenderGraph& graph,
                                        const TransparencyAOStageInputs& inputs);
    void RegisterPostProcessNodes(RenderGraph& graph,
                                  const PostProcessStageInputs& inputs);
} // namespace OloEngine::RenderPipelineBuilderInternal
