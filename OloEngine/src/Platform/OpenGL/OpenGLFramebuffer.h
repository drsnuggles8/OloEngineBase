#pragma once

#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
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

        void SetRenderViewportSize(u32 width, u32 height) override;
        [[nodiscard]] u32 GetRenderViewportWidth() const override
        {
            return m_RenderViewportWidth;
        }
        [[nodiscard]] u32 GetRenderViewportHeight() const override
        {
            return m_RenderViewportHeight;
        }

        int ReadPixel(u32 attachmentIndex, int x, int y) override;

        void ClearAttachment(u32 attachmentIndex, int value) override;
        void ClearAttachment(u32 attachmentIndex, const glm::vec4& value) override;
        void ClearAllAttachments(const glm::vec4& clearColor = glm::vec4(0.0f), int entityIdClear = -1) override;

        [[nodiscard("Store this!")]] u32 GetColorAttachmentRendererID(const u32 index) const override
        {
            OLO_CORE_ASSERT(index < m_ColorAttachments.size());
            return m_ColorAttachments[index];
        }
        [[nodiscard("Store this!")]] u32 GetDepthAttachmentRendererID() const override
        {
            return m_DepthAttachment;
        }

        [[nodiscard("Store this!")]] RHI::ResourceHandle GetColorAttachmentHandle(const u32 index) const override
        {
            OLO_CORE_ASSERT(index < m_ColorAttachmentHandles.size());
            return m_ColorAttachmentHandles[index].Get();
        }
        [[nodiscard("Store this!")]] RHI::ResourceHandle GetDepthAttachmentHandle() const override
        {
            return m_DepthAttachmentHandle.Get();
        }
        [[nodiscard("Store this!")]] const FramebufferSpecification& GetSpecification() const override
        {
            return m_Specification;
        }
        [[nodiscard("Store this!")]] u32 GetRendererID() const override
        {
            return m_RendererID;
        }

        [[nodiscard]] RHI::ResourceHandle GetRHIHandle() const override
        {
            return m_RHIHandle.Get();
        }

        void AttachDepthTextureArrayLayer(u32 textureArrayRendererID, u32 layer) override;

        // Static initialization/shutdown for shared resources
        static void InitSharedResources();
        static void ShutdownSharedResources();

      private:
        u32 m_RendererID = 0;
        // Generation-checked identity for m_RendererID above, kept in
        // lockstep by m_RHIHandle.Sync() at every site that assigns the
        // native name. RAII retires the entry, so a handle to a destroyed
        // object can never resolve to a recycled GL name (issue #691).
        RHI::ScopedResourceHandle m_RHIHandle;
        FramebufferSpecification m_Specification;

        // DRS render viewport override. When non-zero, Bind() uses these
        // dimensions for glViewport instead of m_Specification.Width/Height.
        // Reset to zero by Resize() so physical resizes clear the override.
        u32 m_RenderViewportWidth = 0;
        u32 m_RenderViewportHeight = 0;

        std::vector<FramebufferTextureSpecification> m_ColorAttachmentSpecifications;
        FramebufferTextureSpecification m_DepthAttachmentSpecification = FramebufferTextureFormat::None;

        std::vector<u32> m_ColorAttachments;
        u32 m_DepthAttachment = 0;
        // Identities parallel to the native names above. A resize genuinely
        // destroys and recreates the attachment textures, so unlike a texture
        // hot-reload these become NEW objects and must get new handles —
        // anything still holding the old ones has to see them go stale.
        std::vector<RHI::ScopedResourceHandle> m_ColorAttachmentHandles;
        RHI::ScopedResourceHandle m_DepthAttachmentHandle;

        // Shared post-processing shader (static to avoid recompilation for each framebuffer)
        static Ref<class Shader> s_PostProcessShader;
        static std::once_flag s_InitOnceFlag;
    };
} // namespace OloEngine
