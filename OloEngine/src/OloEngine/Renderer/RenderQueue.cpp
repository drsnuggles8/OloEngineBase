#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderQueue.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderCommand.h"

namespace OloEngine
{
    Scope<RenderQueue::SceneData> RenderQueue::s_SceneData = CreateScope<RenderQueue::SceneData>();
    std::vector<Ref<RenderCommandBase>> RenderQueue::s_CommandQueue;
    std::queue<Ref<LegacyDrawMeshCommand>> RenderQueue::s_MeshCommandPool;
    std::queue<Ref<LegacyDrawQuadCommand>> RenderQueue::s_QuadCommandPool;
    std::queue<Ref<StateChangeCommand>> RenderQueue::s_StateCommandPool;
    RenderQueue::Statistics RenderQueue::s_Stats;
    RenderQueue::Config RenderQueue::s_Config;
    RenderState RenderQueue::s_CurrentState;

    void RenderQueue::Init(const Config& config)
    {
        s_Config = config;
        s_SceneData = CreateScope<SceneData>();
        s_CommandQueue.reserve(s_Config.CommandQueueReserve);
        
        for (sizet i = 0; i < s_Config.InitialPoolSize; ++i)
        {
            s_MeshCommandPool.push(CreateRef<LegacyDrawMeshCommand>());
            s_QuadCommandPool.push(CreateRef<LegacyDrawQuadCommand>());
            s_StateCommandPool.push(CreateRef<StateChangeCommand>());
        }
        
        // Initialize default state
        InitializeDefaultState();
    }

    void RenderQueue::Shutdown()
    {
        s_CommandQueue.clear();
        s_SceneData.reset();
        
        while (!s_MeshCommandPool.empty())
            s_MeshCommandPool.pop();
        while (!s_QuadCommandPool.empty())
            s_QuadCommandPool.pop();
        while (!s_StateCommandPool.empty())
            s_StateCommandPool.pop();
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

    void RenderQueue::GrowCommandPool(LegacyCommandType type)
    {
        switch (type)
        {
            case LegacyCommandType::Mesh:
                if (s_MeshCommandPool.size() < s_Config.MaxPoolSize)
                    s_MeshCommandPool.push(CreateRef<LegacyDrawMeshCommand>());
                break;
                
            case LegacyCommandType::Quad:
                if (s_QuadCommandPool.size() < s_Config.MaxPoolSize)
                    s_QuadCommandPool.push(CreateRef<LegacyDrawQuadCommand>());
                break;
                
            case LegacyCommandType::StateChange:
                if (s_StateCommandPool.size() < s_Config.MaxPoolSize)
                    s_StateCommandPool.push(CreateRef<StateChangeCommand>());
                break;
                
            default:
                break;
        }
    }

    Ref<RenderCommandBase> RenderQueue::GetCommandFromPool(LegacyCommandType type)
    {
        switch (type)
        {
            case LegacyCommandType::Mesh:
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
                return CreateRef<LegacyDrawMeshCommand>();
            }
            
            case LegacyCommandType::Quad:
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
                return CreateRef<LegacyDrawQuadCommand>();
            }
            
            case LegacyCommandType::StateChange:
            {
                if (!s_StateCommandPool.empty())
                {
                    auto command = s_StateCommandPool.front();
                    s_StateCommandPool.pop();
                    s_Stats.PoolHits++;
                    return command;
                }
                s_Stats.PoolMisses++;
                GrowCommandPool(type);
                return CreateRef<StateChangeCommand>();
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
            case LegacyCommandType::Mesh:
                s_MeshCommandPool.push(std::static_pointer_cast<LegacyDrawMeshCommand>(command));
                break;
            
            case LegacyCommandType::Quad:
                s_QuadCommandPool.push(std::static_pointer_cast<LegacyDrawQuadCommand>(command));
                break;
                
            case LegacyCommandType::StateChange:
                s_StateCommandPool.push(std::static_pointer_cast<StateChangeCommand>(command));
                break;
            
            default:
                break;
        }
    }

    void RenderQueue::SubmitMesh(const Ref<Mesh>& mesh, const glm::mat4& transform, const Material& material, bool isStatic)
    {
        auto command = GetCommandFromPool(LegacyCommandType::Mesh);
        if (auto meshCommand = std::static_pointer_cast<LegacyDrawMeshCommand>(command))
        {
            meshCommand->Set(mesh, transform, material, isStatic);
            s_CommandQueue.push_back(std::move(command));
            s_Stats.CommandCount++;
        }
    }

    void RenderQueue::SubmitQuad(const glm::mat4& transform, const Ref<Texture2D>& texture)
    {
        auto command = GetCommandFromPool(LegacyCommandType::Quad);
        if (auto quadCommand = std::static_pointer_cast<LegacyDrawQuadCommand>(command))
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
        s_Stats.StateCommandCount = 0;
    }

    void RenderQueue::SortCommands()
    {
        // First, let's partition the queue into groups that maintain their relative order
        std::vector<std::vector<Ref<RenderCommandBase>>> commandGroups;
        
        // Group by successive state changes followed by draw calls
        std::vector<Ref<RenderCommandBase>> currentGroup;
        LegacyCommandType lastType = LegacyCommandType::StateChange; // Start with state change as the initial group type
        
        for (const auto& cmd : s_CommandQueue)
        {
            // If we encounter a different command type than the last one, 
            // start a new group (except for the first command)
            if (!currentGroup.empty() && 
                ((lastType == LegacyCommandType::StateChange && cmd->GetType() != LegacyCommandType::StateChange) ||
                 (lastType != LegacyCommandType::StateChange && cmd->GetType() == LegacyCommandType::StateChange)))
            {
                commandGroups.push_back(std::move(currentGroup));
                currentGroup.clear();
            }
            
            currentGroup.push_back(cmd);
            lastType = cmd->GetType();
        }
        
        // Add the last group if it's not empty
        if (!currentGroup.empty())
        {
            commandGroups.push_back(std::move(currentGroup));
        }
        
        // Now sort each group internally
        for (auto& group : commandGroups)
        {
            // Only sort within groups of the same command type
            if (!group.empty())
            {
                if (group[0]->GetType() == LegacyCommandType::StateChange)
                {
                    // For state changes, sort by state type to minimize state transitions
                    std::stable_sort(group.begin(), group.end(),
                        [](const Ref<RenderCommandBase>& a, const Ref<RenderCommandBase>& b) {
                            return a->GetStateChangeKey() < b->GetStateChangeKey();
                        });
                }
                else
                {
                    // For draw commands, sort to minimize state changes
                    std::stable_sort(group.begin(), group.end(),
                        [](const Ref<RenderCommandBase>& a, const Ref<RenderCommandBase>& b) {
                            if (a->GetShaderKey() != b->GetShaderKey())
                                return a->GetShaderKey() < b->GetShaderKey();
                            
                            if (a->GetMaterialKey() != b->GetMaterialKey())
                                return a->GetMaterialKey() < b->GetMaterialKey();
                            
                            return a->GetTextureKey() < b->GetTextureKey();
                        });
                }
            }
        }
        
        // Rebuild the command queue from the sorted groups
        s_CommandQueue.clear();
        for (auto& group : commandGroups)
        {
            for (auto& cmd : group)
            {
                s_CommandQueue.push_back(std::move(cmd));
            }
        }
    }

    void RenderQueue::BatchCommands()
    {
        if (s_CommandQueue.empty())
            return;

        std::vector<Ref<RenderCommandBase>> batchedCommands;
        batchedCommands.reserve(s_CommandQueue.size());

        // We need to be careful not to batch across different command types
        // and respect the order of state changes
        for (sizet i = 0; i < s_CommandQueue.size();)
        {
            auto current = s_CommandQueue[i];
            sizet batchSize = 1;
            
            // Only try merging for draw commands, not state changes
            if (current->GetType() != LegacyCommandType::StateChange && s_Config.EnableMerging)
            {
                // Try to merge with subsequent commands
                while (batchSize < s_Config.MaxBatchSize && i + batchSize < s_CommandQueue.size())
                {
                    auto next = s_CommandQueue[i + batchSize];
                    
                    // Don't cross state change boundaries
                    if (next->GetType() == LegacyCommandType::StateChange)
                        break;
                        
                    if (!current->CanBatchWith(*next))
                        break;

                    if (current->MergeWith(*next))
                    {
                        s_Stats.MergedCommands++;
                        batchSize++;
                    }
                    else
                    {
                        break;
                    }
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
        u64 currentShaderKey = 0;
        u64 currentMaterialKey = 0;
        u64 currentTextureKey = 0;
        
        for (const auto& command : s_CommandQueue)
        {
            if (command->GetType() == LegacyCommandType::StateChange)
            {
                // Execute state change commands directly
                command->Execute();
                continue;
            }
            
            // For drawing commands, track state changes
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

    // State tracking implementation
    bool RenderQueue::IsRedundantStateChange(const RenderStateBase& state)
    {
        switch (state.Type)
        {
            case StateType::Blend:
                return *static_cast<const BlendState*>(&state) == s_CurrentState.Blend;
                
            case StateType::Depth:
                return *static_cast<const DepthState*>(&state) == s_CurrentState.Depth;
                
            case StateType::Stencil:
                return *static_cast<const StencilState*>(&state) == s_CurrentState.Stencil;
                
            case StateType::Culling:
                return *static_cast<const CullingState*>(&state) == s_CurrentState.Culling;
                
            case StateType::LineWidth:
                return *static_cast<const LineWidthState*>(&state) == s_CurrentState.LineWidth;
                
            case StateType::PolygonMode:
                return *static_cast<const PolygonModeState*>(&state) == s_CurrentState.PolygonMode;
                
            case StateType::Scissor:
                return *static_cast<const ScissorState*>(&state) == s_CurrentState.Scissor;
                
            case StateType::ColorMask:
                return *static_cast<const ColorMaskState*>(&state) == s_CurrentState.ColorMask;
                
            case StateType::PolygonOffset:
                return *static_cast<const PolygonOffsetState*>(&state) == s_CurrentState.PolygonOffset;
                
            case StateType::Multisampling:
                return *static_cast<const MultisamplingState*>(&state) == s_CurrentState.Multisampling;
                
            default:
                return false;
        }
    }

    void RenderQueue::UpdateCurrentState(const RenderStateBase& state)
    {
        switch (state.Type)
        {
            case StateType::Blend:
                s_CurrentState.Blend = *static_cast<const BlendState*>(&state);
                break;
                
            case StateType::Depth:
                s_CurrentState.Depth = *static_cast<const DepthState*>(&state);
                break;
                
            case StateType::Stencil:
                s_CurrentState.Stencil = *static_cast<const StencilState*>(&state);
                break;
                
            case StateType::Culling:
                s_CurrentState.Culling = *static_cast<const CullingState*>(&state);
                break;
                
            case StateType::LineWidth:
                s_CurrentState.LineWidth = *static_cast<const LineWidthState*>(&state);
                break;
                
            case StateType::PolygonMode:
                s_CurrentState.PolygonMode = *static_cast<const PolygonModeState*>(&state);
                break;
                
            case StateType::Scissor:
                s_CurrentState.Scissor = *static_cast<const ScissorState*>(&state);
                break;
                
            case StateType::ColorMask:
                s_CurrentState.ColorMask = *static_cast<const ColorMaskState*>(&state);
                break;
                
            case StateType::PolygonOffset:
                s_CurrentState.PolygonOffset = *static_cast<const PolygonOffsetState*>(&state);
                break;
                
            case StateType::Multisampling:
                s_CurrentState.Multisampling = *static_cast<const MultisamplingState*>(&state);
                break;
                
            default:
                break;
        }
    }

    void RenderQueue::InitializeDefaultState()
    {
        // Initialize with OpenGL defaults
        s_CurrentState.Blend.Enabled = false;
        s_CurrentState.Blend.SrcFactor = GL_ONE;
        s_CurrentState.Blend.DstFactor = GL_ZERO;
        s_CurrentState.Blend.Equation = GL_FUNC_ADD;
        
        s_CurrentState.Depth.TestEnabled = false;
        s_CurrentState.Depth.WriteMask = true;
        s_CurrentState.Depth.Function = GL_LESS;
        
        s_CurrentState.Stencil.Enabled = false;
        s_CurrentState.Stencil.Function = GL_ALWAYS;
        s_CurrentState.Stencil.Reference = 0;
        s_CurrentState.Stencil.ReadMask = 0xFF;
        s_CurrentState.Stencil.WriteMask = 0xFF;
        s_CurrentState.Stencil.StencilFail = GL_KEEP;
        s_CurrentState.Stencil.DepthFail = GL_KEEP;
        s_CurrentState.Stencil.DepthPass = GL_KEEP;
        
        s_CurrentState.Culling.Enabled = false;
        s_CurrentState.Culling.Face = GL_BACK;
        
        s_CurrentState.LineWidth.Width = 1.0f;
        
        s_CurrentState.PolygonMode.Face = GL_FRONT_AND_BACK;
        s_CurrentState.PolygonMode.Mode = GL_FILL;
        
        s_CurrentState.Scissor.Enabled = false;
        
        s_CurrentState.ColorMask.Red = true;
        s_CurrentState.ColorMask.Green = true;
        s_CurrentState.ColorMask.Blue = true;
        s_CurrentState.ColorMask.Alpha = true;
        
        s_CurrentState.PolygonOffset.Enabled = false;
        s_CurrentState.PolygonOffset.Factor = 0.0f;
        s_CurrentState.PolygonOffset.Units = 0.0f;
        
        s_CurrentState.Multisampling.Enabled = true;
    }

    bool StateChangeCommand::CanBatchWith(const RenderCommandBase& other) const
    {
        if (other.GetType() != LegacyCommandType::StateChange)
            return false;

        // State commands with the same state type can potentially be batched
        auto& otherState = static_cast<const StateChangeCommand&>(other);
        return m_StateType == otherState.GetStateType();
    }

    bool StateChangeCommand::MergeWith(const RenderCommandBase& other)
    {
        // We don't merge different state commands, we will simply use the latest one
        return false;
    }

    void StateChangeCommand::Execute()
    {
        if (!m_State)
            return;

        auto& rendererAPI = RenderCommand::GetRendererAPI();
        
        switch (m_StateType)
        {
            case StateType::Blend:
            {
                auto& state = static_cast<const BlendState&>(*m_State);
                rendererAPI.SetBlendState(state.Enabled);
                if (state.Enabled)
                {
                    rendererAPI.SetBlendFunc(state.SrcFactor, state.DstFactor);
                    rendererAPI.SetBlendEquation(state.Equation);
                }
                break;
            }
            
            case StateType::Depth:
            {
                auto& state = static_cast<const DepthState&>(*m_State);
                rendererAPI.SetDepthTest(state.TestEnabled);
                rendererAPI.SetDepthMask(state.WriteMask);
                rendererAPI.SetDepthFunc(state.Function);
                break;
            }
            
            case StateType::Stencil:
            {
                auto& state = static_cast<const StencilState&>(*m_State);
                if (state.Enabled)
                    rendererAPI.EnableStencilTest();
                else
                    rendererAPI.DisableStencilTest();
                
                rendererAPI.SetStencilFunc(state.Function, state.Reference, state.ReadMask);
                rendererAPI.SetStencilMask(state.WriteMask);
                rendererAPI.SetStencilOp(state.StencilFail, state.DepthFail, state.DepthPass);
                break;
            }
            
            case StateType::Culling:
            {
                auto& state = static_cast<const CullingState&>(*m_State);
                if (state.Enabled)
                {
                    rendererAPI.EnableCulling();
                    rendererAPI.SetCullFace(state.Face);
                }
                else
                {
                    rendererAPI.DisableCulling();
                }
                break;
            }
            
            case StateType::LineWidth:
            {
                auto& state = static_cast<const LineWidthState&>(*m_State);
                rendererAPI.SetLineWidth(state.Width);
                break;
            }
            
            case StateType::PolygonMode:
            {
                auto& state = static_cast<const PolygonModeState&>(*m_State);
                rendererAPI.SetPolygonMode(state.Face, state.Mode);
                break;
            }
            
            case StateType::Scissor:
            {
                auto& state = static_cast<const ScissorState&>(*m_State);
                if (state.Enabled)
                {
                    rendererAPI.EnableScissorTest();
                    rendererAPI.SetScissorBox(state.X, state.Y, state.Width, state.Height);
                }
                else
                {
                    rendererAPI.DisableScissorTest();
                }
                break;
            }
            
            case StateType::ColorMask:
            {
                auto& state = static_cast<const ColorMaskState&>(*m_State);
                rendererAPI.SetColorMask(state.Red, state.Green, state.Blue, state.Alpha);
                break;
            }
            
            case StateType::PolygonOffset:
            {
                auto& state = static_cast<const PolygonOffsetState&>(*m_State);
                if (state.Enabled)
                    rendererAPI.SetPolygonOffset(state.Factor, state.Units);
                else
                    rendererAPI.SetPolygonOffset(0.0f, 0.0f);
                break;
            }
            
            case StateType::Multisampling:
            {
                auto& state = static_cast<const MultisamplingState&>(*m_State);
                if (state.Enabled)
                    rendererAPI.EnableMultisampling();
                else
                    rendererAPI.DisableMultisampling();
                break;
            }
            
            default:
                break;
        }
    }

    void LegacyDrawMeshCommand::Execute()
    {
        if (m_Transforms.size() == 1)
        {
            // Single mesh rendering
            Renderer3D::RenderMeshInternal(m_Mesh, m_Transforms[0], m_Material);
        }
        else if (m_Transforms.size() > 1)
        {
            // Instanced rendering for multiple transforms
            Renderer3D::RenderMeshInstanced(m_Mesh, m_Transforms, m_Material);
        }
    }

    u64 LegacyDrawMeshCommand::GetShaderKey() const
    {
        return 0;
    }

    u64 LegacyDrawMeshCommand::GetMaterialKey() const
    {
        u64 key = 0;
        
        key ^= std::hash<f32>{}(m_Material.Ambient.x);
        key ^= std::hash<f32>{}(m_Material.Ambient.y);
        key ^= std::hash<f32>{}(m_Material.Ambient.z);
        
        key ^= std::hash<f32>{}(m_Material.Diffuse.x);
        key ^= std::hash<f32>{}(m_Material.Diffuse.y);
        key ^= std::hash<f32>{}(m_Material.Diffuse.z);
        
        key ^= std::hash<f32>{}(m_Material.Specular.x);
        key ^= std::hash<f32>{}(m_Material.Specular.y);
        key ^= std::hash<f32>{}(m_Material.Specular.z);
        key ^= std::hash<f32>{}(m_Material.Shininess);
        
        key ^= std::hash<bool>{}(m_Material.UseTextureMaps);
        
        return key;
    }

    u64 LegacyDrawMeshCommand::GetTextureKey() const
    {
        u64 key = 0;
        
        if (m_Material.UseTextureMaps)
        {
            if (m_Material.DiffuseMap)
                key ^= std::hash<u32>{}(m_Material.DiffuseMap->GetRendererID());
            if (m_Material.SpecularMap)
                key ^= std::hash<u32>{}(m_Material.SpecularMap->GetRendererID());
        }
        
        return key;
    }

    bool LegacyDrawMeshCommand::CanBatchWith(const RenderCommandBase& other) const
    {
        if (other.GetType() != LegacyCommandType::Mesh)
            return false;

        const auto& otherMesh = static_cast<const LegacyDrawMeshCommand&>(other);
        return m_Mesh == otherMesh.m_Mesh && 
               m_Material == otherMesh.m_Material;
    }

    bool LegacyDrawMeshCommand::MergeWith(const RenderCommandBase& other)
    {
        if (!CanBatchWith(other))
            return false;

        const auto& otherMesh = static_cast<const LegacyDrawMeshCommand&>(other);
        
        // Add the transform from the other command to our transforms list
        for (const auto& transform : otherMesh.m_Transforms)
        {
            m_Transforms.push_back(transform);
        }
        
        m_BatchSize = m_Transforms.size();
        return true;
    }

    void LegacyDrawQuadCommand::Execute()
    {
        Renderer3D::RenderQuadInternal(m_Transform, m_Texture);
    }

    u64 LegacyDrawQuadCommand::GetShaderKey() const
    {
        return 1;
    }

    u64 LegacyDrawQuadCommand::GetMaterialKey() const
    {
        return 0;
    }

    u64 LegacyDrawQuadCommand::GetTextureKey() const
    {
        return m_Texture ? std::hash<u32>{}(m_Texture->GetRendererID()) : 0;
    }

    bool LegacyDrawQuadCommand::CanBatchWith(const RenderCommandBase& other) const
    {
        if (other.GetType() != LegacyCommandType::Quad)
            return false;

        const auto& otherQuad = static_cast<const LegacyDrawQuadCommand&>(other);
        return m_Texture == otherQuad.m_Texture;
    }

    bool LegacyDrawQuadCommand::MergeWith(const RenderCommandBase& other)
    {
        if (!CanBatchWith(other))
            return false;

        const auto& otherQuad = static_cast<const LegacyDrawQuadCommand&>(other);
        m_BatchSize += otherQuad.m_BatchSize;
        return true;
    }
}
