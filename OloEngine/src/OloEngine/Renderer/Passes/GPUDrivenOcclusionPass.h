#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/CommandBufferRenderPass.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/Instancing/GPUFrustumCuller.h"

#include <vector>

namespace OloEngine
{
    class CommandPacket;

    // @brief GPU-driven two-phase occlusion culling pass for dense instanced
    // static geometry (#431).
    //
    // Occlusion-culled instanced batches (count >= GPUCullThreshold, HZB
    // occlusion enabled) are routed here instead of into the SceneRenderPass
    // bucket. This pass runs right AFTER ScenePass (which has drawn all
    // non-instanced opaque geometry — the occluders — into the scene depth) and
    // BEFORE the AO / lighting passes, and draws the surviving instances into
    // the borrowed scene framebuffer.
    //
    // Two-phase scheme (Aaltonen/Haar GPU-driven, Nanite-style):
    //   * Phase 1: cull each batch against the PREVIOUS frame's depth pyramid
    //     (reprojected) and draw the survivors. The cull compute runs at
    //     submission time; this pass only replays the resulting indirect draws.
    //   * (Stage 2) build a Hi-Z from the now-current depth, re-test the
    //     phase-1-rejected instances against it, and draw the ones that turned
    //     out visible this frame — eliminating the one-frame disocclusion pop.
    //
    // Forward / Forward+ only: the instanced batches are forward-lit PBR meshes.
    // In Deferred they stay on the SceneRenderPass G-Buffer path (the legacy
    // single-phase frustum+HZB cull still applies there).
    //
    // Known Stage-1 gap (closed in Stage 3): ScenePass exports SceneDepth /
    // SceneNormals at the end of its Execute, before this pass draws, so the
    // AO / SSR passes do not yet see this pass's instanced geometry in their
    // depth/normal inputs. The pass's OWN occlusion Hi-Z (Stage 2) is built
    // from the live framebuffer depth and is unaffected.
    class GPUDrivenOcclusionPass : public CommandBufferRenderPass
    {
      public:
        GPUDrivenOcclusionPass();
        ~GPUDrivenOcclusionPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Execute(RGCommandContext& context) override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        // Register a phase-2 (#431) draw. `packet` is a
        // DrawMeshInstancedCommand whose cull buffers point at `cull`'s phase-2
        // survivor/indirect; `cull` carries the reject buffer the phase-2 cull
        // re-tests against the current-frame HZB. Both are consumed in Execute
        // after the phase-1 draws + Hi-Z build, then dropped. Called from the
        // submission path (Renderer3D::SubmitGPUCulledInstanced) once per batch.
        void SubmitPhase2(CommandPacket* packet, const GPUFrustumCuller::TwoPhaseCullResult& cull);

        // THE FORWARD PREPASS SHARE (issue #1452), run by
        // GPUDrivenOcclusionPrepassPass ahead of the screen-space AO passes when
        // a forward AO buffer is produced: both phases depth + view-normal
        // only, then Execute() replays the same draws in colour.
        void SetupForwardPrepass(RGBuilder& builder, FrameBlackboard& board);
        void ExecuteForwardPrepass(RGCommandContext& context, const Ref<Framebuffer>& sceneTarget);

      private:
        // Binds the scene target with every colour attachment, the opaque
        // depth state and the shared scene resources.
        void BindSceneForDraw(RGCommandContext& context);
        // Phase 1, then — when `cullPhase2` — the mid-frame Hi-Z and the
        // phase-2 culls, then the phase-2 packets. Returns whether the phase-2
        // packets were drawn: they are only when the mid-frame Hi-Z was usable,
        // because the culls that fill their indirect buffers only run then.
        [[nodiscard]] bool DrawPhases(RGCommandContext& context, bool cullPhase2);
        // Copies the scene target's depth / view normals over the exports.
        void ExportDepthAndNormals(RGCommandContext& context, RGTextureHandle depthExport,
                                   RGTextureHandle normalsExport);

        Ref<Framebuffer> m_SceneFramebuffer;
        // Phase-2 work registered this frame (parallel arrays: packet[i] draws
        // cull[i].Phase2Output after DispatchPhase2 fills it). Cleared each Execute.
        std::vector<CommandPacket*> m_Phase2Packets;
        std::vector<GPUFrustumCuller::TwoPhaseCullResult> m_Phase2Culls;
        // SceneDepth / SceneNormals export targets (#431). After drawing,
        // the pass re-copies the live framebuffer depth + view-normals into these
        // so the downstream AO / SSR passes (which sample the exported textures,
        // not the framebuffer) see the instanced geometry. ScenePass exported
        // them before this pass drew.
        RGTextureHandle m_SelectedSceneDepth{};
        RGTextureHandle m_SelectedSceneNormals{};
        RGTextureHandle m_PrepassSceneDepth{};
        RGTextureHandle m_PrepassSceneNormals{};
        RGTextureHandle m_PrepassForwardAODepth{};
        // Set by ExecuteForwardPrepass: both phases are in depth, the phase-2
        // culls have run, and Execute() only replays the draws in colour.
        bool m_ForwardPrepassDrew = false;
        // Whether that prepass drew phase 2. The colour replay draws the
        // phase-2 packets only then: otherwise their indirect buffers were not
        // filled this frame and would replay stale or zero instances.
        bool m_ForwardPrepassDrewPhase2 = false;
        u32 m_SceneColorAttachmentCount = 0;
    };
} // namespace OloEngine
