#pragma once

#include "OloEngine/Renderer/Framebuffer.h"

#include <mutex>

namespace OloEngine
{
    class OpenGLFramebuffer : public Framebuffer
    {
      public:
        explicit OpenGLFramebuffer(FramebufferSpecification spec);
        ~OpenGLFramebuffer() override;

        void Invalidate();

        void Bind() override;
        void Unbind() override;

        // TODO(olbu): Add BindColorAttachment, BindDepthAttachment

        void Resize(u32 width, u32 height) override;
        int ReadPixel(u32 attachmentIndex, int x, int y) override;

        void ClearAttachment(u32 attachmentIndex, int value) override;

        [[nodiscard("Store this!")]] u32 GetColorAttachmentRendererID(const u32 index) const override
        {
            OLO_CORE_ASSERT(index < m_ColorAttachments.size());
            return m_ColorAttachments[index];
        }
        [[nodiscard("Store this!")]] u32 GetDepthAttachmentRendererID() const override
        {
            return m_DepthAttachment;
        }
        [[nodiscard("Store this!")]] const FramebufferSpecification& GetSpecification() const override
        {
            return m_Specification;
        }
        [[nodiscard("Store this!")]] u32 GetRendererID() const
        {
            return m_RendererID;
        }

        // Static initialization/shutdown for shared resources
        static void InitSharedResources();
        static void ShutdownSharedResources();

      private:
        void InitPostProcessing();
        void ApplyPostProcessing();

      private:
        u32 m_RendererID = 0;
        FramebufferSpecification m_Specification;

        std::vector<FramebufferTextureSpecification> m_ColorAttachmentSpecifications;
        FramebufferTextureSpecification m_DepthAttachmentSpecification = FramebufferTextureFormat::None;

        std::vector<u32> m_ColorAttachments;
        u32 m_DepthAttachment = 0;

        // Post-processing resources (per-framebuffer VAO/VBO for proper state isolation)
        u32 m_PostProcessVAO = 0;
        u32 m_PostProcessVBO = 0;

        // Shared post-processing shader (static to avoid recompilation for each framebuffer)
        static Ref<class Shader> s_PostProcessShader;
        static std::once_flag s_InitOnceFlag;
    };
} // namespace OloEngine
