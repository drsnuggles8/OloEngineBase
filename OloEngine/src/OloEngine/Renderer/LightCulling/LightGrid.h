#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/StorageBuffer.h"

namespace OloEngine
{
    struct LightGridConfig
    {
        u32 TileSizePixels = 16;
        u32 MaxLightsPerTile = 256;
        u32 DepthSlices = 1; // 1 = tiled (2D), >1 = clustered (3D)
        f32 NearPlane = 0.1f;
        f32 FarPlane = 1000.0f;
    };

    // @brief Manages screen-space tile/cluster grid for Forward+ light culling.
    //
    // Divides the screen into tiles (2D) or clusters (3D) and maintains GPU
    // resources for per-tile light assignment: a light index list SSBO and
    // a light grid SSBO storing (offset, count) pairs for each tile.
    class LightGrid
    {
      public:
        LightGrid() = default;
        ~LightGrid() = default;

        void Initialize(u32 screenWidth, u32 screenHeight, const LightGridConfig& config);
        void Resize(u32 screenWidth, u32 screenHeight);

        // Bind all grid SSBOs to their shader binding points
        void Bind() const;
        void Unbind() const;

        // Reset the atomic counter before each culling dispatch
        void ResetAtomicCounter();

        [[nodiscard]] u32 GetTileCountX() const
        {
            return m_TileCountX;
        }
        [[nodiscard]] u32 GetTileCountY() const
        {
            return m_TileCountY;
        }
        [[nodiscard]] u32 GetDepthSlices() const
        {
            return m_Config.DepthSlices;
        }
        [[nodiscard]] u32 GetTotalClusters() const
        {
            return m_TileCountX * m_TileCountY * m_Config.DepthSlices;
        }
        [[nodiscard]] u32 GetTileSizePixels() const
        {
            return m_Config.TileSizePixels;
        }
        [[nodiscard]] u32 GetMaxLightsPerTile() const
        {
            return m_Config.MaxLightsPerTile;
        }

        [[nodiscard]] u32 GetScreenWidth() const
        {
            return m_ScreenWidth;
        }
        [[nodiscard]] u32 GetScreenHeight() const
        {
            return m_ScreenHeight;
        }

        [[nodiscard]] const Ref<StorageBuffer>& GetLightIndexSSBO() const
        {
            return m_LightIndexSSBO;
        }
        [[nodiscard]] const Ref<StorageBuffer>& GetLightGridSSBO() const
        {
            return m_LightGridSSBO;
        }
        [[nodiscard]] Ref<StorageBuffer>& GetLightGridSSBO()
        {
            return m_LightGridSSBO;
        }
        [[nodiscard]] const Ref<StorageBuffer>& GetGlobalIndexSSBO() const
        {
            return m_GlobalIndexSSBO;
        }

        [[nodiscard]] bool IsInitialized() const
        {
            return m_Initialized;
        }

      private:
        void CreateBuffers();

        LightGridConfig m_Config;
        u32 m_ScreenWidth = 0;
        u32 m_ScreenHeight = 0;
        u32 m_TileCountX = 0;
        u32 m_TileCountY = 0;

        Ref<StorageBuffer> m_LightIndexSSBO;  // Flat array of light indices
        Ref<StorageBuffer> m_LightGridSSBO;   // Per-tile (offset, count) pairs
        Ref<StorageBuffer> m_GlobalIndexSSBO; // Atomic counter for light list append

        bool m_Initialized = false;
    };
} // namespace OloEngine
