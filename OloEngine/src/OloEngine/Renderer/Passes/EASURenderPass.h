#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    // @brief FSR1 EASU spatial-upscale pass (#480 — completes the #432 epic).
    //
    // EASU is the UPSCALE half of FSR1: it reconstructs a display-resolution HDR
    // image from a scene rendered BELOW display resolution (the DRS render-scale
    // mechanism renders the scene + screen-space band into the corner of the
    // full-size targets). It runs EARLY — right after the deferred screen-space
    // band and BEFORE Bloom — so every display-res post stage (Bloom/DOF/ToneMap)
    // runs at full resolution. The paired RCAS sharpen runs LATE (post-tonemap)
    // in UpscalerRenderPass.
    //
    // Inputs (selected during Setup()):
    //   * The freshest pre-Bloom HDR colour (ContactShadow/SSR/SSGI/AOApply/SSS or
    //     SceneColor) — at reduced resolution, content in the [0, bounds] corner.
    //   * EASUParams UBO (binding 45), uploaded each frame from the render-scale.
    //
    // Output:
    //   * EASUColor (RGBA16Float, display resolution, HDR).
    //
    // Passthrough semantics: when Upscale == Off the pass no-ops, declares no
    // EASUColor output, and leaves m_Target null, so downstream stages resolve by
    // name and fall back past the absent EASUColor to the native SceneColor chain.
    class EASURenderPass : public RenderGraphNode
    {
      public:
        EASURenderPass();
        ~EASURenderPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Init(const FramebufferSpecification& spec) override;
        void Execute(RGCommandContext& context) override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        void SetEnabled(bool enabled) noexcept
        {
            m_Enabled = enabled;
        }
        [[nodiscard]] bool IsEnabled() const noexcept override
        {
            return m_Enabled;
        }

        void SetSettings(const PostProcessSettings& settings)
        {
            m_Settings = settings;
        }

        // Render-scale this pass upscales FROM. Set per-frame by the pipeline
        // alongside Renderer3D::SetRenderScale so the EASU constants match the
        // actual rendered region exactly (both derive renderW = floor(physW*scale)).
        void SetRenderScale(f32 scale) noexcept
        {
            m_RenderScale = scale;
        }

        [[nodiscard]] bool IsReadyForExecution() const noexcept override
        {
            return m_EASUShader && m_EASUShader->IsReady() && m_EASUUBO;
        }

      private:
        void CreateFramebuffer(u32 width, u32 height);

      private:
        bool m_Enabled = false;
        f32 m_RenderScale = 1.0f;
        PostProcessSettings m_Settings;

        Ref<Shader> m_EASUShader;
        Ref<UniformBuffer> m_EASUUBO;
    };
} // namespace OloEngine
