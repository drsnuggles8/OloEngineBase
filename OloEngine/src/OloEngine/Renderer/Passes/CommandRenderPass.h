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
    class CommandRenderPass : public RenderPass
    {
    public:
        CommandRenderPass();
        ~CommandRenderPass() override;

        /**
         * @brief Execute the render pass using the command bucket system.
         * This method implements the base Execute method from RenderPass.
         */
        void Execute() override;

    protected:
        /**
         * @brief Populate the command bucket with rendering commands.
         * Subclasses must implement this to define their specific rendering logic.
         * @param bucket Reference to the command bucket to populate
         */
        virtual void BuildCommandBucket(CommandBucket& bucket) = 0;

        /**
         * @brief Begin rendering to the target framebuffer.
         * Sets up the framebuffer and view/projection matrices for rendering.
         */
        virtual void BeginRender();

        /**
         * @brief End rendering to the target framebuffer.
         * Performs any necessary cleanup after rendering.
         */
        virtual void EndRender();

    protected:
        // Command bucket for storing rendering commands
        CommandBucket m_CommandBucket;
        
        // Command allocator for allocating command memory
        CommandAllocator* m_Allocator = nullptr;
    };
}