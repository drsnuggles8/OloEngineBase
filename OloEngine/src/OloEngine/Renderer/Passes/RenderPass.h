#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Framebuffer.h"

namespace OloEngine
{
    // @brief Base class for all render passes.
    //
    // Provides the minimal interface: name, framebuffer lifecycle, and execution.
    // Passes that need a command bucket should inherit CommandBufferRenderPass instead.
    class RenderPass : public RefCounted
    {
      public:
        virtual ~RenderPass() = default;

        virtual void Init(const FramebufferSpecification& spec) = 0;
        virtual void Execute() = 0;
        [[nodiscard]] virtual Ref<Framebuffer> GetTarget() const = 0;

        void SetName(std::string_view name)
        {
            m_Name = name;
        }
        [[nodiscard]] const std::string& GetName() const
        {
            return m_Name;
        }

        virtual void SetupFramebuffer(u32 width, u32 height) = 0;
        virtual void ResizeFramebuffer(u32 width, u32 height) = 0;
        virtual void OnReset() = 0;

        // Called by RenderGraph to pipe the output framebuffer of a previous pass as input.
        // Passes that accept an input framebuffer should override this.
        virtual void SetInputFramebuffer(const Ref<Framebuffer>& /*input*/) {}

      protected:
        std::string m_Name = "RenderPass";
        Ref<Framebuffer> m_Target;
        FramebufferSpecification m_FramebufferSpec;
    };
} // namespace OloEngine
