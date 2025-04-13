#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/RenderState.h"
#include "OloEngine/Renderer/RenderCommands/RenderCommandBase.h"

#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <unordered_map>
#include <queue>

namespace OloEngine
{
    // Forward declarations
    class RenderCommand;

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
            
            auto command = GetCommandFromPool(LegacyCommandType::StateChange);
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
        static Ref<RenderCommandBase> GetCommandFromPool(LegacyCommandType type);
        static void GrowCommandPool(LegacyCommandType type);
        static void BatchCommands();
        
        // State tracking
        static bool IsRedundantStateChange(const RenderStateBase& state);
        static void UpdateCurrentState(const RenderStateBase& state);
        static void InitializeDefaultState();

        static Scope<SceneData> s_SceneData;
        static std::vector<Ref<RenderCommandBase>> s_CommandQueue;
        static std::queue<Ref<LegacyDrawMeshCommand>> s_MeshCommandPool;
        static std::queue<Ref<LegacyDrawQuadCommand>> s_QuadCommandPool;
        static std::queue<Ref<StateChangeCommand>> s_StateCommandPool;
        static Statistics s_Stats;
        static Config s_Config;
        static RenderState s_CurrentState;
    };
}
