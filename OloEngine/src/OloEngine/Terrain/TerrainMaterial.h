#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Terrain/TerrainLayer.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Texture2DArray.h"

#include <array>
#include <string>

namespace OloEngine
{
    // Manages terrain material layers and their GPU texture arrays.
    // Up to 8 layers supported via 2 RGBA8 splatmaps (4 channels each).
    // Packs per-layer textures into 3 Texture2DArrays: albedo, normal, ARM.
    class TerrainMaterial : public RefCounted
    {
      public:
        TerrainMaterial() = default;

        // Add a layer (returns layer index, or -1 if full)
        i32 AddLayer(const TerrainLayer& layer);

        // Remove a layer by index (shifts subsequent layers down)
        void RemoveLayer(u32 index);

        // Get/set layer at index
        [[nodiscard]] const TerrainLayer& GetLayer(u32 index) const;
        TerrainLayer& GetLayer(u32 index);

        [[nodiscard]] u32 GetLayerCount() const
        {
            return m_LayerCount;
        }

        // Build GPU texture arrays from layer textures.
        // Must be called after adding/modifying layers before rendering.
        void BuildTextureArrays(u32 layerResolution = 512);

        // Splatmap management
        void SetSplatmapPath(u32 index, const std::string& path); // index 0 or 1
        [[nodiscard]] const std::string& GetSplatmapPath(u32 index) const;
        void LoadSplatmaps();

        // GPU resources for rendering
        [[nodiscard]] Ref<Texture2DArray> GetAlbedoArray() const
        {
            return m_AlbedoArray;
        }
        [[nodiscard]] Ref<Texture2DArray> GetNormalArray() const
        {
            return m_NormalArray;
        }
        [[nodiscard]] Ref<Texture2DArray> GetARMArray() const
        {
            return m_ARMArray;
        }
        [[nodiscard]] Ref<Texture2D> GetSplatmap(u32 index) const;

        [[nodiscard]] bool IsBuilt() const
        {
            return m_AlbedoArray != nullptr;
        }

        // CPU splatmap data for paint brush editing
        void InitializeCPUSplatmaps(u32 resolution);
        [[nodiscard]] u32 GetSplatmapResolution() const
        {
            return m_SplatmapResolution;
        }
        [[nodiscard]] std::vector<u8>& GetSplatmapData(u32 index);
        [[nodiscard]] const std::vector<u8>& GetSplatmapData(u32 index) const;
        void UploadSplatmapRegion(u32 splatmapIndex, u32 x, u32 y, u32 w, u32 h);
        [[nodiscard]] bool HasCPUSplatmaps() const
        {
            return m_SplatmapResolution > 0 && !m_CPUSplatmaps[0].empty();
        }

      private:
        // Load a single texture, resize if needed, return RGBA8 pixel data
        static bool LoadTextureData(const std::string& path, u32 targetSize,
                                    std::vector<u8>& outData);

        std::array<TerrainLayer, MAX_TERRAIN_LAYERS> m_Layers;
        u32 m_LayerCount = 0;

        // Splatmaps (RGBA8): splatmap 0 = layers 0-3, splatmap 1 = layers 4-7
        std::array<std::string, 2> m_SplatmapPaths;
        std::array<Ref<Texture2D>, 2> m_Splatmaps;

        // GPU texture arrays (one layer per array slice)
        Ref<Texture2DArray> m_AlbedoArray;
        Ref<Texture2DArray> m_NormalArray;
        Ref<Texture2DArray> m_ARMArray;

        u32 m_LayerResolution = 512; // Resolution of each layer in the texture arrays

        // CPU-side splatmap pixel buffers (RGBA8, row-major)
        std::array<std::vector<u8>, 2> m_CPUSplatmaps;
        u32 m_SplatmapResolution = 0;
    };

} // namespace OloEngine
