#pragma once

#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"

namespace OloEngine
{
    // @brief Render pass that owns a command bucket for deferred command recording and execution.
    //
    // Only passes that participate in the sort-and-dispatch pipeline (e.g., SceneRenderPass,
    // and eventually ShadowRenderPass) should inherit from this class. Fullscreen
    // passes and callback-driven passes should inherit from plain RenderPass.
    class CommandBufferRenderPass : public RenderGraphNode
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
            // The bucket's internal `m_Allocator` is the one consumed by
            // `CommandBucket::CreateDrawCall<T>()` / `BatchCommands` / etc.
            // It is independent of `CommandBufferRenderPass::m_Allocator`, so
            // it must be kept in sync — both at construction and whenever
            // `SetCommandAllocator` is called.
            m_CommandBucket.SetAllocator(m_Allocator);
        }

        void ResetCommandBucket()
        {
            OLO_CORE_ASSERT(m_Allocator, "CommandBufferRenderPass::ResetCommandBucket: No allocator available!");
            m_CommandBucket.Reset(*m_Allocator);
        }

        void SetCommandAllocator(CommandAllocator* allocator)
        {
            m_Allocator = allocator ? allocator : m_OwnedAllocator.get();
            // Keep the bucket-side allocator in sync — it is what
            // `m_CommandBucket.CreateDrawCall<T>()` calls into.
            m_CommandBucket.SetAllocator(m_Allocator);
        }

        void SubmitPacket(CommandPacket* packet)
        {
            m_CommandBucket.SubmitPacket(packet);
        }

        [[nodiscard]] RenderGraphNodeFlags GetFlags() const override
        {
            return RenderGraphNode::GetFlags() | RenderGraphNodeFlags::UsesCommandBucket;
        }

        template<typename T>
        CommandPacket* CreateDrawCall()
        {
            return m_CommandBucket.CreateDrawCall<T>();
        }

        CommandBucket& GetCommandBucket()
        {
            return m_CommandBucket;
        }

        const CommandBucket& GetCommandBucket() const
        {
            return m_CommandBucket;
        }

      protected:
        CommandBucket m_CommandBucket;
        Scope<CommandAllocator> m_OwnedAllocator;
        CommandAllocator* m_Allocator = nullptr;
    };
} // namespace OloEngine
