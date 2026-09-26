#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/CommandBufferRenderPass.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/RGCommandContext.h"

namespace OloEngine
{
    // @brief Render pass for terrain foliage (grass, plants, etc.)
    //
    // Uses the command bucket system for sorted dispatch of DrawFoliageLayerCommands.
    // Renders into the ScenePass framebuffer after opaque scene geometry.
    // Each foliage layer on each terrain entity becomes one POD command.
    //
    // This pass follows the Molecular Matters design — foliage layers are POD
    // commands submitted to a command bucket, sorted by DrawKey, and dispatched
    // through the standard CommandDispatch table.
    class FoliageRenderPass : public CommandBufferRenderPass
    {
      public:
        FoliageRenderPass();
        ~FoliageRenderPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;

        // Setup() declares nothing without a submitted draw (#1315), so that is
        // a declaration input.
        void AppendDeclarationInputs(RGDeclarationKey& key) const override
        {
            key.Add(HasSubmittedCommands());
        }
        void Execute(RGCommandContext& context) override;

        // THE FOLIAGE SHARE OF THE FORWARD PREPASS (issue #1474), run by
        // FoliagePrepassPass. With a forward screen-space AO buffer this frame,
        // the foliage bucket is replayed depth + view normal
        // (Foliage_*_DepthNormal.glsl) after the scene and GPU-driven prepasses
        // and before the AO passes, and the depth, normals and AO depth copy
        // are exported again, so the AO buffer holds the leaves' own occlusion
        // and Execute()'s colour draws can apply it to their ambient term.
        // Without one this declares nothing and does nothing.
        void SetupForwardPrepass(RGBuilder& builder, FrameBlackboard& board);
        void ExecuteForwardPrepass(RGCommandContext& context, const Ref<Framebuffer>& sceneTarget);

        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

      private:
        // Copies a scene-target attachment into its graph export (the
        // self-copy and zero-size guards SceneRenderPass's exports have).
        void CopyToExport(RGCommandContext& context, RGTextureHandle handle, RHI::ResourceHandle source) const;

        Ref<Framebuffer> m_SceneFramebuffer;
        RGTextureHandle m_SelectedVelocityExport;
        RGTextureHandle m_SelectedSceneDepthExport;
        // Whether the colour draws read the forward AO buffer (issue #1474).
        bool m_ReadsForwardAO = false;
        // The prepass half's export handles (its versions of SceneDepth,
        // SceneNormals and ForwardAODepth, which the AO passes read).
        RGTextureHandle m_PrepassSceneDepth;
        RGTextureHandle m_PrepassSceneNormals;
        RGTextureHandle m_PrepassForwardAODepth;
    };
} // namespace OloEngine
