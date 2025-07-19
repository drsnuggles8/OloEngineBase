#include "OloEnginePCH.h"
#include "OloEngine/Renderer/MaterialBatcher.h"
#include "OloEngine/Renderer/PBRValidation.h"
#include <algorithm>

namespace OloEngine
{
    MaterialBatcher::MaterialBatcher()
    {
        m_PendingMaterials.reserve(256); // Reserve space for typical scene
        m_Batches.reserve(32);           // Reserve space for typical batch count
    }

    void MaterialBatcher::AddMaterial(const Material* material, int lightCount, bool isSkinnedMesh)
    {
        if (!material)
        {
            OLO_CORE_WARN("MaterialBatcher::AddMaterial: Attempted to add null material");
            return;
        }

        // Select optimal shader for this material based on lighting conditions
        Ref<Shader> optimalShader = Material::SelectOptimalShader(lightCount, isSkinnedMesh);
        if (!optimalShader)
        {
            OLO_CORE_ERROR("MaterialBatcher::AddMaterial: Failed to select optimal shader for material");
            return;
        }

        // Create material entry
        MaterialEntry entry;
        entry.Material = material;
        entry.OptimalShader = optimalShader;
        entry.MaterialKey = material->CalculateKey();
        entry.LightCount = lightCount;
        entry.IsSkinnedMesh = isSkinnedMesh;

        m_PendingMaterials.push_back(entry);
        m_Stats.TotalMaterials++;
    }

    void MaterialBatcher::ProcessBatches()
    {
        if (m_PendingMaterials.empty())
        {
            return;
        }

        // Sort materials for optimal batching
        SortMaterialsForBatching();

        // Create batches from sorted materials
        u32 processedMaterials = 0;
        while (processedMaterials < m_PendingMaterials.size())
        {
            u32 materialsInBatch = CreateBatch(processedMaterials);
            processedMaterials += materialsInBatch;
        }

        // Update statistics
        UpdateStats();
    }

    void MaterialBatcher::Clear()
    {
        m_PendingMaterials.clear();
        m_Batches.clear();
        m_Stats = BatchingStats{};
    }

    bool MaterialBatcher::AreCompatible(const Material* mat1, const Material* mat2)
    {
        if (!mat1 || !mat2)
            return false;

        // Materials are compatible if they use the same shader
        if (mat1->GetShader() != mat2->GetShader())
            return false;

        // Check if they have similar PBR properties (for UBO batching potential)
        const float tolerance = 0.01f;
        
        // Check metallic and roughness factors
        if (std::abs(mat1->MetallicFactor - mat2->MetallicFactor) > tolerance)
            return false;
            
        if (std::abs(mat1->RoughnessFactor - mat2->RoughnessFactor) > tolerance)
            return false;

        // Check IBL usage
        if (mat1->EnableIBL != mat2->EnableIBL)
            return false;

        return true;
    }

    u32 MaterialBatcher::CreateBatch(u32 startIndex)
    {
        if (startIndex >= m_PendingMaterials.size())
            return 0;

        BatchInfo batch;
        batch.Shader = m_PendingMaterials[startIndex].OptimalShader;
        batch.Materials.push_back(m_PendingMaterials[startIndex].Material);
        batch.MaterialCount = 1;

        // Collect unique textures from the first material
        auto& firstMaterial = m_PendingMaterials[startIndex].Material;
        if (firstMaterial->AlbedoMap)
            batch.Textures.push_back(firstMaterial->AlbedoMap);
        if (firstMaterial->MetallicRoughnessMap)
            batch.Textures.push_back(firstMaterial->MetallicRoughnessMap);
        if (firstMaterial->NormalMap)
            batch.Textures.push_back(firstMaterial->NormalMap);
        if (firstMaterial->AOMap)
            batch.Textures.push_back(firstMaterial->AOMap);
        if (firstMaterial->EmissiveMap)
            batch.Textures.push_back(firstMaterial->EmissiveMap);

        u32 materialsProcessed = 1;

        // Try to add more compatible materials to this batch
        for (u32 i = startIndex + 1; i < m_PendingMaterials.size(); ++i)
        {
            auto& candidateMaterial = m_PendingMaterials[i].Material;
            
            // Check shader compatibility
            if (m_PendingMaterials[i].OptimalShader != batch.Shader)
                continue;

            // Check if we can fit more textures
            u32 additionalTextures = 0;
            if (candidateMaterial->AlbedoMap && 
                std::find(batch.Textures.begin(), batch.Textures.end(), candidateMaterial->AlbedoMap) == batch.Textures.end())
                additionalTextures++;
            if (candidateMaterial->MetallicRoughnessMap && 
                std::find(batch.Textures.begin(), batch.Textures.end(), candidateMaterial->MetallicRoughnessMap) == batch.Textures.end())
                additionalTextures++;
            if (candidateMaterial->NormalMap && 
                std::find(batch.Textures.begin(), batch.Textures.end(), candidateMaterial->NormalMap) == batch.Textures.end())
                additionalTextures++;
            if (candidateMaterial->AOMap && 
                std::find(batch.Textures.begin(), batch.Textures.end(), candidateMaterial->AOMap) == batch.Textures.end())
                additionalTextures++;
            if (candidateMaterial->EmissiveMap && 
                std::find(batch.Textures.begin(), batch.Textures.end(), candidateMaterial->EmissiveMap) == batch.Textures.end())
                additionalTextures++;

            // Check if we exceed texture limit
            if (batch.Textures.size() + additionalTextures > m_MaxTexturesPerBatch)
                continue;

            // Check material compatibility
            if (!AreCompatible(firstMaterial, candidateMaterial))
                continue;

            // Add material to batch
            batch.Materials.push_back(candidateMaterial);
            batch.MaterialCount++;

            // Add new textures to batch
            if (candidateMaterial->AlbedoMap && 
                std::find(batch.Textures.begin(), batch.Textures.end(), candidateMaterial->AlbedoMap) == batch.Textures.end())
                batch.Textures.push_back(candidateMaterial->AlbedoMap);
            if (candidateMaterial->MetallicRoughnessMap && 
                std::find(batch.Textures.begin(), batch.Textures.end(), candidateMaterial->MetallicRoughnessMap) == batch.Textures.end())
                batch.Textures.push_back(candidateMaterial->MetallicRoughnessMap);
            if (candidateMaterial->NormalMap && 
                std::find(batch.Textures.begin(), batch.Textures.end(), candidateMaterial->NormalMap) == batch.Textures.end())
                batch.Textures.push_back(candidateMaterial->NormalMap);
            if (candidateMaterial->AOMap && 
                std::find(batch.Textures.begin(), batch.Textures.end(), candidateMaterial->AOMap) == batch.Textures.end())
                batch.Textures.push_back(candidateMaterial->AOMap);
            if (candidateMaterial->EmissiveMap && 
                std::find(batch.Textures.begin(), batch.Textures.end(), candidateMaterial->EmissiveMap) == batch.Textures.end())
                batch.Textures.push_back(candidateMaterial->EmissiveMap);

            materialsProcessed++;

            // Remove processed material from pending list
            m_PendingMaterials.erase(m_PendingMaterials.begin() + i);
            i--; // Adjust index after removal
        }

        m_Batches.push_back(batch);
        return materialsProcessed;
    }

    u32 MaterialBatcher::CalculateCompatibilityScore(const Ref<Material>& mat1, const Ref<Material>& mat2)
    {
        u32 score = 0;

        // Same shader = high score
        if (mat1->GetShader() == mat2->GetShader())
            score += 100;

        // Similar metallic factor
        float metallicDiff = std::abs(mat1->MetallicFactor - mat2->MetallicFactor);
        if (metallicDiff < 0.1f)
            score += 20;
        else if (metallicDiff < 0.3f)
            score += 10;

        // Similar roughness factor
        float roughnessDiff = std::abs(mat1->RoughnessFactor - mat2->RoughnessFactor);
        if (roughnessDiff < 0.1f)
            score += 20;
        else if (roughnessDiff < 0.3f)
            score += 10;

        // Same IBL usage
        if (mat1->EnableIBL == mat2->EnableIBL)
            score += 15;

        // Shared textures
        u32 sharedTextures = 0;
        if (mat1->AlbedoMap == mat2->AlbedoMap && mat1->AlbedoMap != nullptr)
            sharedTextures++;
        if (mat1->MetallicRoughnessMap == mat2->MetallicRoughnessMap && mat1->MetallicRoughnessMap != nullptr)
            sharedTextures++;
        if (mat1->NormalMap == mat2->NormalMap && mat1->NormalMap != nullptr)
            sharedTextures++;
        if (mat1->AOMap == mat2->AOMap && mat1->AOMap != nullptr)
            sharedTextures++;
        if (mat1->EmissiveMap == mat2->EmissiveMap && mat1->EmissiveMap != nullptr)
            sharedTextures++;

        score += sharedTextures * 10;

        return score;
    }

    void MaterialBatcher::SortMaterialsForBatching()
    {
        // Sort by shader first, then by compatibility
        std::sort(m_PendingMaterials.begin(), m_PendingMaterials.end(),
            [this](const MaterialEntry& a, const MaterialEntry& b) -> bool
            {
                // Primary sort: shader ID
                if (a.OptimalShader->GetRendererID() != b.OptimalShader->GetRendererID())
                    return a.OptimalShader->GetRendererID() < b.OptimalShader->GetRendererID();

                // Secondary sort: material key (groups similar materials)
                if (a.MaterialKey != b.MaterialKey)
                    return a.MaterialKey < b.MaterialKey;

                // Tertiary sort: light count (prefer consistent lighting)
                if (a.LightCount != b.LightCount)
                    return a.LightCount < b.LightCount;

                // Final sort: skinned mesh status
                return a.IsSkinnedMesh < b.IsSkinnedMesh;
            });
    }

    void MaterialBatcher::UpdateStats()
    {
        m_Stats.BatchCount = static_cast<u32>(m_Batches.size());
        
        // Calculate texture switches (assuming each batch requires texture binding)
        m_Stats.TextureSwitches = 0;
        for (const auto& batch : m_Batches)
        {
            m_Stats.TextureSwitches += static_cast<u32>(batch.Textures.size());
        }

        // Calculate shader switches
        std::unordered_set<u32> uniqueShaders;
        for (const auto& batch : m_Batches)
        {
            if (batch.Shader)
                uniqueShaders.insert(batch.Shader->GetRendererID());
        }
        m_Stats.ShaderSwitches = static_cast<u32>(uniqueShaders.size());

        // Calculate batching efficiency
        if (m_Stats.TotalMaterials > 0)
        {
            u32 batchedMaterials = 0;
            for (const auto& batch : m_Batches)
            {
                batchedMaterials += batch.MaterialCount;
            }
            m_Stats.BatchingEfficiency = (static_cast<float>(batchedMaterials) / static_cast<float>(m_Stats.TotalMaterials)) * 100.0f;
        }
        else
        {
            m_Stats.BatchingEfficiency = 0.0f;
        }

        // Log batching results
        OLO_CORE_INFO("MaterialBatcher: Processed {} materials into {} batches", 
                      m_Stats.TotalMaterials, m_Stats.BatchCount);
        OLO_CORE_INFO("MaterialBatcher: Efficiency: {:.1f}%, Texture switches: {}, Shader switches: {}",
                      m_Stats.BatchingEfficiency, m_Stats.TextureSwitches, m_Stats.ShaderSwitches);
    }
}
