#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Texture.h"

#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace OloEngine
{
    class TerrainData : public Asset
    {
      public:
        TerrainData() = default;
        ~TerrainData() override = default;

        // Load heightmap from a 16-bit PNG or raw R32F file
        bool LoadFromFile(const std::string& path);

        // Create a flat heightmap of given resolution
        void CreateFlat(u32 resolution, f32 defaultHeight = 0.0f);

        // Generate procedural terrain using fBm simplex noise
        // seed: random seed, octaves: detail layers, frequency: base scale, amplitude: height variation
        // lacunarity: frequency multiplier per octave, persistence: amplitude multiplier per octave
        void GenerateProcedural(u32 resolution, i32 seed, u32 octaves = 6,
                                f32 frequency = 2.0f, f32 amplitude = 1.0f,
                                f32 lacunarity = 2.0f, f32 persistence = 0.5f);

        // CPU height query with bilinear interpolation — normalizedX/Z in [0, 1]
        [[nodiscard]] f32 GetHeightAt(f32 normalizedX, f32 normalizedZ) const;

        // CPU normal query from finite differences
        [[nodiscard]] glm::vec3 GetNormalAt(f32 normalizedX, f32 normalizedZ, f32 worldSizeX, f32 worldSizeZ, f32 heightScale) const;

        [[nodiscard]] u32 GetResolution() const { return m_Resolution; }
        [[nodiscard]] const std::vector<f32>& GetHeightData() const { return m_Heights; }
        [[nodiscard]] std::vector<f32>& GetHeightData() { return m_Heights; }
        [[nodiscard]] Ref<Texture2D> GetGPUHeightmap() const { return m_GPUHeightmap; }

        // Re-upload full heightmap to GPU (call after CPU edits)
        void UploadToGPU();

        // Re-upload a rectangular region to GPU (partial update for brush editing)
        void UploadRegionToGPU(u32 x, u32 y, u32 width, u32 height);

        // Export heightmap to file
        // R32F raw format: direct float array dump (resolution × resolution × 4 bytes)
        bool ExportRawR32F(const std::string& path) const;
        // R16 raw format: quantized to 16-bit unsigned (resolution × resolution × 2 bytes)
        bool ExportRawR16(const std::string& path) const;

        // Asset interface
        static AssetType GetStaticType() { return AssetType::Terrain; }
        AssetType GetAssetType() const override { return GetStaticType(); }

      private:
        u32 m_Resolution = 0;         // Heightmap is m_Resolution × m_Resolution
        std::vector<f32> m_Heights;   // Row-major CPU heightmap [0, 1] range
        Ref<Texture2D> m_GPUHeightmap; // R32F GPU texture
    };
} // namespace OloEngine
