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

namespace OloEngine
{
    // Forward declarations
    class RenderCommand;

    // Base class for all render commands
    class RenderCommandBase
    {
    public:
        virtual ~RenderCommandBase() = default;
        virtual void Execute() = 0;
    };

    // Command for drawing a mesh with material
    class DrawMeshCommand : public RenderCommandBase
    {
    public:
        DrawMeshCommand(const Ref<Mesh>& mesh, const glm::mat4& transform, const Material& material)
            : m_Mesh(mesh), m_Transform(transform), m_Material(material) {}

        void Execute() override;

    private:
        Ref<Mesh> m_Mesh;
        glm::mat4 m_Transform;
        Material m_Material;
    };

    // Command for drawing a textured quad
    class DrawQuadCommand : public RenderCommandBase
    {
    public:
        DrawQuadCommand(const glm::mat4& transform, const Ref<Texture2D>& texture)
            : m_Transform(transform), m_Texture(texture) {}

        void Execute() override;

    private:
        glm::mat4 m_Transform;
        Ref<Texture2D> m_Texture;
    };

    // Main render queue class
    class RenderQueue
    {
    public:
        static void Init();
        static void Shutdown();

        // Command submission
        static void SubmitMesh(const Ref<Mesh>& mesh, const glm::mat4& transform, const Material& material);
        static void SubmitQuad(const glm::mat4& transform, const Ref<Texture2D>& texture);

        // Scene management
        static void BeginScene(const glm::mat4& viewProjectionMatrix);
        static void EndScene();

        // Execution
        static void Flush();

        // Statistics
        struct Statistics
        {
            u32 DrawCalls = 0;
            u32 CommandCount = 0;
        };
        static void ResetStats();
        static Statistics GetStats();

    private:
        static void SortCommands();
        static void ExecuteCommands();

        struct SceneData
        {
            glm::mat4 ViewProjectionMatrix;
        };

        static Scope<SceneData> s_SceneData;
        static std::vector<std::unique_ptr<RenderCommandBase>> s_CommandQueue;
        static Statistics s_Stats;
    };
} 