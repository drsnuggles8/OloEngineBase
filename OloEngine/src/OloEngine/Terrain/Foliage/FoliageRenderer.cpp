#include "OloEnginePCH.h"
#include "FoliageRenderer.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/Buffer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Renderer/BoundingVolume.h"

#include <glm/gtc/constants.hpp>

namespace OloEngine
{
    // Simple hash for deterministic placement — returns float in [0, 1)
    static f32 HashPosition(f32 x, f32 z, u32 seed)
    {
        u32 h = static_cast<u32>(x * 73856093.0f) ^ static_cast<u32>(z * 19349663.0f) ^ seed;
        h = (h * 2654435761u) >> 16;
        return static_cast<f32>(h & 0xFFFF) / 65536.0f;
    }

    void FoliageRenderer::BuildQuadGeometry(LayerRenderData& data)
    {
        // Billboard quad: 4 vertices, centered at bottom
        // Positions in local space, billboard rotation handled in shader
        f32 quadVertices[] = {
            // x,    y,    z,    u,    v
            -0.5f,
            0.0f,
            0.0f,
            0.0f,
            0.0f, // bottom-left
            0.5f,
            0.0f,
            0.0f,
            1.0f,
            0.0f, // bottom-right
            0.5f,
            1.0f,
            0.0f,
            1.0f,
            1.0f, // top-right
            -0.5f,
            1.0f,
            0.0f,
            0.0f,
            1.0f, // top-left
        };

        u32 indices[] = { 0, 1, 2, 2, 3, 0 };

        data.VAO = VertexArray::Create();

        data.QuadVBO = VertexBuffer::Create(quadVertices, sizeof(quadVertices));
        data.QuadVBO->SetLayout({
            { ShaderDataType::Float3, "a_Position" },
            { ShaderDataType::Float2, "a_TexCoord" },
        });
        data.VAO->AddVertexBuffer(data.QuadVBO);

        data.IBO = IndexBuffer::Create(indices, 6);
        data.VAO->SetIndexBuffer(data.IBO);
        data.IndexCount = 6;
    }

    void FoliageRenderer::UploadInstances(LayerRenderData& data, const std::vector<FoliageInstanceData>& instances)
    {
        if (instances.empty())
        {
            data.InstanceCount = 0;
            return;
        }

        u32 dataSize = static_cast<u32>(instances.size() * sizeof(FoliageInstanceData));

        // Create or resize instance buffer
        if (!data.InstanceVBO || data.InstanceCount < instances.size())
        {
            data.InstanceVBO = VertexBuffer::Create(dataSize);
            data.InstanceVBO->SetLayout({
                { ShaderDataType::Float4, "a_PositionScale" },
                { ShaderDataType::Float4, "a_RotationHeight" },
                { ShaderDataType::Float4, "a_ColorAlpha" },
            });
            data.VAO->AddInstanceBuffer(data.InstanceVBO);
        }

        data.InstanceVBO->SetData({ instances.data(), dataSize });
        data.InstanceCount = static_cast<u32>(instances.size());
    }

    void FoliageRenderer::GenerateInstances(
        const std::vector<FoliageLayer>& layers,
        const TerrainData& terrainData,
        const TerrainMaterial* material,
        f32 worldSizeX, f32 worldSizeZ, f32 heightScale)
    {
        OLO_PROFILE_FUNCTION();

        m_Layers.resize(layers.size());

        for (sizet layerIdx = 0; layerIdx < layers.size(); ++layerIdx)
        {
            const auto& layer = layers[layerIdx];
            auto& renderData = m_Layers[layerIdx];

            if (!layer.Enabled || layer.Density <= 0.0f)
            {
                renderData.InstanceCount = 0;
                continue;
            }

            // Build quad geometry on first use
            if (!renderData.VAO)
            {
                BuildQuadGeometry(renderData);
            }

            // Store layer render properties
            renderData.ViewDistance = layer.ViewDistance;
            renderData.FadeStartDistance = layer.FadeStartDistance;
            renderData.WindStrength = layer.WindStrength;
            renderData.WindSpeed = layer.WindSpeed;
            renderData.BaseColor = layer.BaseColor;
            renderData.AlphaCutoff = layer.AlphaCutoff;

            // Load albedo texture if needed
            if (!layer.AlbedoPath.empty() && !renderData.AlbedoTexture)
            {
                renderData.AlbedoTexture = Texture2D::Create(layer.AlbedoPath);
            }

            // Calculate grid spacing from density
            f32 spacing = 1.0f / std::sqrt(layer.Density);
            u32 countX = static_cast<u32>(std::ceil(worldSizeX / spacing));
            u32 countZ = static_cast<u32>(std::ceil(worldSizeZ / spacing));

            // Get splatmap data for density masking
            const u8* splatData = nullptr;
            u32 splatRes = 0;
            if (material && layer.SplatmapChannel >= 0 && layer.SplatmapChannel < 8)
            {
                i32 splatIdx = layer.SplatmapChannel / 4;
                if (material->HasCPUSplatmaps())
                {
                    const auto& splatmapVec = material->GetSplatmapData(static_cast<u32>(splatIdx));
                    if (!splatmapVec.empty())
                    {
                        splatData = splatmapVec.data();
                        splatRes = material->GetSplatmapResolution();
                    }
                }
            }

            f32 cosMinSlope = std::cos(glm::radians(layer.MaxSlopeAngle));
            f32 cosMaxSlope = std::cos(glm::radians(layer.MinSlopeAngle));

            std::vector<FoliageInstanceData> instances;
            instances.reserve(countX * countZ / 4); // Estimate ~25% coverage

            u32 seed = static_cast<u32>(layerIdx * 17 + 31);

            for (u32 iz = 0; iz < countZ; ++iz)
            {
                for (u32 ix = 0; ix < countX; ++ix)
                {
                    // Jittered position
                    f32 jx = HashPosition(static_cast<f32>(ix), static_cast<f32>(iz), seed);
                    f32 jz = HashPosition(static_cast<f32>(ix), static_cast<f32>(iz), seed + 7);

                    f32 worldX = (static_cast<f32>(ix) + jx) * spacing;
                    f32 worldZ = (static_cast<f32>(iz) + jz) * spacing;

                    if (worldX >= worldSizeX || worldZ >= worldSizeZ)
                        continue;

                    f32 nx = worldX / worldSizeX;
                    f32 nz = worldZ / worldSizeZ;

                    // Slope check
                    glm::vec3 normal = terrainData.GetNormalAt(nx, nz, worldSizeX, worldSizeZ, heightScale);
                    f32 upDot = normal.y; // dot(normal, up)
                    if (upDot < cosMinSlope || upDot > cosMaxSlope)
                        continue;

                    // Splatmap density check
                    if (splatData && splatRes > 0)
                    {
                        u32 sx = std::min(static_cast<u32>(nx * static_cast<f32>(splatRes)), splatRes - 1);
                        u32 sz = std::min(static_cast<u32>(nz * static_cast<f32>(splatRes)), splatRes - 1);
                        i32 channelInSplat = layer.SplatmapChannel % 4;
                        // Splatmap is RGBA packed, so index = (sz * splatRes + sx) * 4 + channel
                        f32 splatWeight = static_cast<f32>(splatData[(sz * splatRes + sx) * 4 + channelInSplat]) / 255.0f;
                        f32 threshold = HashPosition(static_cast<f32>(ix) + 0.5f, static_cast<f32>(iz) + 0.5f, seed + 13);
                        if (threshold > splatWeight)
                            continue;
                    }

                    // Height
                    f32 height = terrainData.GetHeightAt(nx, nz) * heightScale;

                    // Randomize scale and height
                    f32 scaleRand = HashPosition(static_cast<f32>(ix), static_cast<f32>(iz), seed + 3);
                    f32 heightRand = HashPosition(static_cast<f32>(ix), static_cast<f32>(iz), seed + 5);
                    f32 scale = glm::mix(layer.MinScale, layer.MaxScale, scaleRand);
                    f32 instanceHeight = glm::mix(layer.MinHeight, layer.MaxHeight, heightRand);

                    // Random rotation
                    f32 rotation = 0.0f;
                    if (layer.RandomRotation)
                    {
                        rotation = HashPosition(static_cast<f32>(ix), static_cast<f32>(iz), seed + 11) * glm::two_pi<f32>();
                    }

                    FoliageInstanceData instance;
                    instance.PositionScale = glm::vec4(worldX, height, worldZ, scale);
                    instance.RotationHeight = glm::vec4(rotation, instanceHeight, 1.0f, 0.0f); // fade=1 (full)
                    instance.ColorAlpha = glm::vec4(layer.BaseColor, layer.AlphaCutoff);
                    instances.push_back(instance);
                }
            }

            UploadInstances(renderData, instances);
        }
    }

    void FoliageRenderer::Render(
        const Frustum& frustum,
        const glm::vec3& cameraPos,
        const Ref<Shader>& shader)
    {
        OLO_PROFILE_FUNCTION();

        if (!shader)
            return;

        shader->Bind();
        m_VisibleInstances = 0;

        for (auto& layer : m_Layers)
        {
            if (layer.InstanceCount == 0 || !layer.VAO)
                continue;

            // Upload per-layer foliage UBO
            ShaderBindingLayout::FoliageUBO foliageUBOData{};
            foliageUBOData.Time = m_Time;
            foliageUBOData.WindStrength = layer.WindStrength;
            foliageUBOData.WindSpeed = layer.WindSpeed;
            foliageUBOData.ViewDistance = layer.ViewDistance;
            foliageUBOData.FadeStart = layer.FadeStartDistance;
            foliageUBOData.AlphaCutoff = layer.AlphaCutoff;
            foliageUBOData.BaseColor = layer.BaseColor;
            auto foliageUBO = Renderer3D::GetFoliageUBO();
            foliageUBO->SetData(&foliageUBOData, ShaderBindingLayout::FoliageUBO::GetSize());

            // Bind albedo texture
            if (layer.AlbedoTexture)
            {
                layer.AlbedoTexture->Bind(0); // TEX_DIFFUSE
            }

            layer.VAO->Bind();
            RenderCommand::DrawIndexedInstanced(layer.VAO, layer.IndexCount, layer.InstanceCount);
            m_VisibleInstances += layer.InstanceCount;
        }
    }

    void FoliageRenderer::RenderShadows(const Ref<Shader>& depthShader)
    {
        OLO_PROFILE_FUNCTION();

        if (!depthShader)
            return;

        depthShader->Bind();

        for (auto& layer : m_Layers)
        {
            if (layer.InstanceCount == 0 || !layer.VAO)
                continue;

            // Upload per-layer foliage UBO for depth pass
            ShaderBindingLayout::FoliageUBO foliageUBOData{};
            foliageUBOData.Time = m_Time;
            foliageUBOData.WindStrength = layer.WindStrength;
            foliageUBOData.WindSpeed = layer.WindSpeed;
            foliageUBOData.AlphaCutoff = layer.AlphaCutoff;
            auto foliageUBO = Renderer3D::GetFoliageUBO();
            foliageUBO->SetData(&foliageUBOData, ShaderBindingLayout::FoliageUBO::GetSize());

            // Bind albedo for alpha test in shadow pass
            if (layer.AlbedoTexture)
            {
                layer.AlbedoTexture->Bind(0);
            }

            layer.VAO->Bind();
            RenderCommand::DrawIndexedInstanced(layer.VAO, layer.IndexCount, layer.InstanceCount);
        }
    }

    u32 FoliageRenderer::GetTotalInstanceCount() const
    {
        u32 total = 0;
        for (const auto& layer : m_Layers)
        {
            total += layer.InstanceCount;
        }
        return total;
    }
} // namespace OloEngine
