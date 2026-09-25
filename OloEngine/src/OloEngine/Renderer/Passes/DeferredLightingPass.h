#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/GBuffer.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <array>
#include <glm/glm.hpp>

namespace OloEngine
{
    // @brief Deferred lighting composition pass.
    //
    // Reads the G-Buffer and writes fully-lit scene colour into the
    // forward scene framebuffer's colour attachment 0. This keeps
    // downstream passes (PostProcess, Selection Outline, UIComposite)
    // oblivious to the rendering path.
    //
    // Current iteration supports directional / point / spot lights via
    // the `MultiLightBuffer` UBO; shadow sampling, IBL, Forward+ tile
    // evaluation and per-sample MSAA lighting are follow-ups.
    //
    // If `RenderingPath::Deferred` is inactive or the G-Buffer was not
    // provided, Execute() is a no-op — the pass is safe to register
    // unconditionally in the render graph.
    class DeferredLightingPass : public RenderGraphNode
    {
      public:
        DeferredLightingPass();
        ~DeferredLightingPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;

        // Setup() picks per-sample shading, and the multisample inputs it reads, from
        // these four.
        void AppendDeclarationInputs(RGDeclarationKey& key) const override;
        void Init(const FramebufferSpecification& spec) override;
        void Execute(RGCommandContext& context) override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        // The G-Buffer is provided by SceneRenderPass each frame; null
        // turns the pass into a no-op.
        void SetGBuffer(const Ref<GBuffer>& gbuffer) noexcept
        {
            m_GBuffer = gbuffer;
        }
        // SetSceneColorHandle removed; Execute() self-resolves
        // Forwarded from RendererSettings; 0 = lighting, non-zero = skip
        // (SceneRenderPass's BlitGBufferDebug already wrote a channel).
        void SetDebugChannel(u32 channel) noexcept
        {
            m_DebugChannel = channel;
        }

        // Controls per-sample MSAA shading. When true (and GBuffer sample
        // count > 1), Execute() binds the multisample G-Buffer attachments
        // and uses the sampler2DMS shader variant; otherwise it samples
        // the resolved single-sample copy via the default shader.
        void SetPerSampleLighting(bool enable) noexcept
        {
            m_PerSampleLighting = enable;
        }

        // Which of a material's separated lighting outputs replaces the
        // composite (issue #1231). None is the normal frame. Driven by
        // PostProcessSettings::MaterialDebug, like the other *DebugView flags.
        void SetMaterialDebugView(MaterialDebugView view) noexcept
        {
            m_MaterialDebugView = view;
        }
        [[nodiscard]] MaterialDebugView GetMaterialDebugView() const noexcept
        {
            return m_MaterialDebugView;
        }

        // Screen-space AO applied to the AMBIENT term inside this pass (issue
        // #1336) — RenderPipeline sets it from SelectScreenSpaceAOApplication,
        // which on the deferred path takes the AO buffer away from
        // PostProcess_SSAOApply's whole-frame multiply. `projA` / `projB` are
        // the reconstruction projection's (2,2) / (3,2) coefficients, the pair
        // the AO pass's own depth-aware upsample linearises with.
        void SetScreenSpaceAO(bool applyToAmbient, f32 intensity, f32 projA, f32 projB) noexcept
        {
            m_ScreenAOToAmbient = applyToAmbient;
            m_ScreenAOIntensity = intensity;
            m_ScreenAOProjA = projA;
            m_ScreenAOProjB = projB;
        }
        // The DeferredLightingControls ScreenAOParams lanes as RenderPipeline
        // set them: x = applied to the ambient here, y = strength, z/w = the
        // reconstruction projection's (2,2) / (3,2). SSGI replaces a fraction
        // of the SAME ambient and needs the same AO to know how much is there.
        [[nodiscard]] glm::vec4 ScreenSpaceAOParams() const noexcept
        {
            return { m_ScreenAOToAmbient ? 1.0f : 0.0f, m_ScreenAOIntensity, m_ScreenAOProjA, m_ScreenAOProjB };
        }

        // Screen-space contact shadows applied to the PRIMARY directional
        // light's visibility inside this pass (issue #1336), from the march
        // parameters in `ubo` (UBO_CONTACT_SHADOW, filled by RenderPipeline).
        // Off when `applyToSun` is false or no UBO was handed over.
        void SetContactShadow(bool applyToSun, const Ref<UniformBuffer>& ubo) noexcept
        {
            m_ContactShadowInLighting = applyToSun;
            m_ContactShadowUBO = ubo;
        }

        // The ambient ladder's rung controls this pass shades with (issue
        // #1336): x = EnableIBL (a complete global IBL trio is bound), y =
        // EnableLightProbes, z = IBLIntensity, w = 0. ONE function, read by
        // Execute for DeferredLightingControls and by RenderPipeline for SSGI,
        // so the two cannot disagree about which rung a pixel took.
        [[nodiscard]] static glm::vec4 AmbientLadderControls();

      private:
        // Composite the virtualized-geometry cluster/LOD/overdraw debug image over the LIT
        // scene colour (issue #629). No-op unless VirtualMeshRegistry's debug mode is active
        // AND RendererSettings::VirtualDebugToViewport is set.
        //
        // This lives at the END of Execute rather than in VirtualGeometryPass because that
        // pass runs BEFORE lighting — it writes the G-Buffer — so anything it drew into scene
        // colour would be overwritten by the lighting draw. Before this existed, flipping the
        // Statistics panel's "Debug view" combo changed nothing a human could see: the debug
        // image was only reachable as an MCP capture target.
        void BlitVirtualGeometryDebugOverlay();

        struct SelectedInputs
        {
            RGTextureHandle GBufferAlbedo;
            RGTextureHandle GBufferNormal;
            RGTextureHandle GBufferEmissive;
            RGTextureHandle Velocity;
            RGTextureHandle GBufferBakedGI; // RT5 — baked lightmap irradiance + coverage (issue #865)
            RGTextureHandle SceneDepth;
            RGTextureHandle AOBuffer;
            RGTextureHandle ShadowMapCSM;
            RGTextureHandle ShadowMapAtlas; // local-light shadow atlas (issue #435)
            // Ray-traced shadow visibility mask (issue #1056). Invalid whenever
            // the technique fell back, which is what makes the fallback a fact
            // the graph carries rather than a flag a shader has to be told.
            RGTextureHandle RayTracedShadowMask;
            // ReSTIR DI's resolved direct lighting (issue #1140). Invalid
            // whenever the tier stood down, which is what makes the fallback a
            // fact the GRAPH carries rather than a flag a shader has to be told;
            // the shader's own per-pixel alpha test is the second half.
            RGTextureHandle ReSTIRDIRadiance;
            RGTextureHandle ReSTIRGIRadiance;
            bool UsesReSTIRPT = false;
            // Comparison-OFF raw-depth view GL ids (PCSS blocker search); 0 = none.
            RHI::ResourceHandle ShadowMapCSMRawID{};
            RHI::ResourceHandle ShadowMapAtlasRawID{};
            RGTextureHandle IrradianceMap;
            RGTextureHandle PrefilterMap;
            RGTextureHandle BrdfLut;
        };

        Ref<Shader> m_Shader;              // sampler2D variant (non-MSAA / resolved)
        Ref<Shader> m_ShaderMSAA;          // sampler2DMS variant (per-sample)
        Ref<Shader> m_VirtualDebugOverlay; // lazily loaded; only when the debug overlay is on
        Ref<GBuffer> m_GBuffer;
        Ref<Framebuffer> m_SceneFramebuffer;
        Ref<UniformBuffer> m_ControlsUBO;
        SelectedInputs m_SelectedInputs{};
        u32 m_DebugChannel = 0;
        MaterialDebugView m_MaterialDebugView = MaterialDebugView::None;
        bool m_PerSampleLighting = true;
        bool m_UseMSAAShading = false;
        bool m_ScreenAOToAmbient = false;
        f32 m_ScreenAOIntensity = 1.0f;
        f32 m_ScreenAOProjA = 0.0f;
        f32 m_ScreenAOProjB = 0.0f;
        bool m_ContactShadowInLighting = false;
        Ref<UniformBuffer> m_ContactShadowUBO;
    };
} // namespace OloEngine
