#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    // Per-frame automatic-exposure parameters fed to the ToneMap pass by the
    // render pipeline (sourced from PostProcessSettings + the frame delta time).
    // The metering math itself lives in Renderer/AutoExposure.h.
    struct AutoExposureFrameParams
    {
        bool Enabled = false;
        f32 MinLogLuminance = -8.0f;
        f32 MaxLogLuminance = 3.5f;
        f32 SpeedUp = 3.0f;
        f32 SpeedDown = 1.0f;
        f32 Compensation = 0.0f;
        f32 MinExposure = 0.05f;
        f32 MaxExposure = 16.0f;
        f32 DeltaTime = 0.0f;
    };

    // @brief Standalone tone-mapping post-process pass (HDR → LDR).
    //
    // Phase F slice 17 — standalone effect in the dynamic post chain
    // following the pattern established by FXAARenderPass (slice 16).
    //
    // Sits third in the extracted-effect sub-chain:
    //   PostProcess → ChromAberration → ColorGrading → ToneMap → Vignette → FXAA
    //
    // Unlike the other extracted effects, tone mapping runs unconditionally
    // (no per-settings gate). `m_Enabled` defaults to `true` and is never
    // set false by Renderer3D. The `SetEnabled` setter is provided only for
    // debugging / future use.
    class ToneMapRenderPass : public RenderGraphNode
    {
      public:
        ToneMapRenderPass();
        ~ToneMapRenderPass() override = default;

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

        void SetPostProcessUBO(const Ref<UniformBuffer>& ubo) noexcept
        {
            m_PostProcessUBO = ubo;
        }

        // Underwater fog UBO (Renderer3D-owned, binding 36). Bound during
        // Execute so the tone-map shader's underwater stage can read the
        // camera-below-water tint parameters. See WATER_FUTURE_IMPROVEMENTS.md §7.2.
        void SetUnderwaterFogUBO(const Ref<UniformBuffer>& ubo) noexcept
        {
            m_UnderwaterFogUBO = ubo;
        }

        // Automatic exposure / eye adaptation. When enabled, two compute passes
        // meter the HDR input each frame and write the metered exposure into a
        // storage buffer the tone-map shader consumes (the manual Exposure is
        // bypassed). See Renderer/AutoExposure.h for the math.
        void SetAutoExposure(const AutoExposureFrameParams& params) noexcept
        {
            m_AutoExposure = params;
        }

        [[nodiscard]] bool IsReadyForExecution() const noexcept override
        {
            return m_Shader && m_Shader->IsReady() && m_PostProcessUBO;
        }

      private:
        void CreateFramebuffer(u32 width, u32 height);

        // Lazily create the metering compute shaders + histogram buffer (only
        // when auto-exposure is first used). Returns false if creation failed.
        bool EnsureAutoExposureResources();
        // Run the histogram + average compute passes over the HDR input,
        // leaving the metered exposure in m_ExposureStateBuffer[0].
        void RunAutoExposureMetering(const RGCommandContext& context, u32 hdrTextureID, u32 width, u32 height);

        Ref<Shader> m_Shader;
        Ref<UniformBuffer> m_PostProcessUBO;
        Ref<UniformBuffer> m_UnderwaterFogUBO;
        RGTextureHandle m_SelectedSceneDepthTexture;

        // Auto-exposure (eye adaptation) resources. m_ExposureStateBuffer holds
        // [0]=metered exposure (<=0 => use manual), [1]=adapted luminance (persists
        // across frames); it is created in Init so binding 20 is always valid.
        Ref<ComputeShader> m_HistogramShader;
        Ref<ComputeShader> m_AverageShader;
        Ref<StorageBuffer> m_HistogramBuffer;
        Ref<StorageBuffer> m_ExposureStateBuffer;
        AutoExposureFrameParams m_AutoExposure;
        bool m_AutoExposureActiveLastFrame = false;

        // Defaults true — tone mapping runs every frame unconditionally.
        bool m_Enabled = true;
    };
} // namespace OloEngine
