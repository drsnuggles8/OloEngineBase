#include "OloEnginePCH.h"
#include "ParticleBatchRenderer.h"
#include "OloEngine/Particle/GPUParticleSystem.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glm/gtc/matrix_transform.hpp>
#include <memory>

namespace OloEngine
{
    struct ParticleBatchData
    {
        static constexpr u32 MaxInstances = 10000;

        Ref<VertexArray> VAO;
        Ref<VertexBuffer> QuadVBO;     // Unit quad vertices (per-vertex)
        Ref<VertexBuffer> InstanceVBO; // Per-instance data
        Ref<Shader> ParticleShader;

        std::unique_ptr<ParticleInstance[]> InstanceBase;
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

        // Particle params UBO (binding 2, std140 layout)
        // Must match the ParticleParams uniform block in the shaders
        struct ParticleParamsData
        {
            glm::vec3 CameraRight{};         // offset 0  (align 16)
            f32 _pad0{};                     // offset 12
            glm::vec3 CameraUp{};            // offset 16 (align 16)
            i32 HasTexture = 0;              // offset 28
            i32 SoftParticlesEnabled = 0;    // offset 32
            f32 SoftParticleDistance = 1.0f; // offset 36
            f32 NearClip = 0.1f;             // offset 40
            f32 FarClip = 1000.0f;           // offset 44
            glm::vec2 ViewportSize{};        // offset 48 (align 8)
            f32 _pad1[2]{};                  // offset 56 (pad to 64)
        };
        static_assert(sizeof(ParticleParamsData) == 64, "ParticleParamsData must be 64 bytes for std140");

        Ref<UniformBuffer> ParticleParamsUBO;
        ParticleParamsData ParticleParamsBuffer;

        // Soft particle state
        SoftParticleParams SoftParams;

        // Mesh particle resources
        Ref<Shader> MeshParticleShader;
        Ref<UniformBuffer> MeshInstanceUBO;

        // Trail rendering resources
        static constexpr u32 MaxTrailQuads = 10000;
        static constexpr u32 MaxTrailVertices = MaxTrailQuads * 4;
        static constexpr u32 MaxTrailIndices = MaxTrailQuads * 6;

        Ref<VertexArray> TrailVAO;
        Ref<VertexBuffer> TrailVBO;
        Ref<Shader> TrailShader;

        std::unique_ptr<TrailVertex[]> TrailVertexBase;
        TrailVertex* TrailVertexPtr = nullptr;
        u32 TrailQuadCount = 0;

        Ref<Texture2D> CurrentTrailTexture;

        // GPU billboard resources
        Ref<Shader> GPUBillboardShader;

        ParticleBatchRenderer::Statistics Stats;
    };

    static ParticleBatchData s_Data;

    void ParticleBatchRenderer::Init()
    {
        OLO_PROFILE_FUNCTION();

        s_Data.VAO = VertexArray::Create();

        // Unit quad: 4 vertices with 2D positions (centered at origin, size 1x1)
        f32 quadVertices[] = {
            -0.5f, -0.5f, // bottom-left
            0.5f, -0.5f,  // bottom-right
            0.5f, 0.5f,   // top-right
            -0.5f, 0.5f   // top-left
        };

        s_Data.QuadVBO = VertexBuffer::Create(quadVertices, sizeof(quadVertices));
        s_Data.QuadVBO->SetLayout({ { ShaderDataType::Float2, "a_QuadPos" } });
        s_Data.VAO->AddVertexBuffer(s_Data.QuadVBO);

        // Index buffer for the unit quad (two triangles)
        u32 indices[] = { 0, 1, 2, 2, 3, 0 };
        auto indexBuffer = IndexBuffer::Create(indices, 6);
        s_Data.VAO->SetIndexBuffer(indexBuffer);

        // Instance buffer (dynamic, per-instance data)
        s_Data.InstanceVBO = VertexBuffer::Create(ParticleBatchData::MaxInstances * sizeof(ParticleInstance));
        s_Data.InstanceVBO->SetLayout({ { ShaderDataType::Float4, "a_PositionSize" },
                                        { ShaderDataType::Float4, "a_Color" },
                                        { ShaderDataType::Float4, "a_UVRect" },
                                        { ShaderDataType::Float4, "a_VelocityRotation" },
                                        { ShaderDataType::Float, "a_StretchFactor" },
                                        { ShaderDataType::Int, "a_EntityID" } });
        s_Data.VAO->AddInstanceBuffer(s_Data.InstanceVBO);

        // CPU-side staging buffer
        s_Data.InstanceBase = std::make_unique<ParticleInstance[]>(ParticleBatchData::MaxInstances);

        // White texture for untextured particles
        s_Data.WhiteTexture = Texture2D::Create(TextureSpecification());
        u32 whiteTextureData = 0xffffffffU;
        s_Data.WhiteTexture->SetData(&whiteTextureData, sizeof(u32));

        // Load particle shader
        s_Data.ParticleShader = Shader::Create("assets/shaders/Particle_Billboard.glsl");

        // Camera UBO (binding 0, shared with other renderers)
        s_Data.CameraUBO = UniformBuffer::Create(sizeof(ParticleBatchData::CameraData), 0);

        // Particle params UBO (binding 2)
        s_Data.ParticleParamsUBO = UniformBuffer::Create(sizeof(ParticleBatchData::ParticleParamsData), 2);

        // Mesh particle resources
        s_Data.MeshParticleShader = Shader::Create("assets/shaders/Particle_Mesh.glsl");

        // UBO for single mesh particle instance data (binding 3)
        s_Data.MeshInstanceUBO = UniformBuffer::Create(sizeof(MeshParticleInstance), 3);

        // Trail rendering resources
        s_Data.TrailVAO = VertexArray::Create();

        s_Data.TrailVBO = VertexBuffer::Create(ParticleBatchData::MaxTrailVertices * sizeof(TrailVertex));
        s_Data.TrailVBO->SetLayout({ { ShaderDataType::Float3, "a_Position" },
                                     { ShaderDataType::Float4, "a_Color" },
                                     { ShaderDataType::Float2, "a_TexCoord" },
                                     { ShaderDataType::Int, "a_EntityID" } });
        s_Data.TrailVAO->AddVertexBuffer(s_Data.TrailVBO);

        // Pre-generate index buffer for trail quads (0-1-2, 2-3-0 pattern)
        auto trailIndices = std::make_unique<u32[]>(ParticleBatchData::MaxTrailIndices);
        for (u32 i = 0; i < ParticleBatchData::MaxTrailQuads; ++i)
        {
            u32 base = i * 4;
            u32 idx = i * 6;
            trailIndices[idx + 0] = base + 0;
            trailIndices[idx + 1] = base + 1;
            trailIndices[idx + 2] = base + 2;
            trailIndices[idx + 3] = base + 2;
            trailIndices[idx + 4] = base + 3;
            trailIndices[idx + 5] = base + 0;
        }
        auto trailIBO = IndexBuffer::Create(trailIndices.get(), ParticleBatchData::MaxTrailIndices);
        s_Data.TrailVAO->SetIndexBuffer(trailIBO);

        s_Data.TrailVertexBase = std::make_unique<TrailVertex[]>(ParticleBatchData::MaxTrailVertices);

        s_Data.TrailShader = Shader::Create("assets/shaders/Particle_Trail.glsl");

        // GPU billboard shader (reads particle data from SSBO)
        s_Data.GPUBillboardShader = Shader::Create("assets/shaders/Particle_Billboard_GPU.glsl");
    }

    void ParticleBatchRenderer::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        s_Data.InstanceBase.reset();
        s_Data.InstancePtr = nullptr;

        s_Data.TrailVertexBase.reset();
        s_Data.TrailVertexPtr = nullptr;
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

        // Reset trail state for the new frame
        s_Data.TrailQuadCount = 0;
        s_Data.TrailVertexPtr = s_Data.TrailVertexBase.get();
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

        // Reset trail state for the new frame
        s_Data.TrailQuadCount = 0;
        s_Data.TrailVertexPtr = s_Data.TrailVertexBase.get();
    }

    void ParticleBatchRenderer::SetSoftParticleParams(const SoftParticleParams& params)
    {
        s_Data.SoftParams = params;
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

    // Upload shared particle params UBO and rebind camera + params UBOs
    static void UploadParticleParams(bool hasTexture)
    {
        auto& params = s_Data.ParticleParamsBuffer;
        params.CameraRight = s_Data.CameraRight;
        params.CameraUp = s_Data.CameraUp;
        params.HasTexture = hasTexture ? 1 : 0;
        params.SoftParticlesEnabled = s_Data.SoftParams.Enabled ? 1 : 0;
        params.SoftParticleDistance = s_Data.SoftParams.Distance;
        params.NearClip = s_Data.SoftParams.NearClip;
        params.FarClip = s_Data.SoftParams.FarClip;
        params.ViewportSize = s_Data.SoftParams.ViewportSize;
        s_Data.ParticleParamsUBO->SetData(&params, sizeof(params));

        // Re-bind UBOs (ScenePass CommandDispatch may overwrite binding points)
        s_Data.CameraUBO->Bind();
        s_Data.ParticleParamsUBO->Bind();
    }

    // Bind textures for particle rendering (slot 0 = diffuse, slot 1 = depth for soft particles)
    static void BindParticleTextures(bool hasTexture, u32 textureID)
    {
        RenderCommand::BindTexture(0, hasTexture ? textureID : s_Data.WhiteTexture->GetRendererID());
        RenderCommand::BindTexture(1, s_Data.SoftParams.Enabled ? s_Data.SoftParams.DepthTextureID : s_Data.WhiteTexture->GetRendererID());
    }

    void ParticleBatchRenderer::EndBatch()
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.InstanceCount > 0)
        {
            Flush();
        }

        if (s_Data.TrailQuadCount > 0)
        {
            FlushTrails();
        }

        s_Data.CurrentTexture = nullptr;
        s_Data.CurrentTrailTexture = nullptr;
        s_Data.SoftParams = {};
    }

    void ParticleBatchRenderer::Flush()
    {
        if (s_Data.InstanceCount == 0)
        {
            return;
        }

        OLO_PROFILE_FUNCTION();

        // Upload instance data to GPU
        u32 dataSize = s_Data.InstanceCount * sizeof(ParticleInstance);
        s_Data.InstanceVBO->SetData({ s_Data.InstanceBase.get(), dataSize });

        // Populate ParticleParams UBO
        bool hasTexture = (s_Data.CurrentTexture && s_Data.CurrentTexture != s_Data.WhiteTexture);
        UploadParticleParams(hasTexture);

        // Bind shader
        s_Data.ParticleShader->Bind();

        // Bind textures
        BindParticleTextures(hasTexture, hasTexture ? s_Data.CurrentTexture->GetRendererID() : 0);

        // Instanced draw call
        RenderCommand::DrawIndexedInstanced(s_Data.VAO, 6, s_Data.InstanceCount);

        s_Data.Stats.DrawCalls++;
        s_Data.Stats.InstanceCount += s_Data.InstanceCount;
    }

    void ParticleBatchRenderer::RenderMeshParticles(const Ref<Mesh>& mesh,
                                                    std::span<const MeshParticleInstance> instances,
                                                    const Ref<Texture2D>& texture)
    {
        if (!mesh || !mesh->IsValid() || instances.empty())
        {
            return;
        }

        OLO_PROFILE_FUNCTION();

        // Populate ParticleParams UBO (reuse the same UBO at binding 2)
        bool hasTexture = (texture != nullptr);
        UploadParticleParams(hasTexture);

        // Re-bind mesh instance UBO (ScenePass CommandDispatch may overwrite binding 3)
        s_Data.MeshInstanceUBO->Bind();

        // Bind mesh shader
        s_Data.MeshParticleShader->Bind();

        // Bind textures
        BindParticleTextures(hasTexture, hasTexture ? texture->GetRendererID() : 0);

        auto vao = mesh->GetVertexArray();
        u32 indexCount = mesh->GetIndexCount();

        // Render each mesh particle individually (one draw call per particle)
        // gl_InstanceIndex is not supported in the engine's shader cross-compilation
        // pipeline (spirv-cross outputs gl_InstanceID which shaderc rejects), so we
        // pass a single instance per UBO and draw without instancing.
        for (u32 i = 0; i < instances.size(); ++i)
        {
            s_Data.MeshInstanceUBO->SetData(&instances[i], sizeof(MeshParticleInstance));
            RenderCommand::DrawIndexed(vao, indexCount);

            s_Data.Stats.DrawCalls++;
            s_Data.Stats.InstanceCount++;
        }
    }

    void ParticleBatchRenderer::SubmitTrailQuad(std::span<const glm::vec3, 4> positions,
                                                std::span<const glm::vec4, 4> colors,
                                                std::span<const glm::vec2, 4> texCoords,
                                                int entityID)
    {
        if (s_Data.TrailQuadCount >= ParticleBatchData::MaxTrailQuads)
        {
            FlushTrails();
            s_Data.TrailQuadCount = 0;
            s_Data.TrailVertexPtr = s_Data.TrailVertexBase.get();
        }

        for (u32 i = 0; i < 4; ++i)
        {
            s_Data.TrailVertexPtr->Position = positions[i];
            s_Data.TrailVertexPtr->Color = colors[i];
            s_Data.TrailVertexPtr->TexCoord = texCoords[i];
            s_Data.TrailVertexPtr->EntityID = entityID;
            ++s_Data.TrailVertexPtr;
        }

        ++s_Data.TrailQuadCount;
    }

    void ParticleBatchRenderer::SetTrailTexture(const Ref<Texture2D>& texture)
    {
        const auto& newTex = texture ? texture : s_Data.WhiteTexture;

        if (s_Data.CurrentTrailTexture && s_Data.CurrentTrailTexture != newTex && s_Data.TrailQuadCount > 0)
        {
            FlushTrails();
            s_Data.TrailQuadCount = 0;
            s_Data.TrailVertexPtr = s_Data.TrailVertexBase.get();
        }

        s_Data.CurrentTrailTexture = newTex;
    }

    void ParticleBatchRenderer::FlushTrails()
    {
        if (s_Data.TrailQuadCount == 0)
        {
            return;
        }

        OLO_PROFILE_FUNCTION();

        // Upload trail vertex data
        u32 dataSize = static_cast<u32>(reinterpret_cast<u8*>(s_Data.TrailVertexPtr) - reinterpret_cast<u8*>(s_Data.TrailVertexBase.get()));
        s_Data.TrailVBO->SetData({ s_Data.TrailVertexBase.get(), dataSize });

        // Populate ParticleParams UBO
        bool hasTexture = (s_Data.CurrentTrailTexture && s_Data.CurrentTrailTexture != s_Data.WhiteTexture);
        UploadParticleParams(hasTexture);

        // Bind trail shader
        s_Data.TrailShader->Bind();

        // Bind textures
        BindParticleTextures(hasTexture, hasTexture ? s_Data.CurrentTrailTexture->GetRendererID() : 0);

        // Draw trail quads
        u32 indexCount = s_Data.TrailQuadCount * 6;
        RenderCommand::DrawIndexed(s_Data.TrailVAO, indexCount);

        s_Data.Stats.DrawCalls++;
        s_Data.Stats.InstanceCount += s_Data.TrailQuadCount;
    }

    void ParticleBatchRenderer::RenderGPUBillboards(GPUParticleSystem& gpuSystem,
                                                    const Ref<Texture2D>& texture)
    {
        OLO_PROFILE_FUNCTION();

        if (!gpuSystem.IsInitialized())
        {
            return;
        }

        // Populate ParticleParams UBO
        bool hasTexture = (texture != nullptr);
        UploadParticleParams(hasTexture);

        // Bind GPU billboard shader
        s_Data.GPUBillboardShader->Bind();

        // Bind particle and alive-index SSBOs so the vertex shader can read them
        gpuSystem.GetParticleSSBO()->Bind();
        gpuSystem.GetAliveIndexSSBO()->Bind();

        // Bind textures
        BindParticleTextures(hasTexture, hasTexture ? texture->GetRendererID() : 0);

        // Indirect draw using the quad VAO (same unit quad as CPU path)
        RenderCommand::DrawElementsIndirect(s_Data.VAO, gpuSystem.GetIndirectDrawSSBO()->GetRendererID());

        s_Data.Stats.DrawCalls++;
        // InstanceCount not updated here: GetAliveCount() requires a GPU→CPU readback that would stall the pipeline
    }

    void ParticleBatchRenderer::StartNewBatch()
    {
        s_Data.InstanceCount = 0;
        s_Data.InstancePtr = s_Data.InstanceBase.get();
    }

    void ParticleBatchRenderer::ResetStats()
    {
        s_Data.Stats = {};
    }

    ParticleBatchRenderer::Statistics ParticleBatchRenderer::GetStats()
    {
        return s_Data.Stats;
    }
} // namespace OloEngine
