#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/GBuffer.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/ResourceHandle.h"

namespace OloEngine
{
    class DecalRenderPass;

    // @brief Thin graph-scheduled shim that drains the opaque decal bucket
    // into the G-Buffer between `ScenePass` and `DeferredLightingPass`.
    //
    // Historically `SceneRenderPass::Execute()` called
    // `DecalRenderPass::ExecuteOnGBuffer(...)` synchronously between its
    // MRT write and the debug blit. That implicit coupling wasn't visible
    // in the render graph — a graph consumer couldn't tell that decals
    // had to complete before `DeferredLightingPass` ran by inspecting
    // edges alone. This pass turns the coupling into an explicit graph
    // node so:
    //
    //   - Ordering is declared via `AddExecutionDependency` edges,
    //   - Resource publication of the resolved G-Buffer albedo/normal/
    //     emissive textures (plus deferred `SceneNormals`) is centralized
    //     after opaque decals have run,
    //   - Future reorderings of the deferred pipeline don't accidentally
    //     invert the "opaque decals before lighting" contract.
    //
    // The pass holds non-owning refs to the `DecalRenderPass` (source of
    // the command bucket) and the `GBuffer` (target). It is registered
    // by `Renderer3D::ConfigureRenderGraph` only when
    // `RenderingPath::Deferred` is active.
    class DeferredOpaqueDecalPass : public RenderGraphNode
    {
      public:
        DeferredOpaqueDecalPass();
        ~DeferredOpaqueDecalPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Execute(RGCommandContext& context) override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;

        void SetDecalPass(const Ref<DecalRenderPass>& decalPass) noexcept
        {
            m_DecalPass = decalPass;
        }
        void SetGBuffer(const Ref<GBuffer>& gbuffer) noexcept
        {
            m_GBuffer = gbuffer;
        }
        // Forwarded from RendererSettings. In MSAA per-sample mode,
        // decals rasterize into the multisample G-Buffer FBO but still
        // sample resolved single-sample depth; otherwise both read and
        // write target the resolved FBO.
        void SetPerSampleLighting(bool enable) noexcept
        {
            m_PerSampleLighting = enable;
        }

      private:
        Ref<DecalRenderPass> m_DecalPass;
        Ref<GBuffer> m_GBuffer;
        bool m_PerSampleLighting = false;
        RGTextureHandle m_SelectedSceneNormalsExport{};
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
