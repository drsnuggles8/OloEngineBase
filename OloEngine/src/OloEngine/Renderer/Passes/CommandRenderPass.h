#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"
#include "OloEngine/Renderer/Commands/CommandMemoryManager.h"

namespace OloEngine
{
    /**
     * @brief Base class for render passes that use the command-based rendering system.
     * 
     * This class extends RenderPass to integrate with the command bucket system,
     * allowing render passes to efficiently build and execute rendering commands.
     */
    class CommandRenderPass
    {
    public:
		virtual ~CommandRenderPass() = default;

		/**
         * @brief Initialize the command-based render pass with a framebuffer specification.
         * @param spec The specification for the framebuffer to render to
         */
        virtual void Init(const FramebufferSpecification& spec) = 0;
        
        /**
         * @brief Execute the render pass operations.
         * This is called by the render graph during the execution phase.
         */
        virtual void Execute() = 0;

        /**
         * @brief Get the target framebuffer of this pass.
         * @return A reference to the target framebuffer
         */
        [[nodiscard]] virtual Ref<Framebuffer> GetTarget() const = 0;

        /**
         * @brief Set a name for this render pass.
         * @param name The name to assign to this pass
         */
        void SetName(std::string_view name) { m_Name = name; }
        
        /**
         * @brief Get the name of this render pass.
         * @return The name of the render pass
         */
        [[nodiscard]] const std::string& GetName() const { return m_Name; }

        /**
         * @brief Set up the framebuffer for this pass with the specified dimensions.
         * @param width The width of the framebuffer
         * @param height The height of the framebuffer
         */
        virtual void SetupFramebuffer(u32 width, u32 height) = 0;
        
        /**
         * @brief Resize the framebuffer when the window is resized.
         * @param width The new width of the framebuffer
         * @param height The new height of the framebuffer
         */
        virtual void ResizeFramebuffer(u32 width, u32 height) = 0;

        /**
         * @brief Called when the render pass needs to be reset.
         * This might happen after significant renderer changes or window resize events.
         */
        virtual void OnReset() = 0;
        
        /**
         * @brief Reset the command bucket to prepare for a new frame.
         */
        void ResetCommandBucket() { m_CommandBucket.Reset(); }
        
        /**
         * @brief Set the command allocator to use for this render pass.
         * @param allocator The command allocator to use
         */
        void SetCommandAllocator(CommandAllocator* allocator) { m_Allocator = allocator; }
        
        /**
         * @brief Submit a command to the pass's command bucket.
         * @param command The command to submit
         * @param commandAllocator The allocator to use for command memory
         * @return True if the command was successfully submitted
         */
        bool SubmitCommand(const void* command, CommandType type, CommandAllocator& commandAllocator)
        {
            if (!m_Allocator)
            {
                m_Allocator = &commandAllocator;
            }
            
            return m_CommandBucket.AddCommand(command, type, *m_Allocator);
        }

    protected:
		std::string m_Name = "CommandRenderPass";
		Ref<Framebuffer> m_Target;
		FramebufferSpecification m_FramebufferSpec;

		CommandBucket m_CommandBucket;
		CommandAllocator* m_Allocator = nullptr;
    };
}