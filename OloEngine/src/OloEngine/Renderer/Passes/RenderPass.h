#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Framebuffer.h"

namespace OloEngine
{
    // Forward declarations
    class RenderGraph;

    class RenderPass
    {
    public:
        virtual ~RenderPass() = default;

        // Initialize the render pass with a framebuffer specification
        virtual void Init(const FramebufferSpecification& spec) = 0;

        // Execute the render pass
        virtual void Execute() = 0;

        // Get the pass's input and output framebuffers
        [[nodiscard]] virtual Ref<Framebuffer> GetTarget() const = 0;

        // Set the render pass name (for debugging)
        void SetName(const std::string& name) { m_Name = name; }
        [[nodiscard]] const std::string& GetName() const { return m_Name; }

        // Setup framebuffer
        virtual void SetupFramebuffer(uint32_t width, uint32_t height) = 0;
        virtual void ResizeFramebuffer(uint32_t width, uint32_t height) = 0;

        // Called whenever the pass needs to be reset (e.g., window resize)
        virtual void OnReset() = 0;

    protected:
        std::string m_Name = "RenderPass";
        Ref<Framebuffer> m_Target;
        FramebufferSpecification m_FramebufferSpec;
    };
} 