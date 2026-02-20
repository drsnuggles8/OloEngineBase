#include "OloEnginePCH.h"
#include "ParticleBatchRenderer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glm/gtc/matrix_transform.hpp>

namespace OloEngine
{
    struct ParticleBatchData
    {
        static constexpr u32 MaxInstances = 10000;

        Ref<VertexArray> VAO;
        Ref<VertexBuffer> QuadVBO;      // Unit quad vertices (per-vertex)
        Ref<VertexBuffer> InstanceVBO;   // Per-instance data
        Ref<Shader> ParticleShader;

        ParticleInstance* InstanceBase = nullptr;
        ParticleInstance* InstancePtr = nullptr;
        u32 InstanceCount = 0;

        Ref<Texture2D> CurrentTexture;
        Ref<Texture2D> WhiteTexture;

        Ref<UniformBuffer> CameraUBO;

        struct CameraData
        {
            glm::mat4 ViewProjection;
        };
        CameraData CameraBuffer;

        glm::vec3 CameraRight{};
        glm::vec3 CameraUp{};

        ParticleBatchRenderer::Statistics Stats;
    };

    static ParticleBatchData s_Data;

    void ParticleBatchRenderer::Init()
    {
        OLO_PROFILE_FUNCTION();

        s_Data.VAO = VertexArray::Create();

        // Unit quad: 4 vertices with 2D positions (centered at origin, size 1x1)
        f32 quadVertices[] = {
            -0.5f, -0.5f,  // bottom-left
             0.5f, -0.5f,  // bottom-right
             0.5f,  0.5f,  // top-right
            -0.5f,  0.5f   // top-left
        };

        s_Data.QuadVBO = VertexBuffer::Create(quadVertices, sizeof(quadVertices));
        s_Data.QuadVBO->SetLayout({
            { ShaderDataType::Float2, "a_QuadPos" }
        });
        s_Data.VAO->AddVertexBuffer(s_Data.QuadVBO);

        // Index buffer for the unit quad (two triangles)
        u32 indices[] = { 0, 1, 2, 2, 3, 0 };
        auto indexBuffer = IndexBuffer::Create(indices, 6);
        s_Data.VAO->SetIndexBuffer(indexBuffer);

        // Instance buffer (dynamic, per-instance data)
        s_Data.InstanceVBO = VertexBuffer::Create(ParticleBatchData::MaxInstances * sizeof(ParticleInstance));
        s_Data.InstanceVBO->SetLayout({
            { ShaderDataType::Float4, "a_PositionSize" },
            { ShaderDataType::Float4, "a_Color" },
            { ShaderDataType::Float4, "a_UVRect" },
            { ShaderDataType::Float4, "a_VelocityRotation" },
            { ShaderDataType::Float,  "a_StretchFactor" },
            { ShaderDataType::Int,    "a_EntityID" }
        });
        s_Data.VAO->AddInstanceBuffer(s_Data.InstanceVBO);

        // CPU-side staging buffer
        s_Data.InstanceBase = new ParticleInstance[ParticleBatchData::MaxInstances];

        // White texture for untextured particles
        s_Data.WhiteTexture = Texture2D::Create(TextureSpecification());
        u32 whiteTextureData = 0xffffffffU;
        s_Data.WhiteTexture->SetData(&whiteTextureData, sizeof(u32));

        // Load particle shader
        s_Data.ParticleShader = Shader::Create("assets/shaders/Particle_Billboard.glsl");

        // Camera UBO (binding 0, shared with other renderers)
        s_Data.CameraUBO = UniformBuffer::Create(sizeof(ParticleBatchData::CameraData), 0);
    }

    void ParticleBatchRenderer::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        delete[] s_Data.InstanceBase;
        s_Data.InstanceBase = nullptr;
        s_Data.InstancePtr = nullptr;
    }

    void ParticleBatchRenderer::BeginBatch(const EditorCamera& camera)
    {
        OLO_PROFILE_FUNCTION();

        s_Data.CameraBuffer.ViewProjection = camera.GetViewProjection();
        s_Data.CameraUBO->SetData(&s_Data.CameraBuffer, sizeof(ParticleBatchData::CameraData));

        s_Data.CameraRight = camera.GetRightDirection();
        s_Data.CameraUp = camera.GetUpDirection();

        s_Data.Stats = {};
        StartNewBatch();
    }

    void ParticleBatchRenderer::BeginBatch(const Camera& camera, const glm::mat4& cameraTransform)
    {
        OLO_PROFILE_FUNCTION();

        s_Data.CameraBuffer.ViewProjection = camera.GetProjection() * glm::inverse(cameraTransform);
        s_Data.CameraUBO->SetData(&s_Data.CameraBuffer, sizeof(ParticleBatchData::CameraData));

        // Extract camera right and up from the transform matrix columns
        s_Data.CameraRight = glm::normalize(glm::vec3(cameraTransform[0]));
        s_Data.CameraUp = glm::normalize(glm::vec3(cameraTransform[1]));

        s_Data.Stats = {};
        StartNewBatch();
    }

    void ParticleBatchRenderer::Submit(const glm::vec3& position, f32 size, f32 rotation,
                                        const glm::vec4& color, const glm::vec4& uvRect,
                                        int entityID)
    {
        if (s_Data.InstanceCount >= ParticleBatchData::MaxInstances)
        {
            Flush();
            StartNewBatch();
        }

        auto& inst = *s_Data.InstancePtr;
        inst.PositionSize = { position.x, position.y, position.z, size };
        inst.Color = color;
        inst.UVRect = uvRect;
        inst.VelocityRotation = { 0.0f, 0.0f, 0.0f, rotation };
        inst.StretchFactor = 0.0f;
        inst.EntityID = entityID;

        ++s_Data.InstancePtr;
        ++s_Data.InstanceCount;
    }

    void ParticleBatchRenderer::SubmitStretched(const glm::vec3& position, f32 size,
                                                 const glm::vec3& velocity, f32 stretchFactor,
                                                 const glm::vec4& color, const glm::vec4& uvRect,
                                                 int entityID)
    {
        if (s_Data.InstanceCount >= ParticleBatchData::MaxInstances)
        {
            Flush();
            StartNewBatch();
        }

        auto& inst = *s_Data.InstancePtr;
        inst.PositionSize = { position.x, position.y, position.z, size };
        inst.Color = color;
        inst.UVRect = uvRect;
        inst.VelocityRotation = { velocity.x, velocity.y, velocity.z, 0.0f };
        inst.StretchFactor = stretchFactor;
        inst.EntityID = entityID;

        ++s_Data.InstancePtr;
        ++s_Data.InstanceCount;
    }

    void ParticleBatchRenderer::SetTexture(const Ref<Texture2D>& texture)
    {
        const auto& newTex = texture ? texture : s_Data.WhiteTexture;

        if (s_Data.CurrentTexture && s_Data.CurrentTexture != newTex && s_Data.InstanceCount > 0)
        {
            Flush();
            StartNewBatch();
        }

        s_Data.CurrentTexture = newTex;
    }

    void ParticleBatchRenderer::EndBatch()
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.InstanceCount > 0)
        {
            Flush();
        }

        s_Data.CurrentTexture = nullptr;
    }

    void ParticleBatchRenderer::Flush()
    {
        if (s_Data.InstanceCount == 0)
        {
            return;
        }

        // Upload instance data to GPU
        u32 dataSize = s_Data.InstanceCount * sizeof(ParticleInstance);
        s_Data.InstanceVBO->SetData({ s_Data.InstanceBase, dataSize });

        // Bind shader and set uniforms
        s_Data.ParticleShader->Bind();
        s_Data.ParticleShader->SetFloat3("u_CameraRight", s_Data.CameraRight);
        s_Data.ParticleShader->SetFloat3("u_CameraUp", s_Data.CameraUp);

        // Bind texture
        bool hasTexture = (s_Data.CurrentTexture && s_Data.CurrentTexture != s_Data.WhiteTexture);
        s_Data.ParticleShader->SetInt("u_HasTexture", hasTexture ? 1 : 0);
        if (hasTexture)
        {
            RenderCommand::BindTexture(0, s_Data.CurrentTexture->GetRendererID());
        }

        // Instanced draw call
        RenderCommand::DrawIndexedInstanced(s_Data.VAO, 6, s_Data.InstanceCount);

        s_Data.Stats.DrawCalls++;
        s_Data.Stats.InstanceCount += s_Data.InstanceCount;
    }

    void ParticleBatchRenderer::StartNewBatch()
    {
        s_Data.InstanceCount = 0;
        s_Data.InstancePtr = s_Data.InstanceBase;
    }

    void ParticleBatchRenderer::ResetStats()
    {
        s_Data.Stats = {};
    }

    ParticleBatchRenderer::Statistics ParticleBatchRenderer::GetStats()
    {
        return s_Data.Stats;
    }
}
