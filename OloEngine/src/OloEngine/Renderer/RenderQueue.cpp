#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderQueue.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderCommand.h"

namespace OloEngine
{
    Scope<RenderQueue::SceneData> RenderQueue::s_SceneData = CreateScope<RenderQueue::SceneData>();
    std::vector<std::unique_ptr<RenderCommandBase>> RenderQueue::s_CommandQueue;
    RenderQueue::Statistics RenderQueue::s_Stats;

    void RenderQueue::Init()
    {
        OLO_INFO("Initializing RenderQueue");
        s_SceneData = CreateScope<SceneData>();
        s_CommandQueue.reserve(1000); // Pre-allocate space for commands
        OLO_INFO("RenderQueue initialized with capacity for 1000 commands");
    }

    void RenderQueue::Shutdown()
    {
        OLO_INFO("Shutting down RenderQueue");
        s_CommandQueue.clear();
        s_SceneData.reset();
        OLO_INFO("RenderQueue shutdown complete");
    }

    void RenderQueue::BeginScene(const glm::mat4& viewProjectionMatrix)
    {
        OLO_TRACE("RenderQueue::BeginScene");
        s_SceneData->ViewProjectionMatrix = viewProjectionMatrix;
        s_CommandQueue.clear(); // Clear any leftover commands
        s_Stats = Statistics(); // Reset statistics
    }

    void RenderQueue::EndScene()
    {
        OLO_TRACE("RenderQueue::EndScene");
        Flush();
    }

    void RenderQueue::SubmitMesh(const Ref<Mesh>& mesh, const glm::mat4& transform, const Material& material)
    {
        OLO_TRACE("RenderQueue::SubmitMesh - Adding mesh command to queue");
        s_CommandQueue.push_back(std::make_unique<DrawMeshCommand>(mesh, transform, material));
        s_Stats.CommandCount++;
        OLO_TRACE("RenderQueue::SubmitMesh - Current command count: {}", s_Stats.CommandCount);
    }

    void RenderQueue::SubmitQuad(const glm::mat4& transform, const Ref<Texture2D>& texture)
    {
        OLO_TRACE("RenderQueue::SubmitQuad - Adding quad command to queue");
        s_CommandQueue.push_back(std::make_unique<DrawQuadCommand>(transform, texture));
        s_Stats.CommandCount++;
        OLO_TRACE("RenderQueue::SubmitQuad - Current command count: {}", s_Stats.CommandCount);
    }

    void RenderQueue::Flush()
    {
        OLO_INFO("RenderQueue::Flush - Starting flush with {} commands", s_Stats.CommandCount);
        
        if (s_CommandQueue.empty())
        {
            OLO_WARN("RenderQueue::Flush - No commands to execute!");
            return;
        }

        SortCommands();
        ExecuteCommands();
        
        OLO_INFO("RenderQueue::Flush - Executed {} draw calls", s_Stats.DrawCalls);
        s_CommandQueue.clear();
        s_Stats.CommandCount = 0;
    }

    void RenderQueue::SortCommands()
    {
        OLO_TRACE("RenderQueue::SortCommands - Sorting {} commands", s_CommandQueue.size());
        // TODO: Implement command sorting based on:
        // 1. Shader program
        // 2. Material properties
        // 3. Texture bindings
        // 4. Depth sorting for transparent objects
    }

    void RenderQueue::ExecuteCommands()
    {
        OLO_TRACE("RenderQueue::ExecuteCommands - Starting execution of {} commands", s_CommandQueue.size());
        
        for (const auto& command : s_CommandQueue)
        {
            command->Execute();
            s_Stats.DrawCalls++;
        }
        
        OLO_TRACE("RenderQueue::ExecuteCommands - Completed {} draw calls", s_Stats.DrawCalls);
    }

    void RenderQueue::ResetStats()
    {
        OLO_TRACE("RenderQueue::ResetStats");
        s_Stats = Statistics();
    }

    RenderQueue::Statistics RenderQueue::GetStats()
    {
        return s_Stats;
    }

    // DrawMeshCommand implementation
    void DrawMeshCommand::Execute()
    {
        OLO_TRACE("DrawMeshCommand::Execute - Executing mesh command");
        Renderer3D::RenderMeshInternal(m_Mesh, m_Transform, m_Material);
    }

    // DrawQuadCommand implementation
    void DrawQuadCommand::Execute()
    {
        OLO_TRACE("DrawQuadCommand::Execute - Executing quad command");
        Renderer3D::RenderQuadInternal(m_Transform, m_Texture);
    }
} 