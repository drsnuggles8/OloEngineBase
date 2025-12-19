#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"
#include "OloEngine/Renderer/Commands/CommandMemoryManager.h"

namespace OloEngine
{
    // @brief Base class for render passes that use the command-based rendering system.
    // 
    // This class integrates with the command bucket system,
    // allowing render passes to efficiently build and execute rendering commands.
    class RenderPass : public RefCounted
    {
    public:
        RenderPass()
        {
            // Create owned allocator with default block size
            m_OwnedAllocator = CreateScope<CommandAllocator>();
            m_Allocator = m_OwnedAllocator.get();
        }
        
        virtual ~RenderPass() = default;

        virtual void Init(const FramebufferSpecification& spec) = 0;
        virtual void Execute() = 0;
        [[nodiscard]] virtual Ref<Framebuffer> GetTarget() const = 0;

        void SetName(std::string_view name) { m_Name = name; }
        [[nodiscard]] const std::string& GetName() const { return m_Name; }

        virtual void SetupFramebuffer(u32 width, u32 height) = 0;
        virtual void ResizeFramebuffer(u32 width, u32 height) = 0;
        virtual void OnReset() = 0;

        void ResetCommandBucket() 
        { 
            OLO_CORE_ASSERT(m_Allocator, "RenderPass::ResetCommandBucket: No allocator available!");
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
		
        CommandBucket& GetCommandBucket() { return m_CommandBucket; }
    protected:
        std::string m_Name = "RenderPass";
        Ref<Framebuffer> m_Target;
        FramebufferSpecification m_FramebufferSpec;
        CommandBucket m_CommandBucket;
        Scope<CommandAllocator> m_OwnedAllocator;
        CommandAllocator* m_Allocator = nullptr;
    };
}
