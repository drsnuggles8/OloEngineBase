#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/OITBuffer.h"
#include "OloEngine/Renderer/Passes/CommandBufferRenderPass.h"
#include "OloEngine/Renderer/Shader.h"

#include <functional>
#include <utility>

namespace OloEngine
{
    // @brief Render pass for animated water surfaces.
    //
    // Uses the command bucket system for sorted dispatch of DrawWaterCommands.
    // Renders into the ScenePass framebuffer after foliage geometry but before decals.
    // Water is rendered with alpha blending for transparency support.
    //
    // This pass follows the Molecular Matters design — water surfaces are POD
    // commands submitted to a command bucket, sorted by DrawKey, and dispatched
    // through the standard CommandDispatch table.
    class WaterRenderPass : public CommandBufferRenderPass
    {
      public:
        WaterRenderPass();
        ~WaterRenderPass() override;

        WaterRenderPass(const WaterRenderPass&) = delete;
        WaterRenderPass& operator=(const WaterRenderPass&) = delete;
        WaterRenderPass(WaterRenderPass&&) = delete;
        WaterRenderPass& operator=(WaterRenderPass&&) = delete;

        void Init(const FramebufferSpecification& spec) override;
        void Execute() override;
        void Execute(RGCommandContext& context) override;
        [[nodiscard]] SubmissionModel GetSubmissionModel() const override
        {
            return SubmissionModel::Mixed;
        }
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        void SetSceneFramebuffer(const Ref<Framebuffer>& fb);

        // WB-OIT wiring (mirrors ParticleRenderPass). When a valid OITBuffer
        // is attached AND `SetOITEnabled(true)` is called, `Execute` routes
        // water draws into the OITBuffer attachments with per-attachment
        // blend funcs and installs a `CommandDispatch` shader override so
        // the Water_OIT variant is used in place of the forward Water shader.
        // `SetOITAccumulationMarker` is invoked after a non-empty dispatch so
        // `OITResolveRenderPass` knows it has fresh accumulation to composite.
        void SetOITBuffer(const Ref<OITBuffer>& oitBuffer) noexcept
        {
            m_OITBuffer = oitBuffer;
        }
        void SetOITEnabled(bool enabled) noexcept
        {
            m_OITEnabled = enabled;
        }
        void SetOITAccumulationMarker(std::function<void()> marker)
        {
            m_AccumMarker = std::move(marker);
        }
        void SetOITShader(const Ref<Shader>& shader) noexcept
        {
            m_OITShader = shader;
        }

      private:
        void EnsureRefractionTexture(u32 width, u32 height);

        Ref<Framebuffer> m_SceneFramebuffer;
        u32 m_RefractionTextureID = 0;
        u32 m_RefractionWidth = 0;
        u32 m_RefractionHeight = 0;

        Ref<OITBuffer> m_OITBuffer;
        Ref<Shader> m_OITShader;
        std::function<void()> m_AccumMarker;
        bool m_OITEnabled = false;
    };
} // namespace OloEngine
