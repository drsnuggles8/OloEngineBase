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
        [[nodiscard]] virtual size_t GetBatchSize() const = 0;
    };

    // Command for drawing a mesh with material
    class DrawMeshCommand : public RenderCommandBase
    {
    public:
        DrawMeshCommand() = default;
        void Set(const Ref<Mesh>& mesh, const glm::mat4& transform, const Material& material)
        {
            m_Mesh = mesh;
            m_Transform = transform;
            m_Material = material;
            m_BatchSize = 1;
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
            m_Transform = glm::mat4(1.0f);
            m_Material = Material();
            m_BatchSize = 1;
        }

        // Command batching and merging
        [[nodiscard]] bool CanBatchWith(const RenderCommandBase& other) const override;
        bool MergeWith(const RenderCommandBase& other) override;
        [[nodiscard]] size_t GetBatchSize() const override { return m_BatchSize; }

    private:
        Ref<Mesh> m_Mesh;
        glm::mat4 m_Transform;
        Material m_Material;
        size_t m_BatchSize;
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
        [[nodiscard]] size_t GetBatchSize() const override { return m_BatchSize; }

    private:
        glm::mat4 m_Transform;
        Ref<Texture2D> m_Texture;
        size_t m_BatchSize;
    };

    // Main render queue class
    class RenderQueue
    {
    public:
        struct SceneData
        {
            glm::mat4 ViewProjectionMatrix;
        };

        struct Statistics
        {
            uint32_t CommandCount = 0;
            uint32_t DrawCalls = 0;
            uint32_t PoolHits = 0;
            uint32_t PoolMisses = 0;
            uint32_t StateChanges = 0;
            uint32_t BatchedCommands = 0;
            uint32_t MergedCommands = 0;
        };

        struct Config
        {
            size_t InitialPoolSize = 100;
            size_t MaxPoolSize = 1000;
            size_t CommandQueueReserve = 1000;
            bool EnableSorting = true;
            bool EnableBatching = true;
            bool EnableMerging = true;
            size_t MaxBatchSize = 100;
        };

        static void Init(const Config& config = Config{});
        static void Shutdown();

        static void BeginScene(const glm::mat4& viewProjectionMatrix);
        static void EndScene();

        static void SubmitMesh(const Ref<Mesh>& mesh, const glm::mat4& transform, const Material& material);
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