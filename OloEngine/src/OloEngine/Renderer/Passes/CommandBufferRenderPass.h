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

            // Batching is on: same-mesh-same-material DrawMesh commands collapse
            // into a single DrawMeshInstanced packet that uploads N transforms
            // into the ModelInstanceBuffer SSBO (binding 15) and issues one
            // glDrawElementsInstanced call. Vertex shaders read each instance
            // via `instances[gl_InstanceIndex]` (see InstanceBlock_Vertex.glsl).
            CommandBucketConfig config;
            m_CommandBucket = CommandBucket(config);
            // The bucket's internal `m_Allocator` is the one consumed by
            // `CommandBucket::CreateDrawCall<T>()` / `BatchCommands` / etc.
            // It is independent of `CommandBufferRenderPass::m_Allocator`, so
            // it must be kept in sync — both at construction and whenever
            // `SetCommandAllocator` is called.
            m_CommandBucket.SetAllocator(m_Allocator);
        }

        // Retire this pass's bucket. The allocator is reset only when this pass
        // OWNS it. Render-stream passes share FrameResourceManager's per-frame
        // allocator (RenderPipeline sets it on every stream node), and that
        // allocator is reset once per frame by its owner: a pass that reset it
        // here would free every OTHER stream's packets that have not replayed
        // yet, and the next allocation from it would build a new packet on top
        // of one of them. Forward frames did exactly that to the decal bucket,
        // which an earlier stream pass retired before DecalRenderPass replayed.
        void ResetCommandBucket()
        {
            OLO_CORE_ASSERT(m_Allocator, "CommandBufferRenderPass::ResetCommandBucket: No allocator available!");
            if (m_Allocator == m_OwnedAllocator.get())
            {
                m_CommandBucket.Reset(*m_Allocator);
            }
            else
            {
                // Everything Reset does except the allocator: the bucket's
                // statistics are per frame, and tests and the profiler read them.
                m_CommandBucket.Clear();
                m_CommandBucket.ResetStatistics();
            }
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

        // Whether anything was submitted to this pass this frame. A pass whose
        // Setup() declares nothing without work gates on THIS and reports the
        // same call from AppendDeclarationInputs, so its gate and its key are
        // one value (issues #1315, #1333). A boolean on purpose: the count would
        // rebuild the graph every time one more draw arrived.
        [[nodiscard]] bool HasSubmittedCommands() const
        {
            return m_CommandBucket.GetCommandCount() > 0u;
        }

      protected:
        CommandBucket m_CommandBucket;
        Scope<CommandAllocator> m_OwnedAllocator;
        CommandAllocator* m_Allocator = nullptr;
    };
} // namespace OloEngine
