#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Impostor/ImpostorBaker.h"
#include "OloEngine/Terrain/Foliage/FoliageLayer.h"

#include <glm/glm.hpp>
#include <string>
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

    // Lightweight POD struct exposing per-layer data needed for command submission.
    // Avoids leaking internal LayerRenderData internals (Ref<VertexArray> etc.).
    // Uses u32 for GL resource IDs to avoid pulling in RenderCommand.h.
    struct FoliageLayerDrawInfo
    {
        u32 VertexArrayID = 0;
        u32 IndexCount = 0;
        u32 InstanceCount = 0;
        u32 AlbedoTextureID = 0;
        f32 ViewDistance = 100.0f;
        f32 FadeStartDistance = 80.0f;
        f32 WindStrength = 0.3f;
        f32 WindSpeed = 1.0f;
        glm::vec3 BaseColor{ 1.0f };
        f32 AlphaCutoff = 0.5f;
        BoundingBox Bounds; // Precomputed AABB encompassing all instances in this layer

        // Octahedral impostor LOD (issue #433). UseImpostor + valid atlas IDs
        // route this layer through the impostor card shader instead of the flat
        // billboard; zero/false leaves the existing billboard path untouched.
        bool UseImpostor = false;
        u32 ImpostorAlbedoAtlasID = 0;
        u32 ImpostorNormalDepthAtlasID = 0;
        u32 ImpostorFramesPerAxis = 8;
        bool ImpostorHemi = true;
        f32 ImpostorStartDistance = 40.0f;
        f32 ImpostorTransitionBand = 15.0f;
        f32 ImpostorRadius = 1.0f;
    };

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
        [[nodiscard]] u32 GetVisibleInstanceCount() const
        {
            return m_VisibleInstances;
        }

        // Returns draw info for all active layers (InstanceCount > 0 && VAO valid).
        // Used by Scene to create DrawFoliageLayerCommand packets per layer.
        [[nodiscard]] std::vector<FoliageLayerDrawInfo> GetActiveLayerDrawInfo() const;

        void SetTime(f32 time, f32 prevTime)
        {
            m_Time = time;
            m_PrevTime = prevTime;
        }

      private:
        // Internal per-layer GPU data
        struct LayerRenderData
        {
            Ref<VertexArray> VAO;
            Ref<VertexBuffer> QuadVBO;     // Geometry (unit quad)
            Ref<VertexBuffer> InstanceVBO; // Per-instance data
            Ref<IndexBuffer> IBO;
            u32 InstanceCount = 0;
            u32 InstanceCapacity = 0;
            u32 IndexCount = 0;
            f32 ViewDistance = 100.0f;
            f32 FadeStartDistance = 80.0f;
            f32 WindStrength = 0.3f;
            f32 WindSpeed = 1.0f;
            glm::vec3 BaseColor{ 1.0f };
            f32 AlphaCutoff = 0.5f;
            Ref<Texture2D> AlbedoTexture;
            BoundingBox Bounds; // Precomputed AABB encompassing all instances

            // Octahedral impostor (issue #433). Baked lazily from the layer mesh;
            // the *Baked* fields cache the config the atlas was baked for so a
            // regenerate only re-bakes when the mesh / grid / layout changes.
            ImpostorAtlas Impostor;
            bool UseImpostor = false;
            f32 ImpostorStartDistance = 40.0f;
            f32 ImpostorTransitionBand = 15.0f;
            std::string ImpostorBakedMeshPath;
            std::string ImpostorBakedAlbedoPath;
            glm::vec3 ImpostorBakedBaseColor{ 0.0f };
            f32 ImpostorBakedAlphaCutoff = 0.0f;
            u32 ImpostorBakedFrames = 0;
            u32 ImpostorBakedResolution = 0;
            bool ImpostorBakedHemi = true;
        };

        void BuildQuadGeometry(LayerRenderData& data) const;
        void UploadInstances(LayerRenderData& data, const std::vector<FoliageInstanceData>& instances);

        // Bakes (or re-bakes) the layer's octahedral impostor atlas if UseImpostor
        // and the mesh/grid/layout differs from what was last baked. No-op otherwise.
        void UpdateImpostorAtlas(LayerRenderData& data, const FoliageLayer& layer);

        std::vector<LayerRenderData> m_Layers;
        u32 m_VisibleInstances = 0;
        f32 m_Time = 0.0f;
        f32 m_PrevTime = 0.0f;
    };
} // namespace OloEngine
