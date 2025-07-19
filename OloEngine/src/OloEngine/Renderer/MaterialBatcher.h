#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Texture.h"
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    /**
     * @brief Material Batching System for efficient texture binding and shader switching
     * 
     * This system groups materials by compatibility to minimize state changes and
     * improve rendering performance by batching similar materials together.
     */
    class MaterialBatcher
    {
    public:
        struct BatchInfo
        {
            Ref<Shader> Shader;
            std::vector<Ref<Texture2D>> Textures;  // Batched textures
            std::vector<Material*> Materials; // Materials in this batch
            u32 MaterialCount = 0;
            bool IsCompatible = true;
        };

        struct BatchingStats
        {
            u32 TotalMaterials = 0;
            u32 BatchCount = 0;
            u32 TextureSwitches = 0;
            u32 ShaderSwitches = 0;
            float BatchingEfficiency = 0.0f; // Percentage of materials successfully batched
        };

    public:
        MaterialBatcher();
        ~MaterialBatcher() = default;

        /**
         * @brief Add a material to the batching system
         * @param material Material to batch
         * @param lightCount Current light count for shader selection
         * @param isSkinnedMesh Whether this is for a skinned mesh
         */
        void AddMaterial(const Material* material, int lightCount, bool isSkinnedMesh = false);

        /**
         * @brief Process all added materials and create optimized batches
         */
        void ProcessBatches();

        /**
         * @brief Get all processed batches
         * @return Vector of batch information
         */
        const std::vector<BatchInfo>& GetBatches() const { return m_Batches; }

        /**
         * @brief Clear all batches and reset the batcher
         */
        void Clear();

        /**
         * @brief Get batching statistics
         * @return Current batching performance stats
         */
        const BatchingStats& GetStats() const { return m_Stats; }

        /**
         * @brief Set maximum number of textures per batch (hardware dependent)
         * @param maxTextures Maximum texture units available
         */
        void SetMaxTexturesPerBatch(u32 maxTextures) { m_MaxTexturesPerBatch = maxTextures; }

        /**
         * @brief Check if two materials can be batched together
         * @param mat1 First material
         * @param mat2 Second material
         * @return True if materials are compatible for batching
         */
        static bool AreCompatible(const Material* mat1, const Material* mat2);

    private:
        struct MaterialEntry
        {
            const Material* Material;
            Ref<Shader> OptimalShader;
            u64 MaterialKey;
            int LightCount;
            bool IsSkinnedMesh;
        };

        std::vector<MaterialEntry> m_PendingMaterials;
        std::vector<BatchInfo> m_Batches;
        BatchingStats m_Stats;
        u32 m_MaxTexturesPerBatch = 16; // Conservative default, can be set based on hardware

        /**
         * @brief Create a new batch from compatible materials
         * @param startIndex Starting index in pending materials
         * @return Number of materials added to the batch
         */
        u32 CreateBatch(u32 startIndex);

        /**
         * @brief Calculate compatibility score between two materials
         * @param mat1 First material
         * @param mat2 Second material
         * @return Compatibility score (higher = more compatible)
         */
        u32 CalculateCompatibilityScore(const Ref<Material>& mat1, const Ref<Material>& mat2);

        /**
         * @brief Sort pending materials for optimal batching
         */
        void SortMaterialsForBatching();

        /**
         * @brief Update batching statistics
         */
        void UpdateStats();
    };
}
