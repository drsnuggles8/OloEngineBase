#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/GBuffer.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Instancing/GPUFrustumCuller.h"

#include <vector>

namespace OloEngine
{
    class CommandPacket;

    // @brief Deferred-path phase-2 of the two-phase GPU-driven HZB occlusion
    // cull (#486 — extends #431's Forward/Forward+ two-phase scheme to Deferred).
    //
    // In the Deferred path the phase-1 instanced draws stay on the normal
    // ScenePass G-Buffer bucket (the phase-1 cull ran at submission against the
    // PREVIOUS frame's retained Hi-Z, exactly like the legacy single-phase
    // deferred cull). What was missing there is the SECOND phase: an instance
    // occluded last frame but disoccluded this frame popped for one frame.
    //
    // This pass closes that gap. It runs after ScenePass (occluders + phase-1
    // survivors are now in the G-Buffer depth) and BEFORE DeferredOpaqueDecalPass
    // / AO / DeferredLightingPass. Its Execute:
    //   1. Rebuilds a Hi-Z from THIS frame's G-Buffer depth (occluders +
    //      phase-1 survivors).
    //   2. Re-tests each batch's phase-1 reject list against it (current
    //      transform, no reprojection, no frustum test — UE's OCCLUSION_POST).
    //   3. Draws the recovered (disoccluded) instances into the G-Buffer.
    //   4. Resolves MSAA (per-sample mode) and re-exports the G-Buffer
    //      attachments (SceneDepth / SceneNormals / GBuffer{Albedo,Normal,
    //      Emissive} / Velocity + MSAA companions) so AO / lighting / SSR see
    //      the disoccluded geometry — mirroring DeferredOpaqueDecalPass's
    //      G-Buffer publication.
    //
    // Forward / Forward+ never create a G-Buffer, so the pass no-ops there
    // (m_GBuffer stays null) — the forward two-phase path is handled by the
    // separate GPUDrivenOcclusionPass, which targets the scene framebuffer.
    class DeferredGPUOcclusionPass : public RenderGraphNode
    {
      public:
        DeferredGPUOcclusionPass();
        ~DeferredGPUOcclusionPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Execute(RGCommandContext& context) override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void OnReset() override;

        void SetGBuffer(const Ref<GBuffer>& gbuffer) noexcept
        {
            m_GBuffer = gbuffer;
        }
        // Forwarded from RendererSettings. In MSAA per-sample mode the phase-2
        // instances rasterize into the multisample G-Buffer FBO (and are then
        // resolved); otherwise they draw straight into the resolved FBO — the
        // same targeting rule DeferredOpaqueDecalPass uses.
        void SetPerSampleLighting(bool enable) noexcept
        {
            m_PerSampleLighting = enable;
        }

        // Register a phase-2 draw. `packet` is a DrawMeshInstancedCommand whose
        // cull buffers point at `cull`'s phase-2 survivor/indirect; `cull`
        // carries the reject buffer the phase-2 cull re-tests against this
        // frame's HZB. Both are consumed in Execute after the HZB build, then
        // dropped. Called from Renderer3D::SubmitGPUCulledInstanced once per
        // deferred two-phase batch.
        void SubmitPhase2(CommandPacket* packet, const GPUFrustumCuller::TwoPhaseCullResult& cull);

      private:
        Ref<GBuffer> m_GBuffer;
        bool m_PerSampleLighting = false;

        // Phase-2 work registered this frame (parallel arrays: packet[i] draws
        // cull[i].Phase2Output after DispatchPhase2 fills it). Cleared each Execute.
        std::vector<CommandPacket*> m_Phase2Packets;
        std::vector<GPUFrustumCuller::TwoPhaseCullResult> m_Phase2Culls;

        // G-Buffer re-export targets. After the phase-2 draws the pass copies
        // the live G-Buffer attachments into these graph textures so downstream
        // consumers (which sample the exported textures, not the FBO) include
        // the disoccluded geometry. ScenePass exported them before this pass ran.
        RGTextureHandle m_SelectedSceneDepthExport{};
        RGTextureHandle m_SelectedSceneNormalsExport{};
        RGTextureHandle m_SelectedVelocityExport{};
        RGTextureHandle m_SelectedGBufferAlbedoExport{};
        RGTextureHandle m_SelectedGBufferNormalExport{};
        RGTextureHandle m_SelectedGBufferEmissiveExport{};
        RGTextureHandle m_SelectedGBufferAlbedoMSExport{};
        RGTextureHandle m_SelectedGBufferNormalMSExport{};
        RGTextureHandle m_SelectedGBufferEmissiveMSExport{};
        RGTextureHandle m_SelectedVelocityMSExport{};
        RGTextureHandle m_SelectedSceneDepthMSExport{};
    };
} // namespace OloEngine
