#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"

namespace OloEngine
{
    class ComputeShader;
    class SceneRenderPass;
    class Shader;
    class UniformBuffer;

    // @brief GPU-driven virtualized-geometry pass (Nanite-style cluster LOD DAG,
    // issue #629).
    //
    // Runs right after ScenePass in the Deferred path and before
    // DeferredLightingPass / GTAO (pinned via the G-Buffer resource chains):
    //   1. Per virtual-mesh instance, dispatches VirtualClusterCull.comp — the
    //      view-dependent DAG-cut selection plus frustum + normal-cone + Hi-Z
    //      occlusion culling — compacting survivors into per-instance segments of
    //      a shared indirect command buffer. The visible count never touches the CPU.
    //   2. Small-coverage clusters go to the compute software rasterizer
    //      (VirtualClusterRaster.comp) into a 64-bit visibility buffer, resolved
    //      to the G-Buffer by VirtualVisibilityResolve.glsl; large clusters replay
    //      their segment with one glMultiDrawElementsIndirectCount call through
    //      VirtualMeshGBuffer.glsl (both write the exact same MRT encodings as
    //      PBR_GBuffer.glsl, so lighting/AO/SSR/picking work unchanged). The
    //      multisample G-Buffer is supported (hardware path only under MSAA).
    //
    // DEFERRED BY DESIGN (not a limitation to lift): virtualized geometry is a
    // deferred technique — the compute software rasterizer writes a visibility
    // buffer whose material-resolve pass reconstructs the G-Buffer, and there is
    // no G-Buffer in the Forward / Forward+ paths for it to resolve into. This
    // mirrors real Nanite, which is deferred-only for the same reason; a
    // "forward" path could only ever be the hardware cluster draw with the
    // software rasterizer + visibility buffer abandoned, which defeats the point.
    // The pass therefore no-ops outside the Deferred path.
    class VirtualGeometryPass : public RenderGraphNode
    {
      public:
        VirtualGeometryPass();
        ~VirtualGeometryPass() override = default;

        void Init(const FramebufferSpecification& spec) override;
        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Execute(RGCommandContext& context) override;

        // The G-Buffer is owned by SceneRenderPass; this pass borrows it.
        void SetScenePass(const Ref<SceneRenderPass>& scenePass)
        {
            m_ScenePass = scenePass;
        }

        // Forwarded from RendererSettings.Deferred.PerSampleLighting. In MSAA
        // per-sample mode virtual geometry rasterizes into the multisample
        // G-Buffer and is resolved afterwards; otherwise it draws into the
        // resolved G-Buffer directly — the same target rule DeferredOpaqueDecalPass
        // and DeferredGPUOcclusionPass use.
        void SetPerSampleLighting(bool enable) noexcept
        {
            m_PerSampleLighting = enable;
        }

      private:
        Ref<ComputeShader> m_CullShader;
        Ref<ComputeShader> m_RasterShader;      // portable two-pass 2x32 visibility-buffer rasterizer
        Ref<ComputeShader> m_RasterShaderInt64; // single-pass 64-bit atomic-min variant (null if unsupported)
        bool m_Int64AtomicsSupported = false;   // driver exposes GL_ARB_gpu_shader_int64 + GL_NV_shader_atomic_int64
        Ref<Shader> m_GBufferShader;
        Ref<Shader> m_ResolveShader;         // fullscreen visibility-buffer -> G-Buffer material resolve
        Ref<ComputeShader> m_ColorizeShader; // overdraw count -> heat colour (debug capture)
        Ref<SceneRenderPass> m_ScenePass;
        Ref<UniformBuffer> m_DrawInfoUBO;  // UBO_VIRTUAL_DRAW, one update per MDI/resolve draw
        Ref<UniformBuffer> m_DebugInfoUBO; // UBO_VIRTUAL_DEBUG, one update per frame (debug mode)
        // ScenePass publishes the scene/G-Buffer textures as EXPORT COPIES at
        // the end of its Execute — before this pass draws. Re-export after our
        // draws (DeferredGPUOcclusionPass idiom) or lighting/AO/SSR/TAA and
        // the editor grid treat every virtual-geometry pixel as sky.
        RGTextureHandle m_SelectedSceneDepth{};
        RGTextureHandle m_SelectedVelocity{};
        RGTextureHandle m_SelectedGBufferAlbedo{};
        RGTextureHandle m_SelectedGBufferNormal{};
        RGTextureHandle m_SelectedGBufferEmissive{};
        // MSAA per-sample re-export companions (only touched when the G-Buffer is
        // multisample and per-sample lighting is on).
        RGTextureHandle m_SelectedGBufferAlbedoMS{};
        RGTextureHandle m_SelectedGBufferNormalMS{};
        RGTextureHandle m_SelectedGBufferEmissiveMS{};
        RGTextureHandle m_SelectedVelocityMS{};
        RGTextureHandle m_SelectedSceneDepthMS{};

        bool m_PerSampleLighting = false;
    };
} // namespace OloEngine
