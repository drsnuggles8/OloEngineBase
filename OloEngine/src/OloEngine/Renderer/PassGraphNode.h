#pragma once

#include "OloEngine/Renderer/Passes/CommandBufferRenderPass.h"
#include "OloEngine/Renderer/RenderGraphNode.h"

#include <functional>
#include <string>
#include <utility>

namespace OloEngine
{
    class PassGraphNode : public RenderGraphNode
    {
      public:
        using SetupCallback = std::function<void(RGBuilder&, FrameBlackboard&)>;

        PassGraphNode(std::string name,
                      const Ref<RenderPass>& pass,
                      SetupCallback setup,
                      RenderGraphNodeFlags flags = RenderGraphNodeFlags::Graphics)
            : PassGraphNode(std::move(name),
                            pass,
                            pass ? pass.As<CommandBufferRenderPass>() : Ref<CommandBufferRenderPass>{},
                            std::move(setup),
                            flags)
        {
        }

        PassGraphNode(std::string name,
                      const Ref<CommandBufferRenderPass>& pass,
                      SetupCallback setup,
                      RenderGraphNodeFlags flags = RenderGraphNodeFlags::Graphics |
                                                   RenderGraphNodeFlags::UsesCommandBucket)
            : PassGraphNode(std::move(name),
                            pass ? pass.As<RenderPass>() : Ref<RenderPass>{},
                            pass,
                            std::move(setup),
                            flags)
        {
        }

        [[nodiscard]] std::string_view GetName() const override
        {
            return m_Name;
        }

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override
        {
            if (m_Setup)
                m_Setup(builder, blackboard);
        }

        void Execute(RGCommandContext& context) override
        {
            OLO_CORE_ASSERT(m_Pass, "PassGraphNode::Execute requires a valid pass");
            m_Pass->Execute(context);
        }

        [[nodiscard]] RenderGraphNodeFlags GetFlags() const override
        {
            return m_Flags;
        }

        [[nodiscard]] RenderPass::SubmissionModel GetSubmissionModel() const override
        {
            return m_Pass ? m_Pass->GetSubmissionModel() : RenderGraphNode::GetSubmissionModel();
        }

        void SetupFramebuffer(u32 width, u32 height) override
        {
            if (m_Pass)
                m_Pass->SetupFramebuffer(width, height);
        }

        void ResizeFramebuffer(u32 width, u32 height) override
        {
            if (m_Pass)
                m_Pass->ResizeFramebuffer(width, height);
        }

        void ApplyRenderViewport(u32 width, u32 height) override
        {
            if (m_Pass)
                m_Pass->ApplyRenderViewport(width, height);
        }

        template<typename T>
        CommandPacket* CreateDrawCall()
        {
            return GetCommandBucket().CreateDrawCall<T>();
        }

        void SubmitPacket(CommandPacket* packet)
        {
            GetCommandBufferPass()->SubmitPacket(packet);
        }

        void ResetCommandBucket()
        {
            GetCommandBufferPass()->ResetCommandBucket();
        }

        void SetCommandAllocator(CommandAllocator* allocator)
        {
            GetCommandBufferPass()->SetCommandAllocator(allocator);
        }

        [[nodiscard]] CommandBucket& GetCommandBucket()
        {
            return GetCommandBufferPass()->GetCommandBucket();
        }

        [[nodiscard]] bool HasCommandBucket() const
        {
            return static_cast<bool>(m_CommandBufferPass);
        }

        [[nodiscard]] const Ref<RenderPass>& GetPass() const
        {
            return m_Pass;
        }

      private:
        PassGraphNode(std::string name,
                      const Ref<RenderPass>& pass,
                      const Ref<CommandBufferRenderPass>& commandBufferPass,
                      SetupCallback setup,
                      RenderGraphNodeFlags flags)
            : m_Name(std::move(name)),
              m_Pass(pass),
              m_CommandBufferPass(commandBufferPass),
              m_Setup(std::move(setup)),
              m_Flags(flags)
        {
            OLO_CORE_ASSERT(!m_Name.empty(), "PassGraphNode requires a name");
            OLO_CORE_ASSERT(m_Pass, "PassGraphNode requires a valid pass");
        }

        [[nodiscard]] CommandBufferRenderPass* GetCommandBufferPass()
        {
            OLO_CORE_ASSERT(m_CommandBufferPass, "PassGraphNode requires a CommandBufferRenderPass backing for bucket operations");
            return m_CommandBufferPass.Raw();
        }

        std::string m_Name;
        Ref<RenderPass> m_Pass;
        Ref<CommandBufferRenderPass> m_CommandBufferPass;
        SetupCallback m_Setup;
        RenderGraphNodeFlags m_Flags = RenderGraphNodeFlags::Graphics;
    };
} // namespace OloEngine
