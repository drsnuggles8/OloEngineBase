#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/RenderState.h"

#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <unordered_map>
#include <queue>

namespace OloEngine
{
    // Forward declarations
    class RenderCommand;

    // Command type for sorting
    enum class CommandType
    {
        Mesh,        // 3D mesh with material
        Quad,        // 2D quad with texture
        LightCube,   // Light visualization cube
        StateChange  // OpenGL state change
    };

    // Base class for all render commands
    class RenderCommandBase
    {
    public:
        virtual ~RenderCommandBase() = default;
        virtual void Execute() = 0;
        [[nodiscard]] virtual CommandType GetType() const = 0;
        
        // Sorting keys
        [[nodiscard]] virtual u64 GetShaderKey() const = 0;
        [[nodiscard]] virtual u64 GetMaterialKey() const = 0;
        [[nodiscard]] virtual u64 GetTextureKey() const = 0;
        [[nodiscard]] virtual u64 GetStateChangeKey() const { return 0; }

        // Command pool management
        virtual void Reset() = 0;

        // Command batching and merging
        [[nodiscard]] virtual bool CanBatchWith(const RenderCommandBase& other) const = 0;
        virtual bool MergeWith(const RenderCommandBase& other) = 0;
        [[nodiscard]] virtual sizet GetBatchSize() const = 0;
    };

    // Command for changing OpenGL state
    class StateChangeCommand : public RenderCommandBase
    {
    public:
        StateChangeCommand() = default;
        
        template<typename T>
        void Set(const T& state)
        {
            static_assert(std::is_base_of<RenderStateBase, T>::value, "State must derive from RenderStateBase");
            m_StateType = state.Type;
            m_State = CreateRef<T>(state);
        }

        void Execute() override;
        [[nodiscard]] CommandType GetType() const override { return CommandType::StateChange; }
        
        // Sorting keys - state changes are sorted by type
        [[nodiscard]] u64 GetShaderKey() const override { return 0; }
        [[nodiscard]] u64 GetMaterialKey() const override { return 0; }
        [[nodiscard]] u64 GetTextureKey() const override { return 0; }
        [[nodiscard]] u64 GetStateChangeKey() const override { return static_cast<u64>(m_StateType); }

        void Reset() override
        {
            m_State.reset();
            m_StateType = StateType::None;
        }

        // Command batching and merging
        [[nodiscard]] bool CanBatchWith(const RenderCommandBase& other) const override;
        bool MergeWith(const RenderCommandBase& other) override;
        [[nodiscard]] sizet GetBatchSize() const override { return 1; }
        [[nodiscard]] StateType GetStateType() const { return m_StateType; }
        [[nodiscard]] const Ref<RenderStateBase>& GetState() const { return m_State; }

    private:
        Ref<RenderStateBase> m_State;
        StateType m_StateType = StateType::None;
    };

    // Command for drawing a mesh with material
    class DrawMeshCommand : public RenderCommandBase
    {
    public:
        DrawMeshCommand() = default;
        void Set(const Ref<Mesh>& mesh, const glm::mat4& transform, const Material& material, bool isStatic = false)
        {
            m_Mesh = mesh;
            m_Transforms.clear();
            m_Transforms.push_back(transform);
            m_Material = material;
            m_BatchSize = 1;
            m_IsStatic = isStatic;
        }

        void AddInstance(const glm::mat4& transform)
        {
            m_Transforms.push_back(transform);
            m_BatchSize = m_Transforms.size();
        }

        void Execute() override;
        [[nodiscard]] CommandType GetType() const override { return CommandType::Mesh; }
        
        // Sorting keys
        [[nodiscard]] u64 GetShaderKey() const override;
        [[nodiscard]] u64 GetMaterialKey() const override;
        [[nodiscard]] u64 GetTextureKey() const override;

        void Reset() override
        {
            m_Mesh.reset();
            m_Transforms.clear();
            m_Material = Material();
            m_BatchSize = 1;
            m_IsStatic = false;
        }

        // Command batching and merging
        [[nodiscard]] bool CanBatchWith(const RenderCommandBase& other) const override;
        bool MergeWith(const RenderCommandBase& other) override;
        [[nodiscard]] sizet GetBatchSize() const override { return m_BatchSize; }
        [[nodiscard]] bool IsStatic() const { return m_IsStatic; }

    private:
        Ref<Mesh> m_Mesh;
        std::vector<glm::mat4> m_Transforms;
        Material m_Material;
        sizet m_BatchSize;
        bool m_IsStatic = false;
    };

    // Command for drawing a textured quad
    class DrawQuadCommand : public RenderCommandBase
    {
    public:
        DrawQuadCommand() = default;
        void Set(const glm::mat4& transform, const Ref<Texture2D>& texture)
        {
            m_Transform = transform;
            m_Texture = texture;
            m_BatchSize = 1;
        }

        void Execute() override;
        [[nodiscard]] CommandType GetType() const override { return CommandType::Quad; }
        
        // Sorting keys
        [[nodiscard]] u64 GetShaderKey() const override;
        [[nodiscard]] u64 GetMaterialKey() const override;
        [[nodiscard]] u64 GetTextureKey() const override;

        void Reset() override
        {
            m_Transform = glm::mat4(1.0f);
            m_Texture.reset();
            m_BatchSize = 1;
        }

        // Command batching and merging
        [[nodiscard]] bool CanBatchWith(const RenderCommandBase& other) const override;
        bool MergeWith(const RenderCommandBase& other) override;
        [[nodiscard]] sizet GetBatchSize() const override { return m_BatchSize; }

    private:
        glm::mat4 m_Transform;
        Ref<Texture2D> m_Texture;
        sizet m_BatchSize;
    };

    // Main render queue class
    class RenderQueue
    {
    public:
        struct SceneData
        {
            glm::mat4 ViewProjectionMatrix;
			glm::mat4 ViewMatrix;
			glm::mat4 ProjectionMatrix;
        };

        struct Statistics
        {
            u32 CommandCount = 0;
            u32 DrawCalls = 0;
            u32 PoolHits = 0;
            u32 PoolMisses = 0;
            u32 StateChanges = 0;
            u32 BatchedCommands = 0;
            u32 MergedCommands = 0;
            u32 StateCommandCount = 0;
            u32 RedundantStateChanges = 0;
        };

        struct Config
        {
            sizet InitialPoolSize = 100;
            sizet MaxPoolSize = 1000;
            sizet CommandQueueReserve = 1000;
            bool EnableSorting = false;
            bool EnableBatching = false;
            bool EnableMerging = false;
            sizet MaxBatchSize = 100;
            bool EnableStateTracking = true;
        };

        static void Init(const Config& config = Config{});
        static void Shutdown();

        static void BeginScene(const glm::mat4& viewProjectionMatrix);
        static void EndScene();

        // Drawing submission methods
        static void SubmitMesh(const Ref<Mesh>& mesh, const glm::mat4& transform, const Material& material, bool isStatic = false);
        static void SubmitQuad(const glm::mat4& transform, const Ref<Texture2D>& texture);

        // State change submission methods
        template<typename T>
        static void SubmitStateChange(const T& state)
        {
            static_assert(std::is_base_of<RenderStateBase, T>::value, "State must derive from RenderStateBase");
            
            // If state tracking is enabled, check if this change is redundant
            if (s_Config.EnableStateTracking)
            {
                bool redundant = IsRedundantStateChange(state);
                if (redundant)
                {
                    s_Stats.RedundantStateChanges++;
                    return;
                }
                
                // Update our cached state
                UpdateCurrentState(state);
            }
            
            auto command = GetCommandFromPool(CommandType::StateChange);
            if (auto stateCommand = std::static_pointer_cast<StateChangeCommand>(command))
            {
                stateCommand->Set(state);
                s_CommandQueue.push_back(std::move(command));
                s_Stats.CommandCount++;
                s_Stats.StateCommandCount++;
            }
        }

        static void Flush();
        static void ResetStats();
        [[nodiscard]] static Statistics GetStats();
        
        // Access to current state (useful for tracking)
        [[nodiscard]] static const RenderState& GetCurrentState() { return s_CurrentState; }

    private:
        static void SortCommands();
        static void ExecuteCommands();
        static void ReturnCommandToPool(Ref<RenderCommandBase>&& command);
        static Ref<RenderCommandBase> GetCommandFromPool(CommandType type);
        static void GrowCommandPool(CommandType type);
        static void BatchCommands();
        
        // State tracking
        static bool IsRedundantStateChange(const RenderStateBase& state);
        static void UpdateCurrentState(const RenderStateBase& state);
        static void InitializeDefaultState();

        static Scope<SceneData> s_SceneData;
        static std::vector<Ref<RenderCommandBase>> s_CommandQueue;
        static std::queue<Ref<DrawMeshCommand>> s_MeshCommandPool;
        static std::queue<Ref<DrawQuadCommand>> s_QuadCommandPool;
        static std::queue<Ref<StateChangeCommand>> s_StateCommandPool;
        static Statistics s_Stats;
        static Config s_Config;
        static RenderState s_CurrentState;
    };
}
