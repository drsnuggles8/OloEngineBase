#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <vector>

namespace OloEngine
{
    // @brief Standalone Bloom post-process pass.
    //
    // Phase F slice 23 — standalone bloom stage in the dynamic chain:
    //   AOApply/SSS/Scene -> Bloom -> DOF -> MotionBlur -> TAA -> ...
    //
    // Algorithm:
    //   1. Threshold extract: scene HDR → bloom mip 0 (half-res)
    //   2. Progressive downsample: mip 0 → mip 1 → … → mip N
    //   3. Progressive upsample (additive): mip N → … → mip 0
    //   4. Composite: bloom mip 0 + scene color → output RGBA16F
    //
    // Inputs:
    //   * Input framebuffer handle (PostProcessColor)
    //   * PostProcessUBO (binding 7), uploaded by Renderer3D — texel size
    //     is mutated per mip inside Execute() then restored on exit
    //
    // Output:
    //   * BloomColor (RGBA16F, full-res — composite of scene + bloom glow)
    //
    // Passthrough semantics: when disabled, the pass no-ops and GetTarget()
    // returns the input framebuffer directly (no copy).
    class BloomRenderPass : public RenderPass
    {
      public:
        BloomRenderPass();
        ~BloomRenderPass() override = default;

        void Init(const FramebufferSpecification& spec) override;
        void Execute() override;
        void Execute(RGCommandContext& context) override;
        [[nodiscard]] SubmissionModel GetSubmissionModel() const override
        {
            return SubmissionModel::ImmediateOnly;
        }
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        void SetEnabled(bool enabled) noexcept
        {
            m_Enabled = enabled;
        }
        [[nodiscard]] bool IsEnabled() const noexcept
        {
            return m_Enabled;
        }

        void SetPostProcessUBO(const Ref<UniformBuffer>& ubo) noexcept
        {
            m_PostProcessUBO = ubo;
        }

        // Expose the UBO data pointer so Execute() can mutate TexelSize per mip.
        void SetPostProcessGPUData(PostProcessUBOData* gpuData) noexcept
        {
            m_GPUData = gpuData;
        }

      private:
        void CreateFramebuffers(u32 width, u32 height);

      private:
        static constexpr u32 MAX_BLOOM_MIPS = 5;

                // Bitmask of the last startup/runtime failure state observed in Execute().
                // Used to avoid spamming identical error logs every frame.
                u32 m_LastFailureMask = 0;

        bool m_Enabled = false;

        // Full-resolution composite output (scene + bloom glow)
        Ref<Framebuffer> m_OutputFB;

        Ref<Shader> m_BloomThresholdShader;
        Ref<Shader> m_BloomDownsampleShader;
        Ref<Shader> m_BloomUpsampleShader;
        Ref<Shader> m_BloomCompositeShader;

        Ref<UniformBuffer> m_PostProcessUBO;
        PostProcessUBOData* m_GPUData = nullptr;
    };
} // namespace OloEngine
