#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderQueue.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderCommand.h"

namespace OloEngine
{
    Scope<RenderQueue::SceneData> RenderQueue::s_SceneData = CreateScope<RenderQueue::SceneData>();
    std::vector<std::unique_ptr<RenderCommandBase>> RenderQueue::s_CommandQueue;
    std::queue<std::unique_ptr<DrawMeshCommand>> RenderQueue::s_MeshCommandPool;
    std::queue<std::unique_ptr<DrawQuadCommand>> RenderQueue::s_QuadCommandPool;
    RenderQueue::Statistics RenderQueue::s_Stats;

    void RenderQueue::Init()
    {
        s_SceneData = CreateScope<SceneData>();
        s_CommandQueue.reserve(1000);
        
        // Pre-allocate some commands for the pool
        for (int i = 0; i < 100; ++i)
        {
            s_MeshCommandPool.push(std::make_unique<DrawMeshCommand>());
            s_QuadCommandPool.push(std::make_unique<DrawQuadCommand>());
        }
    }

    void RenderQueue::Shutdown()
    {
        s_CommandQueue.clear();
        s_SceneData.reset();
        
        // Clear command pools
        while (!s_MeshCommandPool.empty())
            s_MeshCommandPool.pop();
        while (!s_QuadCommandPool.empty())
            s_QuadCommandPool.pop();
    }

    void RenderQueue::BeginScene(const glm::mat4& viewProjectionMatrix)
    {
        s_SceneData->ViewProjectionMatrix = viewProjectionMatrix;
        s_CommandQueue.clear();
        s_Stats = Statistics();
    }

    void RenderQueue::EndScene()
    {
        Flush();
    }

    std::unique_ptr<RenderCommandBase> RenderQueue::GetCommandFromPool(CommandType type)
    {
        switch (type)
        {
            case CommandType::Mesh:
            {
                if (!s_MeshCommandPool.empty())
                {
                    auto command = std::move(s_MeshCommandPool.front());
                    s_MeshCommandPool.pop();
                    s_Stats.PoolHits++;
                    return command;
                }
                s_Stats.PoolMisses++;
                return std::make_unique<DrawMeshCommand>();
            }
            
            case CommandType::Quad:
            {
                if (!s_QuadCommandPool.empty())
                {
                    auto command = std::move(s_QuadCommandPool.front());
                    s_QuadCommandPool.pop();
                    s_Stats.PoolHits++;
                    return command;
                }
                s_Stats.PoolMisses++;
                return std::make_unique<DrawQuadCommand>();
            }
            
            default:
                return nullptr;
        }
    }

    void RenderQueue::ReturnCommandToPool(std::unique_ptr<RenderCommandBase>&& command)
    {
        if (!command)
            return;

        command->Reset();

        switch (command->GetType())
        {
            case CommandType::Mesh:
                s_MeshCommandPool.push(std::unique_ptr<DrawMeshCommand>(static_cast<DrawMeshCommand*>(command.release())));
                break;
            
            case CommandType::Quad:
                s_QuadCommandPool.push(std::unique_ptr<DrawQuadCommand>(static_cast<DrawQuadCommand*>(command.release())));
                break;
            
            default:
                break;
        }
    }

    void RenderQueue::SubmitMesh(const Ref<Mesh>& mesh, const glm::mat4& transform, const Material& material)
    {
        auto command = GetCommandFromPool(CommandType::Mesh);
        if (auto meshCommand = static_cast<DrawMeshCommand*>(command.get()))
        {
            meshCommand->Set(mesh, transform, material);
            s_CommandQueue.push_back(std::move(command));
            s_Stats.CommandCount++;
        }
    }

    void RenderQueue::SubmitQuad(const glm::mat4& transform, const Ref<Texture2D>& texture)
    {
        auto command = GetCommandFromPool(CommandType::Quad);
        if (auto quadCommand = static_cast<DrawQuadCommand*>(command.get()))
        {
            quadCommand->Set(transform, texture);
            s_CommandQueue.push_back(std::move(command));
            s_Stats.CommandCount++;
        }
    }

    void RenderQueue::Flush()
    {
        if (s_CommandQueue.empty())
            return;

        SortCommands();
        ExecuteCommands();
        
        // Return all commands to their respective pools
        for (auto& command : s_CommandQueue)
        {
            ReturnCommandToPool(std::move(command));
        }
        
        s_CommandQueue.clear();
        s_Stats.CommandCount = 0;
    }

    void RenderQueue::SortCommands()
    {
        std::sort(s_CommandQueue.begin(), s_CommandQueue.end(),
            [](const std::unique_ptr<RenderCommandBase>& a, const std::unique_ptr<RenderCommandBase>& b) {
                if (a->GetType() != b->GetType())
                    return a->GetType() < b->GetType();
                
                if (a->GetShaderKey() != b->GetShaderKey())
                    return a->GetShaderKey() < b->GetShaderKey();
                
                if (a->GetMaterialKey() != b->GetMaterialKey())
                    return a->GetMaterialKey() < b->GetMaterialKey();
                
                return a->GetTextureKey() < b->GetTextureKey();
            });
    }

    void RenderQueue::ExecuteCommands()
    {
        uint64_t currentShaderKey = 0;
        uint64_t currentMaterialKey = 0;
        uint64_t currentTextureKey = 0;
        
        for (const auto& command : s_CommandQueue)
        {
            if (command->GetShaderKey() != currentShaderKey)
                currentShaderKey = command->GetShaderKey();
            
            if (command->GetMaterialKey() != currentMaterialKey)
                currentMaterialKey = command->GetMaterialKey();
            
            if (command->GetTextureKey() != currentTextureKey)
                currentTextureKey = command->GetTextureKey();
            
            command->Execute();
            s_Stats.DrawCalls++;
        }
    }

    void RenderQueue::ResetStats()
    {
        s_Stats = Statistics();
    }

    RenderQueue::Statistics RenderQueue::GetStats()
    {
        return s_Stats;
    }

    void DrawMeshCommand::Execute()
    {
        Renderer3D::RenderMeshInternal(m_Mesh, m_Transform, m_Material);
    }

    uint64_t DrawMeshCommand::GetShaderKey() const
    {
        return 0; // Lighting shader ID
    }

    uint64_t DrawMeshCommand::GetMaterialKey() const
    {
        uint64_t key = 0;
        
        key ^= std::hash<float>{}(m_Material.Ambient.x);
        key ^= std::hash<float>{}(m_Material.Ambient.y);
        key ^= std::hash<float>{}(m_Material.Ambient.z);
        
        key ^= std::hash<float>{}(m_Material.Diffuse.x);
        key ^= std::hash<float>{}(m_Material.Diffuse.y);
        key ^= std::hash<float>{}(m_Material.Diffuse.z);
        
        key ^= std::hash<float>{}(m_Material.Specular.x);
        key ^= std::hash<float>{}(m_Material.Specular.y);
        key ^= std::hash<float>{}(m_Material.Specular.z);
        key ^= std::hash<float>{}(m_Material.Shininess);
        
        key ^= std::hash<bool>{}(m_Material.UseTextureMaps);
        
        return key;
    }

    uint64_t DrawMeshCommand::GetTextureKey() const
    {
        uint64_t key = 0;
        
        if (m_Material.UseTextureMaps)
        {
            if (m_Material.DiffuseMap)
                key ^= std::hash<uint32_t>{}(m_Material.DiffuseMap->GetRendererID());
            if (m_Material.SpecularMap)
                key ^= std::hash<uint32_t>{}(m_Material.SpecularMap->GetRendererID());
        }
        
        return key;
    }

    void DrawQuadCommand::Execute()
    {
        Renderer3D::RenderQuadInternal(m_Transform, m_Texture);
    }

    uint64_t DrawQuadCommand::GetShaderKey() const
    {
        return 1; // Quad shader ID
    }

    uint64_t DrawQuadCommand::GetMaterialKey() const
    {
        return 0;
    }

    uint64_t DrawQuadCommand::GetTextureKey() const
    {
        return m_Texture ? std::hash<uint32_t>{}(m_Texture->GetRendererID()) : 0;
    }
} 
