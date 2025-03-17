#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Texture.h"

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
        Mesh,    // 3D mesh with material
        Quad,    // 2D quad with texture
        LightCube // Light visualization cube
    };

    // Base class for all render commands
    class RenderCommandBase
    {
    public:
        virtual ~RenderCommandBase() = default;
        virtual void Execute() = 0;
        [[nodiscard]] virtual CommandType GetType() const = 0;
        
        // Sorting keys
        [[nodiscard]] virtual uint64_t GetShaderKey() const = 0;
        [[nodiscard]] virtual uint64_t GetMaterialKey() const = 0;
        [[nodiscard]] virtual uint64_t GetTextureKey() const = 0;

        // Command pool management
        virtual void Reset() = 0;

        // Command batching and merging
        [[nodiscard]] virtual bool CanBatchWith(const RenderCommandBase& other) const = 0;
        virtual bool MergeWith(const RenderCommandBase& other) = 0;
        [[nodiscard]] virtual sizet GetBatchSize() const = 0;
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
        [[nodiscard]] uint64_t GetShaderKey() const override;
        [[nodiscard]] uint64_t GetMaterialKey() const override;
        [[nodiscard]] uint64_t GetTextureKey() const override;

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
        [[nodiscard]] uint64_t GetShaderKey() const override;
        [[nodiscard]] uint64_t GetMaterialKey() const override;
        [[nodiscard]] uint64_t GetTextureKey() const override;

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
        };

        struct Config
        {
            sizet InitialPoolSize = 100;
            sizet MaxPoolSize = 1000;
            sizet CommandQueueReserve = 1000;
            bool EnableSorting = true;
            bool EnableBatching = true;
            bool EnableMerging = true;
            sizet MaxBatchSize = 100;
        };

        static void Init(const Config& config = Config{});
        static void Shutdown();

        static void BeginScene(const glm::mat4& viewProjectionMatrix);
		static void BeginScene(const glm::mat4& viewMatrix, const glm::mat4& projectionMatrix, const glm::mat4& viewProjectionMatrix);
        static void EndScene();

        static void SubmitMesh(const Ref<Mesh>& mesh, const glm::mat4& transform, const Material& material, bool isStatic = false);
        static void SubmitQuad(const glm::mat4& transform, const Ref<Texture2D>& texture);

        static void Flush();
        static void ResetStats();
        [[nodiscard]] static Statistics GetStats();

    private:
        static void SortCommands();
        static void ExecuteCommands();
        static void ReturnCommandToPool(Ref<RenderCommandBase>&& command);
        static Ref<RenderCommandBase> GetCommandFromPool(CommandType type);
        static void GrowCommandPool(CommandType type);
        static void BatchCommands();
        static bool TryMergeCommands(Ref<RenderCommandBase>& current, Ref<RenderCommandBase>& next);

        static Scope<SceneData> s_SceneData;
        static std::vector<Ref<RenderCommandBase>> s_CommandQueue;
        static std::queue<Ref<DrawMeshCommand>> s_MeshCommandPool;
        static std::queue<Ref<DrawQuadCommand>> s_QuadCommandPool;
        static Statistics s_Stats;
        static Config s_Config;
    };
} 
