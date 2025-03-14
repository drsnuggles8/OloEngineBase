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
        virtual CommandType GetType() const = 0;
        
        // Sorting keys
        virtual uint64_t GetShaderKey() const = 0;
        virtual uint64_t GetMaterialKey() const = 0;
        virtual uint64_t GetTextureKey() const = 0;

        // Command pool management
        virtual void Reset() = 0;
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
        }

        void Execute() override;
        CommandType GetType() const override { return CommandType::Mesh; }
        
        // Sorting keys
        uint64_t GetShaderKey() const override;
        uint64_t GetMaterialKey() const override;
        uint64_t GetTextureKey() const override;

        void Reset() override
        {
            m_Mesh.reset();
            m_Transform = glm::mat4(1.0f);
            m_Material = Material();
        }

    private:
        Ref<Mesh> m_Mesh;
        glm::mat4 m_Transform;
        Material m_Material;
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
        }

        void Execute() override;
        CommandType GetType() const override { return CommandType::Quad; }
        
        // Sorting keys
        uint64_t GetShaderKey() const override;
        uint64_t GetMaterialKey() const override;
        uint64_t GetTextureKey() const override;

        void Reset() override
        {
            m_Transform = glm::mat4(1.0f);
            m_Texture.reset();
        }

    private:
        glm::mat4 m_Transform;
        Ref<Texture2D> m_Texture;
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
        };

        static void Init();
        static void Shutdown();

        static void BeginScene(const glm::mat4& viewProjectionMatrix);
        static void EndScene();

        static void SubmitMesh(const Ref<Mesh>& mesh, const glm::mat4& transform, const Material& material);
        static void SubmitQuad(const glm::mat4& transform, const Ref<Texture2D>& texture);

        static void Flush();
        static void ResetStats();
        static Statistics GetStats();

    private:
        static void SortCommands();
        static void ExecuteCommands();
        static void ReturnCommandToPool(std::unique_ptr<RenderCommandBase>&& command);
        static std::unique_ptr<RenderCommandBase> GetCommandFromPool(CommandType type);

        static Scope<SceneData> s_SceneData;
        static std::vector<std::unique_ptr<RenderCommandBase>> s_CommandQueue;
        static std::queue<std::unique_ptr<DrawMeshCommand>> s_MeshCommandPool;
        static std::queue<std::unique_ptr<DrawQuadCommand>> s_QuadCommandPool;
        static Statistics s_Stats;
    };
} 