#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/UniformBufferRegistry.h"

#include <string>
#include <unordered_map>
#include <memory>
#include <functional>

namespace OloEngine
{
    /**
     * @brief Manager for shader resource templates and patterns
     * 
     * This class provides a centralized system for managing shader templates,
     * enabling rapid setup of common shader patterns and resource configurations.
     */
    class ShaderTemplateManager
    {
    public:
        /**
         * @brief Template information structure
         */
        struct TemplateInfo
        {
            std::string Name;
            std::string Description;
            std::string Category;                    // e.g., "PBR", "PostProcess", "Compute"
            std::vector<std::string> RequiredUniforms;
            std::vector<std::string> RequiredTextures;
            std::vector<std::string> RequiredBuffers;
            UniformBufferRegistrySpecification DefaultSpec;
            std::unordered_map<std::string, ShaderResourceInfo> DefaultResources;
            f32 Priority = 1.0f;                     // Template priority for auto-selection
            
            TemplateInfo() = default;
            TemplateInfo(const std::string& name, const std::string& description)
                : Name(name), Description(description) {}
        };

        /**
         * @brief Template match result
         */
        struct TemplateMatch
        {
            std::string TemplateName;
            f32 MatchScore = 0.0f;                   // 0.0 to 1.0, higher is better
            std::vector<std::string> MissingResources;
            std::vector<std::string> ExtraResources;
            std::string Reasoning;                   // Why this template was suggested
            
            bool IsGoodMatch() const { return MatchScore >= 0.7f; }
            bool IsViableMatch() const { return MatchScore >= 0.5f; }
        };

        /**
         * @brief Pattern detection result
         */
        struct PatternDetectionResult
        {
            std::string DetectedPattern;             // Primary pattern detected
            std::vector<TemplateMatch> Suggestions;  // Ranked template suggestions
            std::unordered_map<std::string, f32> PatternConfidence; // Pattern -> confidence
            std::string AutoSelectedTemplate;       // Best automatic selection
            bool HasHighConfidenceMatch = false;    // Whether there's a very confident match
        };

    public:
        ShaderTemplateManager();
        ~ShaderTemplateManager() = default;

        // Template management
        /**
         * @brief Register a new template
         * @param templateInfo Template information
         * @param registryTemplate Optional registry template to associate
         */
        void RegisterTemplate(const TemplateInfo& templateInfo, 
                             const UniformBufferRegistry* registryTemplate = nullptr);

        /**
         * @brief Remove a template
         * @param templateName Name of template to remove
         */
        void RemoveTemplate(const std::string& templateName);

        /**
         * @brief Get template information
         * @param templateName Name of template
         * @return Template info or nullptr if not found
         */
        const TemplateInfo* GetTemplateInfo(const std::string& templateName) const;

        /**
         * @brief Get all registered templates
         * @return Map of template name to template info
         */
        const std::unordered_map<std::string, TemplateInfo>& GetAllTemplates() const { return m_Templates; }

        /**
         * @brief Get templates by category
         * @param category Category to filter by
         * @return Vector of template names in that category
         */
        std::vector<std::string> GetTemplatesByCategory(const std::string& category) const;

        // Pattern detection and template matching
        /**
         * @brief Detect shader pattern and suggest templates
         * @param registry Registry to analyze
         * @return Pattern detection result with template suggestions
         */
        PatternDetectionResult DetectPatternAndSuggestTemplates(const UniformBufferRegistry& registry) const;

        /**
         * @brief Match shader resources against a specific template
         * @param registry Registry to match
         * @param templateName Template to match against
         * @return Match result with score and details
         */
        TemplateMatch MatchAgainstTemplate(const UniformBufferRegistry& registry, 
                                          const std::string& templateName) const;

        /**
         * @brief Find best matching template for a registry
         * @param registry Registry to match
         * @param minScore Minimum match score to consider (default: 0.5)
         * @return Best template match or empty match if none found
         */
        TemplateMatch FindBestTemplate(const UniformBufferRegistry& registry, f32 minScore = 0.5f) const;

        // Template application
        /**
         * @brief Apply template to a registry
         * @param registry Target registry
         * @param templateName Template to apply
         * @return true if template was applied successfully
         */
        bool ApplyTemplate(UniformBufferRegistry& registry, const std::string& templateName) const;

        /**
         * @brief Create registry from template
         * @param templateName Template to use
         * @param shader Target shader
         * @param instanceName Name for the instance
         * @return New registry instance or nullptr if failed
         */
        Scope<UniformBufferRegistry> CreateFromTemplate(const std::string& templateName,
                                                       const Ref<Shader>& shader,
                                                       const std::string& instanceName = "") const;

        // Built-in templates
        /**
         * @brief Initialize built-in templates (PBR, post-process, etc.)
         */
        void InitializeBuiltinTemplates();

        /**
         * @brief Register PBR material template
         */
        void RegisterPBRTemplate();

        /**
         * @brief Register post-processing template
         */
        void RegisterPostProcessTemplate();

        /**
         * @brief Register compute shader template
         */
        void RegisterComputeTemplate();

        /**
         * @brief Register skybox template
         */
        void RegisterSkyboxTemplate();

        /**
         * @brief Register shadow mapping template
         */
        void RegisterShadowMappingTemplate();

        /**
         * @brief Register instanced rendering template
         */
        void RegisterInstancedRenderingTemplate();

        // Advanced features
        /**
         * @brief Generate template from existing registry
         * @param registry Source registry
         * @param templateName Name for the new template
         * @param description Description for the template
         * @param category Category for the template
         * @return true if template was generated successfully
         */
        bool GenerateTemplateFromRegistry(const UniformBufferRegistry& registry,
                                        const std::string& templateName,
                                        const std::string& description,
                                        const std::string& category = "Custom");

        /**
         * @brief Export templates to file
         * @param filepath Path to export file
         * @return true if export was successful
         */
        bool ExportTemplates(const std::string& filepath) const;

        /**
         * @brief Import templates from file
         * @param filepath Path to import file
         * @return true if import was successful
         */
        bool ImportTemplates(const std::string& filepath);

        // Statistics and debugging
        struct Statistics
        {
            u32 TotalTemplates = 0;
            u32 TemplatesUsed = 0;
            u32 PatternDetectionsPerformed = 0;
            u32 SuccessfulMatches = 0;
            f32 AverageMatchScore = 0.0f;
            std::unordered_map<std::string, u32> CategoryUsage;
            std::unordered_map<std::string, u32> TemplateUsage;

            void Reset() { *this = Statistics{}; }
        };

        const Statistics& GetStatistics() const { return m_Statistics; }
        void ResetStatistics() { m_Statistics.Reset(); }

        /**
         * @brief Generate usage report
         * @return Formatted usage report string
         */
        std::string GenerateUsageReport() const;

        /**
         * @brief Render ImGui debug interface
         */
        void RenderDebugInterface();

        // Singleton access
        static ShaderTemplateManager& GetInstance();

    private:
        // Pattern detection helpers
        std::string AnalyzeUniformNames(const std::unordered_map<std::string, ShaderResourceBinding>& bindings) const;
        std::string AnalyzeTextureNames(const std::unordered_map<std::string, ShaderResourceBinding>& bindings) const;
        f32 CalculatePatternConfidence(const std::string& pattern, 
                                      const std::unordered_map<std::string, ShaderResourceBinding>& bindings) const;

        // Template matching helpers
        f32 CalculateResourceMatch(const std::vector<std::string>& required,
                                  const std::vector<std::string>& available) const;
        f32 CalculateNameSimilarity(const std::string& name1, const std::string& name2) const;
        std::vector<std::string> ExtractResourceNames(const std::unordered_map<std::string, ShaderResourceBinding>& bindings,
                                                     ShaderResourceType type) const;

        // Built-in template helpers
        UniformBufferRegistrySpecification CreatePBRSpec() const;
        UniformBufferRegistrySpecification CreatePostProcessSpec() const;
        UniformBufferRegistrySpecification CreateComputeSpec() const;

    private:
        std::unordered_map<std::string, TemplateInfo> m_Templates;
        std::unordered_map<std::string, Scope<UniformBufferRegistry>> m_RegistryTemplates;
        mutable Statistics m_Statistics;
        
        // Pattern keywords for detection
        std::unordered_map<std::string, std::vector<std::string>> m_PatternKeywords;
        std::unordered_map<std::string, f32> m_PatternWeights;
        
        // Initialized state
        bool m_BuiltinTemplatesInitialized = false;
    };
}
