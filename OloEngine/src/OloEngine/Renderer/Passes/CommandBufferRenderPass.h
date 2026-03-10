#pragma once

#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"

namespace OloEngine
{
    // @brief Render pass that owns a command bucket for deferred command recording and execution.
    //
    // Only passes that participate in the sort-and-dispatch pipeline (e.g., SceneRenderPass,
    // and eventually ShadowRenderPass) should inherit from this class. Fullscreen
    // passes and callback-driven passes should inherit from plain RenderPass.
    class CommandBufferRenderPass : public RenderPass
    {
      public:
        CommandBufferRenderPass()
        {
            m_OwnedAllocator = CreateScope<CommandAllocator>();
            m_Allocator = m_OwnedAllocator.get();

            // Disable batching until shaders support instanced drawing
            // (u_ModelMatrices[] / gl_InstanceID). Individual render passes
            // can re-enable it once their shader pipelines are ready.
            CommandBucketConfig config;
            config.EnableBatching = false;
            m_CommandBucket = CommandBucket(config);
        }

        void ResetCommandBucket()
        {
            OLO_CORE_ASSERT(m_Allocator, "CommandBufferRenderPass::ResetCommandBucket: No allocator available!");
            m_CommandBucket.Reset(*m_Allocator);
        }

        void SetCommandAllocator(CommandAllocator* allocator)
        {
            m_Allocator = allocator ? allocator : m_OwnedAllocator.get();
        }

        void SubmitPacket(CommandPacket* packet)
        {
            m_CommandBucket.SubmitPacket(packet);
        }

        CommandBucket& GetCommandBucket()
        {
            return m_CommandBucket;
        }

      protected:
        CommandBucket m_CommandBucket;
        Scope<CommandAllocator> m_OwnedAllocator;
        CommandAllocator* m_Allocator = nullptr;
    };
} // namespace OloEngine
