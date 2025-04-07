#include "OloEnginePCH.h"
#include "CommandRenderPass.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/RenderCommand.h"

namespace OloEngine
{
    CommandRenderPass::CommandRenderPass()
        : m_CommandBucket(CommandBucketConfig())
    {
        OLO_CORE_INFO("Creating CommandRenderPass");
    }

    CommandRenderPass::~CommandRenderPass()
    {
        // Return the allocator if one was assigned
        if (m_Allocator)
        {
            CommandMemoryManager::ReturnAllocator(m_Allocator);
            m_Allocator = nullptr;
        }
    }

    void CommandRenderPass::Execute()
    {
        OLO_PROFILE_FUNCTION();
        
        // Get a fresh allocator for this frame
        if (m_Allocator)
        {
            m_CommandBucket.Reset(*m_Allocator);
            CommandMemoryManager::ReturnAllocator(m_Allocator);
        }
        
        m_Allocator = CommandMemoryManager::GetFrameAllocator();
        if (!m_Allocator)
        {
            OLO_CORE_ERROR("CommandRenderPass::Execute: Failed to get frame allocator!");
            return;
        }

        // Start rendering to the target framebuffer
        BeginRender();

        // Build the command bucket with rendering commands
        BuildCommandBucket(m_CommandBucket);

        // Sort and batch commands for optimal rendering
        m_CommandBucket.SortCommands();
        m_CommandBucket.BatchCommands(*m_Allocator);

        // Execute all commands in the bucket using the renderer API directly
        m_CommandBucket.Execute(RenderCommand::GetRendererAPI());

        // End rendering
        EndRender();

        // Log statistics if verbose logging is enabled
        const auto stats = m_CommandBucket.GetStatistics();
        OLO_CORE_TRACE(
            "{}: Commands[{}] Batched[{}] DrawCalls[{}] StateChanges[{}]", 
            m_Name, stats.TotalCommands, stats.BatchedCommands, 
            stats.DrawCalls, stats.StateChanges
        );
    }

    void CommandRenderPass::BeginRender()
    {
        // Only bind the framebuffer if we have one
        // (FinalRenderPass might render to the default framebuffer)
        if (m_Target)
        {
            m_Target->Bind();
        }
    }

    void CommandRenderPass::EndRender()
    {
        // Unbind the framebuffer if we have one
        if (m_Target)
        {
            m_Target->Unbind();
        }
    }
}