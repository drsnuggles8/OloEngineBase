#include "OloEnginePCH.h"
#include "OpenGLDescriptorSetManager.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"

#include <imgui.h>
#include <algorithm>
#include <sstream>

namespace OloEngine
{
    OpenGLDescriptorSetManager::OpenGLDescriptorSetManager()
    {
        // Initialize with reasonable defaults
        m_GlobalRanges.UniformBufferStart = 0;
        m_GlobalRanges.StorageBufferStart = 32;  // Assume 32 UBO slots, start SSBOs after
        m_GlobalRanges.TextureStart = 0;
        m_GlobalRanges.ImageStart = 32;          // Assume 32 texture slots, start images after

        m_StateCache.Invalidate();
        m_Statistics.Reset();
        
        OLO_CORE_TRACE("OpenGLDescriptorSetManager initialized");
    }

    void OpenGLDescriptorSetManager::CreateSetLayout(u32 setIndex, const std::string& name, const DescriptorSetLayout& layout)
    {
        DescriptorSet descriptorSet;
        descriptorSet.Layout = layout;
        descriptorSet.Layout.SetIndex = setIndex;
        descriptorSet.Layout.Name = name;
        
        // Calculate global binding ranges based on set index and configuration
        u32 uniformBufferRange = descriptorSet.Layout.MaxUniformBuffers;
        u32 storageBufferRange = descriptorSet.Layout.MaxStorageBuffers;
        u32 textureRange = descriptorSet.Layout.MaxTextures;
        u32 imageRange = descriptorSet.Layout.MaxImages;
        
        descriptorSet.Layout.UniformBufferBaseBinding = m_GlobalRanges.UniformBufferStart + (setIndex * uniformBufferRange);
        descriptorSet.Layout.StorageBufferBaseBinding = m_GlobalRanges.StorageBufferStart + (setIndex * storageBufferRange);
        descriptorSet.Layout.TextureBaseBinding = m_GlobalRanges.TextureStart + (setIndex * textureRange);
        descriptorSet.Layout.ImageBaseBinding = m_GlobalRanges.ImageStart + (setIndex * imageRange);
        
        // Pre-allocate resource vectors
        descriptorSet.UniformBufferIDs.resize(uniformBufferRange, 0);
        descriptorSet.StorageBufferIDs.resize(storageBufferRange, 0);
        descriptorSet.TextureIDs.resize(textureRange, 0);
        descriptorSet.TextureTargets.resize(textureRange, 0);
        descriptorSet.ImageIDs.resize(imageRange, 0);
        
        m_DescriptorSets[setIndex] = std::move(descriptorSet);
        m_BindingOrderDirty = true;
        
        OLO_CORE_INFO("Created descriptor set {0} '{1}' with ranges: UBO={2}-{3}, SSBO={4}-{5}, TEX={6}-{7}, IMG={8}-{9}",
                     setIndex, name,
                     descriptorSet.Layout.UniformBufferBaseBinding, 
                     descriptorSet.Layout.UniformBufferBaseBinding + uniformBufferRange - 1,
                     descriptorSet.Layout.StorageBufferBaseBinding,
                     descriptorSet.Layout.StorageBufferBaseBinding + storageBufferRange - 1,
                     descriptorSet.Layout.TextureBaseBinding,
                     descriptorSet.Layout.TextureBaseBinding + textureRange - 1,
                     descriptorSet.Layout.ImageBaseBinding,
                     descriptorSet.Layout.ImageBaseBinding + imageRange - 1);
    }

    void OpenGLDescriptorSetManager::RemoveSetLayout(u32 setIndex)
    {
        auto it = m_DescriptorSets.find(setIndex);
        if (it != m_DescriptorSets.end())
        {
            OLO_CORE_INFO("Removed descriptor set {0} '{1}'", setIndex, it->second.Layout.Name);
            m_DescriptorSets.erase(it);
            m_BindingOrderDirty = true;
        }
    }

    const OpenGLDescriptorSetManager::DescriptorSetLayout* OpenGLDescriptorSetManager::GetSetLayout(u32 setIndex) const
    {
        auto it = m_DescriptorSets.find(setIndex);
        return it != m_DescriptorSets.end() ? &it->second.Layout : nullptr;
    }

    void OpenGLDescriptorSetManager::ConfigureAutomaticBindingRanges(u32 totalUniformBuffers, u32 totalStorageBuffers, 
                                                                    u32 totalTextures, u32 totalImages, u32 setCount)
    {
        // Calculate optimal binding ranges
        u32 uniformBuffersPerSet = (totalUniformBuffers + setCount - 1) / setCount; // Round up
        u32 storageBuffersPerSet = (totalStorageBuffers + setCount - 1) / setCount;
        u32 texturesPerSet = (totalTextures + setCount - 1) / setCount;
        u32 imagesPerSet = (totalImages + setCount - 1) / setCount;

        // Ensure minimum reasonable sizes
        uniformBuffersPerSet = std::max(uniformBuffersPerSet, 4u);
        storageBuffersPerSet = std::max(storageBuffersPerSet, 4u);
        texturesPerSet = std::max(texturesPerSet, 8u);
        imagesPerSet = std::max(imagesPerSet, 4u);

        // Update global ranges
        m_GlobalRanges.UniformBufferStart = 0;
        m_GlobalRanges.StorageBufferStart = uniformBuffersPerSet * setCount;
        m_GlobalRanges.TextureStart = 0;
        m_GlobalRanges.ImageStart = texturesPerSet * setCount;

        OLO_CORE_INFO("Configured automatic binding ranges for {0} sets:", setCount);
        OLO_CORE_INFO("  UBO: {0} per set, SSBO: {1} per set", uniformBuffersPerSet, storageBuffersPerSet);
        OLO_CORE_INFO("  TEX: {0} per set, IMG: {1} per set", texturesPerSet, imagesPerSet);
        OLO_CORE_INFO("  Global ranges - UBO: {0}, SSBO: {1}, TEX: {2}, IMG: {3}",
                     m_GlobalRanges.UniformBufferStart, m_GlobalRanges.StorageBufferStart,
                     m_GlobalRanges.TextureStart, m_GlobalRanges.ImageStart);
    }

    void OpenGLDescriptorSetManager::BindResource(u32 setIndex, const std::string& resourceName, 
                                                 ShaderResourceType resourceType, u32 localBinding, 
                                                 u32 resourceID, GLenum target, u32 arraySize)
    {
        auto it = m_DescriptorSets.find(setIndex);
        if (it == m_DescriptorSets.end())
        {
            OLO_CORE_ERROR("Descriptor set {0} not found", setIndex);
            return;
        }

        DescriptorSet& descriptorSet = it->second;

        if (!ValidateBinding(setIndex, resourceName, resourceType, localBinding))
        {
            OLO_CORE_ERROR("Invalid binding for resource '{0}' in set {1}", resourceName, setIndex);
            return;
        }

        // Calculate global binding
        u32 globalBinding = CalculateGlobalBinding(descriptorSet.Layout, resourceType, localBinding);

        // Create or update resource binding
        ResourceBinding binding(resourceName, resourceType, localBinding);
        binding.GlobalBinding = globalBinding;
        binding.ArraySize = arraySize;
        binding.IsArray = arraySize > 1;
        binding.IsBound = true;
        binding.BoundResourceID = resourceID;
        binding.BoundTarget = target;

        descriptorSet.Bindings[resourceName] = binding;

        // Store resource in appropriate vector
        switch (resourceType)
        {
            case ShaderResourceType::UniformBuffer:
                if (localBinding < descriptorSet.UniformBufferIDs.size())
                    descriptorSet.UniformBufferIDs[localBinding] = resourceID;
                break;
                
            case ShaderResourceType::StorageBuffer:
                if (localBinding < descriptorSet.StorageBufferIDs.size())
                    descriptorSet.StorageBufferIDs[localBinding] = resourceID;
                break;
                
            case ShaderResourceType::Texture2D:
            case ShaderResourceType::TextureCube:
                if (localBinding < descriptorSet.TextureIDs.size())
                {
                    descriptorSet.TextureIDs[localBinding] = resourceID;
                    descriptorSet.TextureTargets[localBinding] = target;
                }
                break;
                
            case ShaderResourceType::Image2D:
                if (localBinding < descriptorSet.ImageIDs.size())
                    descriptorSet.ImageIDs[localBinding] = resourceID;
                break;
                
            default:
                OLO_CORE_WARN("Unsupported resource type for binding: {0}", static_cast<u32>(resourceType));
                break;
        }

        descriptorSet.MarkDirty();
        
        OLO_CORE_TRACE("Bound resource '{0}' (ID: {1}) to set {2}, local binding {3}, global binding {4}",
                      resourceName, resourceID, setIndex, localBinding, globalBinding);
    }

    void OpenGLDescriptorSetManager::UnbindResource(u32 setIndex, const std::string& resourceName)
    {
        auto it = m_DescriptorSets.find(setIndex);
        if (it == m_DescriptorSets.end())
            return;

        DescriptorSet& descriptorSet = it->second;
        auto bindingIt = descriptorSet.Bindings.find(resourceName);
        if (bindingIt == descriptorSet.Bindings.end())
            return;

        const ResourceBinding& binding = bindingIt->second;
        u32 localBinding = binding.LocalBinding;

        // Clear resource from appropriate vector
        switch (binding.Type)
        {
            case ShaderResourceType::UniformBuffer:
                if (localBinding < descriptorSet.UniformBufferIDs.size())
                    descriptorSet.UniformBufferIDs[localBinding] = 0;
                break;
                
            case ShaderResourceType::StorageBuffer:
                if (localBinding < descriptorSet.StorageBufferIDs.size())
                    descriptorSet.StorageBufferIDs[localBinding] = 0;
                break;
                
            case ShaderResourceType::Texture2D:
            case ShaderResourceType::TextureCube:
                if (localBinding < descriptorSet.TextureIDs.size())
                {
                    descriptorSet.TextureIDs[localBinding] = 0;
                    descriptorSet.TextureTargets[localBinding] = 0;
                }
                break;
                
            case ShaderResourceType::Image2D:
                if (localBinding < descriptorSet.ImageIDs.size())
                    descriptorSet.ImageIDs[localBinding] = 0;
                break;
        }

        descriptorSet.Bindings.erase(bindingIt);
        descriptorSet.MarkDirty();
        
        OLO_CORE_TRACE("Unbound resource '{0}' from set {1}", resourceName, setIndex);
    }

    void OpenGLDescriptorSetManager::BindDescriptorSet(u32 setIndex, bool forceRebind)
    {
        RENDERER_PROFILE_SCOPE("OpenGLDescriptorSetManager::BindDescriptorSet");

        auto it = m_DescriptorSets.find(setIndex);
        if (it == m_DescriptorSets.end())
        {
            OLO_CORE_WARN("Attempted to bind non-existent descriptor set {0}", setIndex);
            return;
        }

        DescriptorSet& descriptorSet = it->second;
        
        if (!descriptorSet.Layout.IsActive)
        {
            OLO_CORE_TRACE("Skipping inactive descriptor set {0}", setIndex);
            return;
        }

        if (!descriptorSet.IsDirty && !forceRebind)
        {
            m_Statistics.CacheHits++;
            return;
        }

        m_Statistics.CacheMisses++;
        BindDescriptorSetInternal(descriptorSet, forceRebind);
        
        descriptorSet.MarkClean();
        descriptorSet.LastBoundFrame = m_CurrentFrame;
        m_Statistics.SetBindings++;
        m_Statistics.SetUsage[setIndex]++;
        
        UpdateBindingStatistics(descriptorSet);
    }

    void OpenGLDescriptorSetManager::BindDescriptorSets(const std::vector<u32>& setIndices, bool forceRebind)
    {
        for (u32 setIndex : setIndices)
        {
            BindDescriptorSet(setIndex, forceRebind);
        }
    }

    void OpenGLDescriptorSetManager::BindAllSets(bool forceRebind)
    {
        std::vector<u32> bindingOrder = GetBindingOrder();
        BindDescriptorSets(bindingOrder, forceRebind);
    }

    void OpenGLDescriptorSetManager::MarkSetDirty(u32 setIndex)
    {
        auto it = m_DescriptorSets.find(setIndex);
        if (it != m_DescriptorSets.end())
        {
            it->second.MarkDirty();
        }
    }

    void OpenGLDescriptorSetManager::MarkAllSetsDirty()
    {
        for (auto& [setIndex, descriptorSet] : m_DescriptorSets)
        {
            descriptorSet.MarkDirty();
        }
        m_StateCache.Invalidate();
    }

    void OpenGLDescriptorSetManager::ClearDescriptorSet(u32 setIndex)
    {
        auto it = m_DescriptorSets.find(setIndex);
        if (it != m_DescriptorSets.end())
        {
            it->second.Clear();
            OLO_CORE_TRACE("Cleared descriptor set {0}", setIndex);
        }
    }

    void OpenGLDescriptorSetManager::ClearAllSets()
    {
        for (auto& [setIndex, descriptorSet] : m_DescriptorSets)
        {
            descriptorSet.Clear();
        }
        m_StateCache.Invalidate();
        OLO_CORE_TRACE("Cleared all descriptor sets");
    }

    bool OpenGLDescriptorSetManager::HasDescriptorSet(u32 setIndex) const
    {
        return m_DescriptorSets.find(setIndex) != m_DescriptorSets.end();
    }

    bool OpenGLDescriptorSetManager::IsSetDirty(u32 setIndex) const
    {
        auto it = m_DescriptorSets.find(setIndex);
        return it != m_DescriptorSets.end() ? it->second.IsDirty : false;
    }

    std::vector<u32> OpenGLDescriptorSetManager::GetActiveSetIndices() const
    {
        std::vector<u32> indices;
        for (const auto& [setIndex, descriptorSet] : m_DescriptorSets)
        {
            if (descriptorSet.Layout.IsActive)
            {
                indices.push_back(setIndex);
            }
        }
        return indices;
    }

    std::vector<u32> OpenGLDescriptorSetManager::GetBindingOrder() const
    {
        if (!m_BindingOrderDirty && !m_CachedBindingOrder.empty())
            return m_CachedBindingOrder;

        std::vector<std::pair<u32, f32>> setsPriorities;
        for (const auto& [setIndex, descriptorSet] : m_DescriptorSets)
        {
            if (descriptorSet.Layout.IsActive)
            {
                setsPriorities.emplace_back(setIndex, descriptorSet.Layout.Priority);
            }
        }

        // Sort by priority (higher first), then by set index
        std::sort(setsPriorities.begin(), setsPriorities.end(),
                 [](const auto& a, const auto& b) {
                     if (a.second != b.second)
                         return a.second > b.second; // Higher priority first
                     return a.first < b.first;       // Lower index first for same priority
                 });

        m_CachedBindingOrder.clear();
        for (const auto& [setIndex, priority] : setsPriorities)
        {
            m_CachedBindingOrder.push_back(setIndex);
        }

        m_BindingOrderDirty = false;
        return m_CachedBindingOrder;
    }

    void OpenGLDescriptorSetManager::SetStateCachingEnabled(bool enabled)
    {
        m_StateCachingEnabled = enabled;
        if (!enabled)
        {
            m_StateCache.Invalidate();
        }
    }

    void OpenGLDescriptorSetManager::InvalidateCache()
    {
        m_StateCache.Invalidate();
        MarkAllSetsDirty();
    }

    void OpenGLDescriptorSetManager::CreateStandardPBRLayout()
    {
        // Set 0: System (view/projection matrices, time, camera)
        DescriptorSetLayout systemLayout(0, "System");
        systemLayout.Priority = 4.0f; // Highest priority
        systemLayout.MaxUniformBuffers = 4;
        systemLayout.MaxTextures = 4;
        CreateSetLayout(0, "System", systemLayout);

        // Set 1: Global (lighting, environment, shadows)
        DescriptorSetLayout globalLayout(1, "Global");
        globalLayout.Priority = 3.0f;
        globalLayout.MaxUniformBuffers = 8;
        globalLayout.MaxTextures = 16;
        CreateSetLayout(1, "Global", globalLayout);

        // Set 2: Material (diffuse, normal, metallic/roughness, AO)
        DescriptorSetLayout materialLayout(2, "Material");
        materialLayout.Priority = 2.0f;
        materialLayout.MaxUniformBuffers = 4;
        materialLayout.MaxTextures = 16;
        CreateSetLayout(2, "Material", materialLayout);

        // Set 3: Instance (model matrices, instance data)
        DescriptorSetLayout instanceLayout(3, "Instance");
        instanceLayout.Priority = 1.0f; // Lowest priority
        instanceLayout.MaxUniformBuffers = 2;
        instanceLayout.MaxStorageBuffers = 4;
        instanceLayout.MaxTextures = 4;
        CreateSetLayout(3, "Instance", instanceLayout);

        OLO_CORE_INFO("Created standard PBR descriptor set layout");
    }

    void OpenGLDescriptorSetManager::CreateComputeLayout()
    {
        // Set 0: Compute parameters and configuration
        DescriptorSetLayout computeLayout(0, "Compute");
        computeLayout.Priority = 2.0f;
        computeLayout.MaxUniformBuffers = 4;
        computeLayout.MaxStorageBuffers = 16;
        computeLayout.MaxTextures = 8;
        computeLayout.MaxImages = 8;
        CreateSetLayout(0, "Compute", computeLayout);

        OLO_CORE_INFO("Created compute descriptor set layout");
    }

    void OpenGLDescriptorSetManager::CreatePostProcessLayout()
    {
        // Set 0: Post-process parameters and screen textures
        DescriptorSetLayout postProcessLayout(0, "PostProcess");
        postProcessLayout.Priority = 1.0f;
        postProcessLayout.MaxUniformBuffers = 2;
        postProcessLayout.MaxTextures = 8;
        CreateSetLayout(0, "PostProcess", postProcessLayout);

        OLO_CORE_INFO("Created post-process descriptor set layout");
    }

    u32 OpenGLDescriptorSetManager::MapHazelBinding(const std::string& resourceName, u32 set, u32 binding, ShaderResourceType resourceType)
    {
        auto it = m_DescriptorSets.find(set);
        if (it == m_DescriptorSets.end())
        {
            OLO_CORE_ERROR("Cannot map Hazel binding: descriptor set {0} not found", set);
            return UINT32_MAX;
        }

        return CalculateGlobalBinding(it->second.Layout, resourceType, binding);
    }

    void OpenGLDescriptorSetManager::BindDescriptorSetInternal(DescriptorSet& descriptorSet, bool forceRebind)
    {
        // Bind different resource types
        BindUniformBuffers(descriptorSet);
        BindStorageBuffers(descriptorSet);
        BindTextures(descriptorSet);
        BindImages(descriptorSet);
    }

    void OpenGLDescriptorSetManager::BindUniformBuffers(const DescriptorSet& descriptorSet)
    {
        const auto& layout = descriptorSet.Layout;
        
        if (m_MultiBind)
        {
            // Use multi-bind for efficiency
            for (u32 i = 0; i < descriptorSet.UniformBufferIDs.size(); ++i)
            {
                u32 bufferID = descriptorSet.UniformBufferIDs[i];
                if (bufferID != 0)
                {
                    u32 globalBinding = layout.UniformBufferBaseBinding + i;
                    m_MultiBind->AddBuffer(bufferID, globalBinding, GL_UNIFORM_BUFFER, 0, 0, ShaderResourceType::UniformBuffer);
                }
            }
        }
        else
        {
            // Direct binding
            for (u32 i = 0; i < descriptorSet.UniformBufferIDs.size(); ++i)
            {
                u32 bufferID = descriptorSet.UniformBufferIDs[i];
                if (bufferID != 0)
                {
                    u32 globalBinding = layout.UniformBufferBaseBinding + i;
                    
                    // Check cache
                    if (m_StateCachingEnabled && IsResourceCached(globalBinding, bufferID, GL_UNIFORM_BUFFER))
                    {
                        m_Statistics.RedundantBindingsPrevented++;
                        continue;
                    }
                    
                    glBindBufferBase(GL_UNIFORM_BUFFER, globalBinding, bufferID);
                    UpdateResourceCache(globalBinding, bufferID, GL_UNIFORM_BUFFER);
                    m_Statistics.IndividualBindings++;
                }
            }
        }
    }

    void OpenGLDescriptorSetManager::BindStorageBuffers(const DescriptorSet& descriptorSet)
    {
        const auto& layout = descriptorSet.Layout;
        
        if (m_MultiBind)
        {
            // Use multi-bind for efficiency
            for (u32 i = 0; i < descriptorSet.StorageBufferIDs.size(); ++i)
            {
                u32 bufferID = descriptorSet.StorageBufferIDs[i];
                if (bufferID != 0)
                {
                    u32 globalBinding = layout.StorageBufferBaseBinding + i;
                    m_MultiBind->AddBuffer(bufferID, globalBinding, GL_SHADER_STORAGE_BUFFER, 0, 0, ShaderResourceType::StorageBuffer);
                }
            }
        }
        else
        {
            // Direct binding
            for (u32 i = 0; i < descriptorSet.StorageBufferIDs.size(); ++i)
            {
                u32 bufferID = descriptorSet.StorageBufferIDs[i];
                if (bufferID != 0)
                {
                    u32 globalBinding = layout.StorageBufferBaseBinding + i;
                    
                    // Check cache
                    if (m_StateCachingEnabled && IsResourceCached(globalBinding, bufferID, GL_SHADER_STORAGE_BUFFER))
                    {
                        m_Statistics.RedundantBindingsPrevented++;
                        continue;
                    }
                    
                    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, globalBinding, bufferID);
                    UpdateResourceCache(globalBinding, bufferID, GL_SHADER_STORAGE_BUFFER);
                    m_Statistics.IndividualBindings++;
                }
            }
        }
    }

    void OpenGLDescriptorSetManager::BindTextures(const DescriptorSet& descriptorSet)
    {
        const auto& layout = descriptorSet.Layout;
        
        if (m_MultiBind)
        {
            // Use multi-bind for efficiency
            for (u32 i = 0; i < descriptorSet.TextureIDs.size(); ++i)
            {
                u32 textureID = descriptorSet.TextureIDs[i];
                GLenum target = descriptorSet.TextureTargets[i];
                if (textureID != 0)
                {
                    u32 globalBinding = layout.TextureBaseBinding + i;
                    ShaderResourceType resourceType = (target == GL_TEXTURE_CUBE_MAP) ? 
                        ShaderResourceType::TextureCube : ShaderResourceType::Texture2D;
                    m_MultiBind->AddTexture(textureID, globalBinding, target, resourceType);
                }
            }
        }
        else
        {
            // Direct binding
            for (u32 i = 0; i < descriptorSet.TextureIDs.size(); ++i)
            {
                u32 textureID = descriptorSet.TextureIDs[i];
                GLenum target = descriptorSet.TextureTargets[i];
                if (textureID != 0)
                {
                    u32 globalBinding = layout.TextureBaseBinding + i;
                    
                    // Check cache
                    if (m_StateCachingEnabled && IsResourceCached(globalBinding, textureID, target))
                    {
                        m_Statistics.RedundantBindingsPrevented++;
                        continue;
                    }
                    
                    glActiveTexture(GL_TEXTURE0 + globalBinding);
                    glBindTexture(target, textureID);
                    UpdateResourceCache(globalBinding, textureID, target);
                    m_Statistics.IndividualBindings++;
                }
            }
        }
    }

    void OpenGLDescriptorSetManager::BindImages(const DescriptorSet& descriptorSet)
    {
        const auto& layout = descriptorSet.Layout;
        
        // Images don't have multi-bind support, always use direct binding
        for (u32 i = 0; i < descriptorSet.ImageIDs.size(); ++i)
        {
            u32 imageID = descriptorSet.ImageIDs[i];
            if (imageID != 0)
            {
                u32 globalBinding = layout.ImageBaseBinding + i;
                
                // Check cache
                if (m_StateCachingEnabled && IsResourceCached(globalBinding, imageID, GL_TEXTURE_2D))
                {
                    m_Statistics.RedundantBindingsPrevented++;
                    continue;
                }
                
                // Bind as image (for compute shaders)
                glBindImageTexture(globalBinding, imageID, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8);
                UpdateResourceCache(globalBinding, imageID, GL_TEXTURE_2D);
                m_Statistics.IndividualBindings++;
            }
        }
    }

    u32 OpenGLDescriptorSetManager::CalculateGlobalBinding(const DescriptorSetLayout& layout, ShaderResourceType type, u32 localBinding) const
    {
        switch (type)
        {
            case ShaderResourceType::UniformBuffer:
                return layout.UniformBufferBaseBinding + localBinding;
            case ShaderResourceType::StorageBuffer:
                return layout.StorageBufferBaseBinding + localBinding;
            case ShaderResourceType::Texture2D:
            case ShaderResourceType::TextureCube:
                return layout.TextureBaseBinding + localBinding;
            case ShaderResourceType::Image2D:
                return layout.ImageBaseBinding + localBinding;
            default:
                OLO_CORE_ERROR("Unsupported resource type for global binding calculation");
                return UINT32_MAX;
        }
    }

    void OpenGLDescriptorSetManager::UpdateBindingStatistics(const DescriptorSet& descriptorSet)
    {
        u32 bindingsInSet = 0;
        for (const auto& [name, binding] : descriptorSet.Bindings)
        {
            if (binding.IsBound)
                bindingsInSet++;
        }
        
        m_Statistics.TotalBindings += bindingsInSet;
        
        // Update average bindings per set
        f32 totalSets = static_cast<f32>(m_Statistics.SetBindings);
        m_Statistics.AverageBindingsPerSet = totalSets > 0 ? 
            static_cast<f32>(m_Statistics.TotalBindings) / totalSets : 0.0f;
    }

    bool OpenGLDescriptorSetManager::ValidateBinding(u32 setIndex, const std::string& resourceName, 
                                                   ShaderResourceType resourceType, u32 localBinding) const
    {
        auto it = m_DescriptorSets.find(setIndex);
        if (it == m_DescriptorSets.end())
            return false;

        const DescriptorSetLayout& layout = it->second.Layout;

        switch (resourceType)
        {
            case ShaderResourceType::UniformBuffer:
                return localBinding < layout.MaxUniformBuffers;
            case ShaderResourceType::StorageBuffer:
                return localBinding < layout.MaxStorageBuffers;
            case ShaderResourceType::Texture2D:
            case ShaderResourceType::TextureCube:
                return localBinding < layout.MaxTextures;
            case ShaderResourceType::Image2D:
                return localBinding < layout.MaxImages;
            default:
                return false;
        }
    }

    bool OpenGLDescriptorSetManager::IsResourceCached(u32 globalBinding, u32 resourceID, GLenum target) const
    {
        if (!m_StateCachingEnabled || !m_StateCache.IsValid)
            return false;

        auto it = m_StateCache.BoundResources.find(globalBinding);
        return it != m_StateCache.BoundResources.end() && 
               it->second.first == resourceID && it->second.second == target;
    }

    void OpenGLDescriptorSetManager::UpdateResourceCache(u32 globalBinding, u32 resourceID, GLenum target)
    {
        if (m_StateCachingEnabled)
        {
            m_StateCache.BoundResources[globalBinding] = {resourceID, target};
            m_StateCache.IsValid = true;
        }
    }

    std::string OpenGLDescriptorSetManager::GeneratePerformanceReport() const
    {
        std::ostringstream report;
        report << "OpenGL Descriptor Set Manager Performance Report\n";
        report << "================================================\n";
        report << "Total Bindings: " << m_Statistics.TotalBindings << "\n";
        report << "Set Bindings: " << m_Statistics.SetBindings << "\n";
        report << "Individual Bindings: " << m_Statistics.IndividualBindings << "\n";
        report << "Cache Hit Ratio: " << std::fixed << std::setprecision(2) << (m_Statistics.GetCacheHitRatio() * 100.0f) << "%\n";
        report << "Redundant Bindings Prevented: " << m_Statistics.RedundantBindingsPrevented << "\n";
        report << "Average Bindings Per Set: " << std::fixed << std::setprecision(1) << m_Statistics.AverageBindingsPerSet << "\n";
        report << "\nSet Usage:\n";
        for (const auto& [setIndex, usage] : m_Statistics.SetUsage)
        {
            report << "  Set " << setIndex << ": " << usage << " bindings\n";
        }
        return report.str();
    }
}
