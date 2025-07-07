#include "OloEnginePCH.h"
#include "ShaderTemplateManager.h"
#include "OloEngine/Core/Log.h"

#include <imgui.h>
#include <algorithm>
#include <sstream>
#include <fstream>

namespace OloEngine
{
    ShaderTemplateManager::ShaderTemplateManager()
    {
        // Initialize pattern keywords for detection
        m_PatternKeywords["PBR"] = {
            "albedo", "diffuse", "basecolor", "metallic", "roughness", "normal", "ao", "occlusion",
            "emissive", "emission", "material", "pbr", "brdf", "ibl", "environment"
        };
        
        m_PatternKeywords["PostProcess"] = {
            "scene", "screen", "fullscreen", "quad", "bloom", "tonemap", "gamma", "exposure",
            "colorgrade", "blur", "downsample", "upsample", "filter", "kernel"
        };
        
        m_PatternKeywords["Compute"] = {
            "compute", "dispatch", "workgroup", "local", "shared", "buffer", "image", "atomic"
        };
        
        m_PatternKeywords["Skybox"] = {
            "skybox", "cubemap", "environment", "hdri", "equirectangular", "sky", "atmosphere"
        };
        
        m_PatternKeywords["Shadow"] = {
            "shadow", "depth", "cascade", "bias", "pcf", "vsm", "esm", "light", "matrix"
        };
        
        m_PatternKeywords["Instanced"] = {
            "instance", "instanced", "transform", "matrix", "array", "batch", "draw", "indirect"
        };

        // Set pattern weights (higher = more important for detection)
        m_PatternWeights["PBR"] = 1.0f;
        m_PatternWeights["PostProcess"] = 0.8f;
        m_PatternWeights["Compute"] = 0.9f;
        m_PatternWeights["Skybox"] = 0.7f;
        m_PatternWeights["Shadow"] = 0.8f;
        m_PatternWeights["Instanced"] = 0.6f;
    }

    void ShaderTemplateManager::RegisterTemplate(const TemplateInfo& templateInfo, 
                                                const UniformBufferRegistry* registryTemplate)
    {
        m_Templates[templateInfo.Name] = templateInfo;
        
        if (registryTemplate)
        {
            // Create a copy of the registry template
            m_RegistryTemplates[templateInfo.Name] = std::make_unique<UniformBufferRegistry>(*registryTemplate);
        }
        
        m_Statistics.TotalTemplates++;
        m_Statistics.CategoryUsage[templateInfo.Category]++;
        
        OLO_CORE_INFO("Registered shader template: {0} (category: {1})", 
                     templateInfo.Name, templateInfo.Category);
    }

    void ShaderTemplateManager::RemoveTemplate(const std::string& templateName)
    {
        auto it = m_Templates.find(templateName);
        if (it != m_Templates.end())
        {
            m_Statistics.CategoryUsage[it->second.Category]--;
            m_Templates.erase(it);
            m_RegistryTemplates.erase(templateName);
            m_Statistics.TotalTemplates--;
            
            OLO_CORE_INFO("Removed shader template: {0}", templateName);
        }
    }

    const ShaderTemplateManager::TemplateInfo* ShaderTemplateManager::GetTemplateInfo(const std::string& templateName) const
    {
        auto it = m_Templates.find(templateName);
        return it != m_Templates.end() ? &it->second : nullptr;
    }

    std::vector<std::string> ShaderTemplateManager::GetTemplatesByCategory(const std::string& category) const
    {
        std::vector<std::string> result;
        for (const auto& [name, info] : m_Templates)
        {
            if (info.Category == category)
            {
                result.push_back(name);
            }
        }
        return result;
    }

    ShaderTemplateManager::PatternDetectionResult ShaderTemplateManager::DetectPatternAndSuggestTemplates(
        const UniformBufferRegistry& registry) const
    {
        PatternDetectionResult result;
        m_Statistics.PatternDetectionsPerformed++;

        const auto& bindings = registry.GetBindings();
        
        // Analyze patterns
        for (const auto& [pattern, keywords] : m_PatternKeywords)
        {
            f32 confidence = CalculatePatternConfidence(pattern, bindings);
            result.PatternConfidence[pattern] = confidence;
        }

        // Find the pattern with highest confidence
        f32 maxConfidence = 0.0f;
        for (const auto& [pattern, confidence] : result.PatternConfidence)
        {
            if (confidence > maxConfidence)
            {
                maxConfidence = confidence;
                result.DetectedPattern = pattern;
            }
        }

        result.HasHighConfidenceMatch = maxConfidence >= 0.8f;

        // Generate template suggestions
        for (const auto& [templateName, templateInfo] : m_Templates)
        {
            TemplateMatch match = MatchAgainstTemplate(registry, templateName);
            if (match.IsViableMatch())
            {
                result.Suggestions.push_back(match);
            }
        }

        // Sort suggestions by match score
        std::sort(result.Suggestions.begin(), result.Suggestions.end(),
                 [](const TemplateMatch& a, const TemplateMatch& b) {
                     return a.MatchScore > b.MatchScore;
                 });

        // Set auto-selected template
        if (!result.Suggestions.empty() && result.Suggestions[0].IsGoodMatch())
        {
            result.AutoSelectedTemplate = result.Suggestions[0].TemplateName;
        }

        return result;
    }

    ShaderTemplateManager::TemplateMatch ShaderTemplateManager::MatchAgainstTemplate(
        const UniformBufferRegistry& registry, const std::string& templateName) const
    {
        TemplateMatch match;
        match.TemplateName = templateName;

        const auto* templateInfo = GetTemplateInfo(templateName);
        if (!templateInfo)
        {
            match.Reasoning = "Template not found";
            return match;
        }

        const auto& bindings = registry.GetBindings();
        
        // Extract available resources
        std::vector<std::string> availableUniforms = ExtractResourceNames(bindings, ShaderResourceType::UniformBuffer);
        std::vector<std::string> availableTextures;
        for (const auto& type : {ShaderResourceType::Texture2D, ShaderResourceType::TextureCube})
        {
            auto typeResources = ExtractResourceNames(bindings, type);
            availableTextures.insert(availableTextures.end(), typeResources.begin(), typeResources.end());
        }
        std::vector<std::string> availableBuffers = ExtractResourceNames(bindings, ShaderResourceType::StorageBuffer);

        // Calculate match scores
        f32 uniformMatch = CalculateResourceMatch(templateInfo->RequiredUniforms, availableUniforms);
        f32 textureMatch = CalculateResourceMatch(templateInfo->RequiredTextures, availableTextures);
        f32 bufferMatch = CalculateResourceMatch(templateInfo->RequiredBuffers, availableBuffers);

        // Weight the matches (uniforms are most important, then textures, then buffers)
        match.MatchScore = (uniformMatch * 0.5f + textureMatch * 0.3f + bufferMatch * 0.2f);

        // Adjust for template priority
        match.MatchScore *= templateInfo->Priority;

        // Find missing and extra resources
        for (const std::string& required : templateInfo->RequiredUniforms)
        {
            if (std::find(availableUniforms.begin(), availableUniforms.end(), required) == availableUniforms.end())
            {
                match.MissingResources.push_back(required);
            }
        }

        // Generate reasoning
        std::ostringstream reasoning;
        reasoning << "Match: " << std::fixed << std::setprecision(1) << (match.MatchScore * 100.0f) << "% ";
        reasoning << "(Uniforms: " << (uniformMatch * 100.0f) << "%, ";
        reasoning << "Textures: " << (textureMatch * 100.0f) << "%, ";
        reasoning << "Buffers: " << (bufferMatch * 100.0f) << "%)";
        
        if (!match.MissingResources.empty())
        {
            reasoning << ". Missing: " << match.MissingResources.size() << " resources";
        }
        
        match.Reasoning = reasoning.str();

        return match;
    }

    ShaderTemplateManager::TemplateMatch ShaderTemplateManager::FindBestTemplate(
        const UniformBufferRegistry& registry, f32 minScore) const
    {
        TemplateMatch bestMatch;
        bestMatch.MatchScore = 0.0f;

        for (const auto& [templateName, templateInfo] : m_Templates)
        {
            TemplateMatch match = MatchAgainstTemplate(registry, templateName);
            if (match.MatchScore > bestMatch.MatchScore && match.MatchScore >= minScore)
            {
                bestMatch = match;
            }
        }

        if (bestMatch.MatchScore >= minScore)
        {
            m_Statistics.SuccessfulMatches++;
            m_Statistics.TemplateUsage[bestMatch.TemplateName]++;
        }

        return bestMatch;
    }

    bool ShaderTemplateManager::ApplyTemplate(UniformBufferRegistry& registry, const std::string& templateName) const
    {
        const auto* templateInfo = GetTemplateInfo(templateName);
        if (!templateInfo)
        {
            OLO_CORE_ERROR("Template '{0}' not found", templateName);
            return false;
        }

        // Update registry specification
        registry.UpdateSpecification(templateInfo->DefaultSpec, false);

        // Add default resources
        for (const auto& [resourceName, resourceInfo] : templateInfo->DefaultResources)
        {
            registry.AddDefaultResource(resourceName, resourceInfo);
        }

        m_Statistics.TemplatesUsed++;
        m_Statistics.TemplateUsage[templateName]++;

        OLO_CORE_INFO("Applied template '{0}' to registry", templateName);
        return true;
    }

    Scope<UniformBufferRegistry> ShaderTemplateManager::CreateFromTemplate(
        const std::string& templateName, const Ref<Shader>& shader, const std::string& instanceName) const
    {
        auto templateIt = m_RegistryTemplates.find(templateName);
        if (templateIt == m_RegistryTemplates.end())
        {
            OLO_CORE_ERROR("Registry template '{0}' not found", templateName);
            return nullptr;
        }

        // Clone the template registry
        auto instance = std::make_unique<UniformBufferRegistry>(*templateIt->second);
        instance->SetShader(shader);
        
        if (!instanceName.empty())
        {
            auto spec = instance->GetSpecification();
            spec.Name = instanceName;
            instance->UpdateSpecification(spec, false);
        }

        instance->Initialize();

        m_Statistics.TemplatesUsed++;
        m_Statistics.TemplateUsage[templateName]++;

        OLO_CORE_INFO("Created registry instance '{0}' from template '{1}'", 
                     instanceName.empty() ? "unnamed" : instanceName, templateName);

        return instance;
    }

    void ShaderTemplateManager::InitializeBuiltinTemplates()
    {
        if (m_BuiltinTemplatesInitialized)
            return;

        RegisterPBRTemplate();
        RegisterPostProcessTemplate();
        RegisterComputeTemplate();
        RegisterSkyboxTemplate();
        RegisterShadowMappingTemplate();
        RegisterInstancedRenderingTemplate();

        m_BuiltinTemplatesInitialized = true;
        OLO_CORE_INFO("Initialized built-in shader templates");
    }

    void ShaderTemplateManager::RegisterPBRTemplate()
    {
        TemplateInfo pbrTemplate("PBR", "Physically Based Rendering material template");
        pbrTemplate.Category = "Material";
        pbrTemplate.Priority = 1.0f;
        
        pbrTemplate.RequiredUniforms = {
            "u_ViewProjectionMatrix", "u_ModelMatrix", "u_ViewMatrix", "u_CameraPosition"
        };
        
        pbrTemplate.RequiredTextures = {
            "u_AlbedoTexture", "u_NormalTexture", "u_MetallicRoughnessTexture", "u_AOTexture"
        };
        
        pbrTemplate.DefaultSpec = CreatePBRSpec();
        
        // Add default resources
        pbrTemplate.DefaultResources["SystemUniforms"] = ShaderResourceInfo("SystemUniforms", ShaderResourceType::UniformBuffer, 0);
        pbrTemplate.DefaultResources["MaterialUniforms"] = ShaderResourceInfo("MaterialUniforms", ShaderResourceType::UniformBuffer, 1);
        pbrTemplate.DefaultResources["LightingUniforms"] = ShaderResourceInfo("LightingUniforms", ShaderResourceType::UniformBuffer, 2);

        RegisterTemplate(pbrTemplate);
    }

    void ShaderTemplateManager::RegisterPostProcessTemplate()
    {
        TemplateInfo postProcessTemplate("PostProcess", "Post-processing effect template");
        postProcessTemplate.Category = "PostProcess";
        postProcessTemplate.Priority = 0.8f;
        
        postProcessTemplate.RequiredTextures = {
            "u_SceneTexture", "u_DepthTexture"
        };
        
        postProcessTemplate.DefaultSpec = CreatePostProcessSpec();

        RegisterTemplate(postProcessTemplate);
    }

    void ShaderTemplateManager::RegisterComputeTemplate()
    {
        TemplateInfo computeTemplate("Compute", "Compute shader template");
        computeTemplate.Category = "Compute";
        computeTemplate.Priority = 0.9f;
        
        computeTemplate.RequiredBuffers = {
            "InputBuffer", "OutputBuffer"
        };
        
        computeTemplate.DefaultSpec = CreateComputeSpec();

        RegisterTemplate(computeTemplate);
    }

    void ShaderTemplateManager::RegisterSkyboxTemplate()
    {
        TemplateInfo skyboxTemplate("Skybox", "Skybox rendering template");
        skyboxTemplate.Category = "Environment";
        skyboxTemplate.Priority = 0.7f;
        
        skyboxTemplate.RequiredTextures = {
            "u_SkyboxTexture"
        };
        
        skyboxTemplate.RequiredUniforms = {
            "u_ViewProjectionMatrix"
        };

        RegisterTemplate(skyboxTemplate);
    }

    void ShaderTemplateManager::RegisterShadowMappingTemplate()
    {
        TemplateInfo shadowTemplate("ShadowMapping", "Shadow mapping template");
        shadowTemplate.Category = "Lighting";
        shadowTemplate.Priority = 0.8f;
        
        shadowTemplate.RequiredTextures = {
            "u_ShadowMap", "u_ShadowCascades"
        };
        
        shadowTemplate.RequiredUniforms = {
            "u_LightSpaceMatrix", "u_ShadowBias"
        };

        RegisterTemplate(shadowTemplate);
    }

    void ShaderTemplateManager::RegisterInstancedRenderingTemplate()
    {
        TemplateInfo instancedTemplate("Instanced", "Instanced rendering template");
        instancedTemplate.Category = "Performance";
        instancedTemplate.Priority = 0.6f;
        
        instancedTemplate.RequiredBuffers = {
            "InstanceData"
        };
        
        instancedTemplate.RequiredUniforms = {
            "u_ViewProjectionMatrix"
        };

        RegisterTemplate(instancedTemplate);
    }

    std::string ShaderTemplateManager::AnalyzeUniformNames(
        const std::unordered_map<std::string, ShaderResourceBinding>& bindings) const
    {
        std::unordered_map<std::string, u32> patternMatches;
        
        for (const auto& [name, binding] : bindings)
        {
            if (binding.Type == ShaderResourceType::UniformBuffer)
            {
                std::string lowerName = name;
                std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
                
                for (const auto& [pattern, keywords] : m_PatternKeywords)
                {
                    for (const std::string& keyword : keywords)
                    {
                        if (lowerName.find(keyword) != std::string::npos)
                        {
                            patternMatches[pattern]++;
                        }
                    }
                }
            }
        }
        
        // Find pattern with most matches
        std::string bestPattern = "Unknown";
        u32 maxMatches = 0;
        for (const auto& [pattern, matches] : patternMatches)
        {
            if (matches > maxMatches)
            {
                maxMatches = matches;
                bestPattern = pattern;
            }
        }
        
        return bestPattern;
    }

    f32 ShaderTemplateManager::CalculatePatternConfidence(
        const std::string& pattern, const std::unordered_map<std::string, ShaderResourceBinding>& bindings) const
    {
        auto keywordIt = m_PatternKeywords.find(pattern);
        if (keywordIt == m_PatternKeywords.end())
            return 0.0f;

        const auto& keywords = keywordIt->second;
        u32 matchCount = 0;
        u32 totalResources = 0;

        for (const auto& [name, binding] : bindings)
        {
            totalResources++;
            std::string lowerName = name;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
            
            for (const std::string& keyword : keywords)
            {
                if (lowerName.find(keyword) != std::string::npos)
                {
                    matchCount++;
                    break; // Only count each resource once per pattern
                }
            }
        }

        if (totalResources == 0)
            return 0.0f;

        f32 confidence = static_cast<f32>(matchCount) / static_cast<f32>(totalResources);
        
        // Apply pattern weight
        auto weightIt = m_PatternWeights.find(pattern);
        if (weightIt != m_PatternWeights.end())
        {
            confidence *= weightIt->second;
        }

        return std::min(confidence, 1.0f);
    }

    f32 ShaderTemplateManager::CalculateResourceMatch(const std::vector<std::string>& required,
                                                     const std::vector<std::string>& available) const
    {
        if (required.empty())
            return 1.0f; // Perfect match if no requirements

        u32 matchCount = 0;
        for (const std::string& req : required)
        {
            for (const std::string& avail : available)
            {
                if (CalculateNameSimilarity(req, avail) > 0.8f)
                {
                    matchCount++;
                    break;
                }
            }
        }

        return static_cast<f32>(matchCount) / static_cast<f32>(required.size());
    }

    f32 ShaderTemplateManager::CalculateNameSimilarity(const std::string& name1, const std::string& name2) const
    {
        if (name1 == name2)
            return 1.0f;

        // Simple substring matching - could be enhanced with edit distance
        std::string lower1 = name1, lower2 = name2;
        std::transform(lower1.begin(), lower1.end(), lower1.begin(), ::tolower);
        std::transform(lower2.begin(), lower2.end(), lower2.begin(), ::tolower);

        if (lower1.find(lower2) != std::string::npos || lower2.find(lower1) != std::string::npos)
            return 0.8f;

        return 0.0f;
    }

    std::vector<std::string> ShaderTemplateManager::ExtractResourceNames(
        const std::unordered_map<std::string, ShaderResourceBinding>& bindings, ShaderResourceType type) const
    {
        std::vector<std::string> names;
        for (const auto& [name, binding] : bindings)
        {
            if (binding.Type == type)
            {
                names.push_back(name);
            }
        }
        return names;
    }

    UniformBufferRegistrySpecification ShaderTemplateManager::CreatePBRSpec() const
    {
        auto spec = UniformBufferRegistrySpecification::GetPreset(RegistryConfiguration::Development);
        spec.Name = "PBR Material Registry";
        spec.UseSetPriority = true;
        spec.EnableDefaultResources = true;
        spec.AutoDetectShaderPattern = true;
        return spec;
    }

    UniformBufferRegistrySpecification ShaderTemplateManager::CreatePostProcessSpec() const
    {
        auto spec = UniformBufferRegistrySpecification::GetPreset(RegistryConfiguration::Performance);
        spec.Name = "PostProcess Registry";
        spec.UseSetPriority = false; // Post-process typically uses fewer sets
        spec.EnableDefaultResources = true;
        return spec;
    }

    UniformBufferRegistrySpecification ShaderTemplateManager::CreateComputeSpec() const
    {
        auto spec = UniformBufferRegistrySpecification::GetPreset(RegistryConfiguration::Performance);
        spec.Name = "Compute Registry";
        spec.UseSetPriority = true;
        spec.EnableDefaultResources = false; // Compute shaders often have custom resources
        return spec;
    }

    ShaderTemplateManager& ShaderTemplateManager::GetInstance()
    {
        static ShaderTemplateManager instance;
        return instance;
    }
}
