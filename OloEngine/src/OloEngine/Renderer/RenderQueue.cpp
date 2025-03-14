#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderQueue.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderCommand.h"

namespace OloEngine
{
    Scope<RenderQueue::SceneData> RenderQueue::s_SceneData = CreateScope<RenderQueue::SceneData>();
    std::vector<Ref<RenderCommandBase>> RenderQueue::s_CommandQueue;
    std::queue<Ref<DrawMeshCommand>> RenderQueue::s_MeshCommandPool;
    std::queue<Ref<DrawQuadCommand>> RenderQueue::s_QuadCommandPool;
    RenderQueue::Statistics RenderQueue::s_Stats;
    RenderQueue::Config RenderQueue::s_Config;

    void RenderQueue::Init(const Config& config)
    {
        s_Config = config;
        s_SceneData = CreateScope<SceneData>();
        s_CommandQueue.reserve(s_Config.CommandQueueReserve);
        
        for (size_t i = 0; i < s_Config.InitialPoolSize; ++i)
        {
            s_MeshCommandPool.push(CreateRef<DrawMeshCommand>());
            s_QuadCommandPool.push(CreateRef<DrawQuadCommand>());
        }
    }

    void RenderQueue::Shutdown()
    {
        s_CommandQueue.clear();
        s_SceneData.reset();
        
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

    void RenderQueue::GrowCommandPool(CommandType type)
    {
        if (type == CommandType::Mesh && s_MeshCommandPool.size() < s_Config.MaxPoolSize)
        {
            s_MeshCommandPool.push(CreateRef<DrawMeshCommand>());
        }
        else if (type == CommandType::Quad && s_QuadCommandPool.size() < s_Config.MaxPoolSize)
        {
            s_QuadCommandPool.push(CreateRef<DrawQuadCommand>());
        }
    }

    Ref<RenderCommandBase> RenderQueue::GetCommandFromPool(CommandType type)
    {
        switch (type)
        {
            case CommandType::Mesh:
            {
                if (!s_MeshCommandPool.empty())
                {
                    auto command = s_MeshCommandPool.front();
                    s_MeshCommandPool.pop();
                    s_Stats.PoolHits++;
                    return command;
                }
                s_Stats.PoolMisses++;
                GrowCommandPool(type);
                return CreateRef<DrawMeshCommand>();
            }
            
            case CommandType::Quad:
            {
                if (!s_QuadCommandPool.empty())
                {
                    auto command = s_QuadCommandPool.front();
                    s_QuadCommandPool.pop();
                    s_Stats.PoolHits++;
                    return command;
                }
                s_Stats.PoolMisses++;
                GrowCommandPool(type);
                return CreateRef<DrawQuadCommand>();
            }
            
            default:
                return nullptr;
        }
    }

    void RenderQueue::ReturnCommandToPool(Ref<RenderCommandBase>&& command)
    {
        if (!command)
            return;

        command->Reset();

        switch (command->GetType())
        {
            case CommandType::Mesh:
                s_MeshCommandPool.push(std::static_pointer_cast<DrawMeshCommand>(command));
                break;
            
            case CommandType::Quad:
                s_QuadCommandPool.push(std::static_pointer_cast<DrawQuadCommand>(command));
                break;
            
            default:
                break;
        }
    }

    void RenderQueue::SubmitMesh(const Ref<Mesh>& mesh, const glm::mat4& transform, const Material& material)
    {
        auto command = GetCommandFromPool(CommandType::Mesh);
        if (auto meshCommand = std::static_pointer_cast<DrawMeshCommand>(command))
        {
            meshCommand->Set(mesh, transform, material);
            s_CommandQueue.push_back(std::move(command));
            s_Stats.CommandCount++;
        }
    }

    void RenderQueue::SubmitQuad(const glm::mat4& transform, const Ref<Texture2D>& texture)
    {
        auto command = GetCommandFromPool(CommandType::Quad);
        if (auto quadCommand = std::static_pointer_cast<DrawQuadCommand>(command))
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

        if (s_Config.EnableSorting)
        {
            SortCommands();
        }

        if (s_Config.EnableBatching || s_Config.EnableMerging)
        {
            BatchCommands();
        }

        ExecuteCommands();
        
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
            [](const Ref<RenderCommandBase>& a, const Ref<RenderCommandBase>& b) {
                if (a->GetType() != b->GetType())
                    return a->GetType() < b->GetType();
                
                if (a->GetShaderKey() != b->GetShaderKey())
                    return a->GetShaderKey() < b->GetShaderKey();
                
                if (a->GetMaterialKey() != b->GetMaterialKey())
                    return a->GetMaterialKey() < b->GetMaterialKey();
                
                return a->GetTextureKey() < b->GetTextureKey();
            });
    }

    void RenderQueue::BatchCommands()
    {
        if (s_CommandQueue.empty())
            return;

        std::vector<Ref<RenderCommandBase>> batchedCommands;
        batchedCommands.reserve(s_CommandQueue.size());

        for (size_t i = 0; i < s_CommandQueue.size();)
        {
            auto current = s_CommandQueue[i];
            size_t batchSize = 1;

            // Try to merge with subsequent commands
            while (batchSize < s_Config.MaxBatchSize && i + batchSize < s_CommandQueue.size())
            {
                auto next = s_CommandQueue[i + batchSize];
                if (!current->CanBatchWith(*next))
                    break;

                if (s_Config.EnableMerging && current->MergeWith(*next))
                {
                    s_Stats.MergedCommands++;
                    batchSize++;
                }
                else
                {
                    break;
                }
            }

            if (batchSize > 1)
            {
                s_Stats.BatchedCommands++;
            }

            batchedCommands.push_back(std::move(current));
            i += batchSize;
        }

        s_CommandQueue = std::move(batchedCommands);
    }

    void RenderQueue::ExecuteCommands()
    {
        uint64_t currentShaderKey = 0;
        uint64_t currentMaterialKey = 0;
        uint64_t currentTextureKey = 0;
        
        for (const auto& command : s_CommandQueue)
        {
            bool stateChanged = false;
            
            if (command->GetShaderKey() != currentShaderKey)
            {
                currentShaderKey = command->GetShaderKey();
                stateChanged = true;
            }
            
            if (command->GetMaterialKey() != currentMaterialKey)
            {
                currentMaterialKey = command->GetMaterialKey();
                stateChanged = true;
            }
            
            if (command->GetTextureKey() != currentTextureKey)
            {
                currentTextureKey = command->GetTextureKey();
                stateChanged = true;
            }
            
            if (stateChanged)
            {
                s_Stats.StateChanges++;
            }
            
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
        return 0;
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

    bool DrawMeshCommand::CanBatchWith(const RenderCommandBase& other) const
    {
        if (other.GetType() != CommandType::Mesh)
            return false;

        const auto& otherMesh = static_cast<const DrawMeshCommand&>(other);
        return m_Mesh == otherMesh.m_Mesh && 
               m_Material == otherMesh.m_Material;
    }

    bool DrawMeshCommand::MergeWith(const RenderCommandBase& other)
    {
        if (!CanBatchWith(other))
            return false;

        const auto& otherMesh = static_cast<const DrawMeshCommand&>(other);
        m_BatchSize += otherMesh.m_BatchSize;
        return true;
    }

    void DrawQuadCommand::Execute()
    {
        Renderer3D::RenderQuadInternal(m_Transform, m_Texture);
    }

    uint64_t DrawQuadCommand::GetShaderKey() const
    {
        return 1;
    }

    uint64_t DrawQuadCommand::GetMaterialKey() const
    {
        return 0;
    }

    uint64_t DrawQuadCommand::GetTextureKey() const
    {
        return m_Texture ? std::hash<uint32_t>{}(m_Texture->GetRendererID()) : 0;
    }

    bool DrawQuadCommand::CanBatchWith(const RenderCommandBase& other) const
    {
        if (other.GetType() != CommandType::Quad)
            return false;

        const auto& otherQuad = static_cast<const DrawQuadCommand&>(other);
        return m_Texture == otherQuad.m_Texture;
    }

    bool DrawQuadCommand::MergeWith(const RenderCommandBase& other)
    {
        if (!CanBatchWith(other))
            return false;

        const auto& otherQuad = static_cast<const DrawQuadCommand&>(other);
        m_BatchSize += otherQuad.m_BatchSize;
        return true;
    }
} 
