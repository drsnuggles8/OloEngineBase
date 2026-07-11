#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/LightCulling/ClusteredLighting.h"
#include "OloEngine/Renderer/StorageBuffer.h"

namespace OloEngine
{
    struct LightGridConfig
    {
        // Fixed cluster grid dimensions (see ClusteredLighting.h for the
        // rationale). Resolution-independent: the SSBOs are sized from these
        // once and survive window resizes untouched.
        u32 ClusterCountX = ClusteredLighting::kClusterCountX;
        u32 ClusterCountY = ClusteredLighting::kClusterCountY;
        u32 ClusterCountZ = ClusteredLighting::kClusterCountZ;
        u32 MaxLightsPerCluster = ClusteredLighting::kMaxLightsPerCluster;
    };

    // @brief Manages the 3D froxel cluster grid for clustered Forward+ light
    // culling (issue #435).
    //
    // Divides the view frustum into ClusterCountX × ClusterCountY screen
    // tiles × ClusterCountZ exponential depth slices and maintains GPU
    // resources for per-cluster light assignment: a light index list SSBO and
    // a light grid SSBO storing (offset, count) pairs for each cluster.
    class LightGrid
    {
      public:
        LightGrid() = default;
        ~LightGrid() = default;
        LightGrid(const LightGrid&) = delete;
        LightGrid& operator=(const LightGrid&) = delete;
        LightGrid(LightGrid&&) = delete;
        LightGrid& operator=(LightGrid&&) = delete;

        void Initialize(u32 screenWidth, u32 screenHeight, const LightGridConfig& config);
        void Shutdown();

        // Track the viewport dimensions (feeds the fragment-side tile scale).
        // Cluster-count buffers are resolution-independent, so no GPU
        // resources are recreated here.
        void Resize(u32 screenWidth, u32 screenHeight);

        // Bind all grid SSBOs to their shader binding points
        void Bind() const;
        void Unbind() const;

        // Reset the atomic counter before each culling dispatch
        void ResetAtomicCounter();

        // Clear all (offset, count) pairs in the light grid SSBO
        void ClearLightGrid();

        [[nodiscard]] u32 GetClusterCountX() const
        {
            return m_Config.ClusterCountX;
        }
        [[nodiscard]] u32 GetClusterCountY() const
        {
            return m_Config.ClusterCountY;
        }
        [[nodiscard]] u32 GetClusterCountZ() const
        {
            return m_Config.ClusterCountZ;
        }
        [[nodiscard]] u32 GetTotalClusters() const
        {
            return m_Config.ClusterCountX * m_Config.ClusterCountY * m_Config.ClusterCountZ;
        }
        [[nodiscard]] u32 GetMaxLightsPerCluster() const
        {
            return m_Config.MaxLightsPerCluster;
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
        [[nodiscard]] const Ref<StorageBuffer>& GetGlobalIndexSSBO() const
        {
            return m_GlobalIndexSSBO;
        }

        [[nodiscard]] bool IsInitialized() const
        {
            return m_Initialized;
        }

      private:
        bool CreateBuffers();

        LightGridConfig m_Config;
        u32 m_ScreenWidth = 0;
        u32 m_ScreenHeight = 0;

        Ref<StorageBuffer> m_LightIndexSSBO;  // Flat array of packed light indices
        Ref<StorageBuffer> m_LightGridSSBO;   // Per-cluster (offset, count) pairs
        Ref<StorageBuffer> m_GlobalIndexSSBO; // Atomic counter for light list append

        bool m_Initialized = false;
    };
} // namespace OloEngine
