#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Terrain/Foliage/FoliageLayer.h"

#include <glm/glm.hpp>
#include <vector>

namespace OloEngine
{
    class VertexArray;
    class VertexBuffer;
    class IndexBuffer;
    class Shader;
    class TerrainData;
    class TerrainMaterial;
    class Frustum;

    // Manages foliage instance generation, culling, and instanced rendering.
    // Generates instances on the CPU from terrain data + foliage layer config,
    // uploads to a per-layer instance VBO, and draws with DrawIndexedInstanced.
    class FoliageRenderer : public RefCounted
    {
      public:
        FoliageRenderer() = default;

        // Regenerate all instances for the given layers from terrain data.
        // Call when terrain changes (erosion, sculpting) or layer settings change.
        void GenerateInstances(
            const std::vector<FoliageLayer>& layers,
            const TerrainData& terrainData,
            const TerrainMaterial* material,
            f32 worldSizeX, f32 worldSizeZ, f32 heightScale);

        // Render all visible foliage layers (frustum culled per-chunk groups)
        void Render(
            const Frustum& frustum,
            const glm::vec3& cameraPos,
            const Ref<Shader>& shader);

        // Render shadow depth pass for all layers
        void RenderShadows(const Ref<Shader>& depthShader);

        [[nodiscard]] u32 GetTotalInstanceCount() const;
        [[nodiscard]] u32 GetVisibleInstanceCount() const { return m_VisibleInstances; }

        void SetTime(f32 time) { m_Time = time; }

      private:
        // Internal per-layer GPU data
        struct LayerRenderData
        {
            Ref<VertexArray> VAO;
            Ref<VertexBuffer> QuadVBO;     // Geometry (unit quad)
            Ref<VertexBuffer> InstanceVBO; // Per-instance data
            Ref<IndexBuffer> IBO;
            u32 InstanceCount = 0;
            u32 IndexCount = 0;
            f32 ViewDistance = 100.0f;
            f32 FadeStartDistance = 80.0f;
            f32 WindStrength = 0.3f;
            f32 WindSpeed = 1.0f;
            glm::vec3 BaseColor{1.0f};
            f32 AlphaCutoff = 0.5f;
            Ref<Texture2D> AlbedoTexture;
        };

        void BuildQuadGeometry(LayerRenderData& data);
        void UploadInstances(LayerRenderData& data, const std::vector<FoliageInstanceData>& instances);

        std::vector<LayerRenderData> m_Layers;
        u32 m_VisibleInstances = 0;
        f32 m_Time = 0.0f;
    };
} // namespace OloEngine
