#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/UniformBufferRegistry.h"
#include "OloEngine/Renderer/ResourceHierarchy.h"
#include "OloEngine/Renderer/DeferredResourceResolver.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <memory>

namespace OloEngine
{
    /**
     * @brief Validation severity levels
     */
    enum class ValidationSeverity : u8
    {
        Info = 0,           // Informational message
        Warning = 1,        // Non-critical issue
        Error = 2,          // Critical error that must be fixed
        Fatal = 3           // Fatal error that prevents operation
    };

    /**
     * @brief Validation issue categories
     */
    enum class ValidationCategory : u8
    {
        ResourceBinding = 0,        // Issues with resource binding
        TypeMismatch = 1,           // Type validation errors
        DependencyGraph = 2,        // Dependency resolution issues
        ResourceLifetime = 3,       // Resource scope/lifetime issues
        Performance = 4,            // Performance-related warnings
        Compatibility = 5           // Compatibility issues
    };

    /**
     * @brief Information about a validation issue
     */
    struct ValidationIssue
    {
        ValidationSeverity Severity = ValidationSeverity::Info;
        ValidationCategory Category = ValidationCategory::ResourceBinding;
        std::string ResourceName;
        std::string Message;
        std::string Details;
        std::string Suggestion;                             // Suggested fix
        
        // Context information
        std::string ShaderName;
        u32 BindingPoint = 0;
        u32 FrameDetected = 0;
        
        // Source location (if available)
        std::string SourceFile;
        u32 SourceLine = 0;
        
        ValidationIssue() = default;
        ValidationIssue(ValidationSeverity severity, ValidationCategory category,
                       const std::string& resourceName, const std::string& message)
            : Severity(severity), Category(category), ResourceName(resourceName), Message(message) {}
    };

    /**
     * @brief Dependency rule for resource validation
     */
    struct DependencyRule
    {
        std::string ResourceName;
        std::unordered_set<std::string> RequiredDependencies;
        std::unordered_set<std::string> ForbiddenDependencies;
        std::unordered_set<ShaderResourceType> CompatibleTypes;
        ResourcePriority MinimumPriority = ResourcePriority::Instance;
        ResourceScope RequiredScope = ResourceScope::Frame;
        
        // Timing constraints
        bool MustBeResolvedFirst = false;
        bool CanBeResolvedLazy = true;
        
        DependencyRule() = default;
        DependencyRule(const std::string& resourceName)
            : ResourceName(resourceName) {}
    };

    /**
     * @brief Context information for validation
     */
    struct ValidationContext
    {
        const ResourceHierarchy* Hierarchy = nullptr;
        const DeferredResourceResolver* Resolver = nullptr;
        const UniformBufferRegistry* Registry = nullptr;
        
        // Current validation state
        u32 CurrentFrame = 0;
        std::string CurrentShader;
        
        // Validation options
        bool ValidatePerformance = true;
        bool ValidateCompatibility = true;
        bool ValidateLifetime = true;
        bool StrictMode = false;
    };

    /**
     * @brief Resource compatibility rule
     */
    struct CompatibilityRule
    {
        std::string Name;
        std::function<bool(const ResourceNode&, const ValidationContext&)> ValidatorFunction;
        ValidationSeverity FailureSeverity = ValidationSeverity::Error;
        std::string FailureMessage;
        
        CompatibilityRule() = default;
        CompatibilityRule(const std::string& name, ValidationSeverity severity, const std::string& message)
            : Name(name), FailureSeverity(severity), FailureMessage(message) {}
    };

    /**
     * @brief Comprehensive resource validation and dependency management system
     * 
     * Provides validation of resource hierarchies, dependency graphs, type compatibility,
     * and performance characteristics. Includes rule-based validation and customizable
     * validation pipelines.
     */
    class ResourceValidator
    {
    public:
        ResourceValidator() = default;
        ~ResourceValidator() = default;

        // No copy semantics
        ResourceValidator(const ResourceValidator&) = delete;
        ResourceValidator& operator=(const ResourceValidator&) = delete;

        // Move semantics
        ResourceValidator(ResourceValidator&&) = default;
        ResourceValidator& operator=(ResourceValidator&&) = default;

        /**
         * @brief Initialize the validator
         */
        void Initialize();

        /**
         * @brief Shutdown the validator
         */
        void Shutdown();

        /**
         * @brief Set the validation context
         * @param context Validation context with references to systems
         */
        void SetValidationContext(const ValidationContext& context);

        /**
         * @brief Add a dependency rule
         * @param rule Dependency rule to add
         */
        void AddDependencyRule(const DependencyRule& rule);

        /**
         * @brief Add a compatibility rule
         * @param rule Compatibility rule to add
         */
        void AddCompatibilityRule(const CompatibilityRule& rule);

        /**
         * @brief Remove a dependency rule
         * @param resourceName Resource name to remove rule for
         */
        void RemoveDependencyRule(const std::string& resourceName);

        /**
         * @brief Remove a compatibility rule
         * @param ruleName Name of the rule to remove
         */
        void RemoveCompatibilityRule(const std::string& ruleName);

        /**
         * @brief Validate a specific resource
         * @param resourceName Resource name to validate
         * @return Vector of validation issues found
         */
        std::vector<ValidationIssue> ValidateResource(const std::string& resourceName);

        /**
         * @brief Validate an entire resource hierarchy
         * @param hierarchy Resource hierarchy to validate
         * @return Vector of validation issues found
         */
        std::vector<ValidationIssue> ValidateHierarchy(const ResourceHierarchy& hierarchy);

        /**
         * @brief Validate a resource registry
         * @param registry Resource registry to validate
         * @return Vector of validation issues found
         */
        std::vector<ValidationIssue> ValidateRegistry(const UniformBufferRegistry& registry);

        /**
         * @brief Validate deferred resource resolution
         * @param resolver Deferred resource resolver to validate
         * @return Vector of validation issues found
         */
        std::vector<ValidationIssue> ValidateResolver(const DeferredResourceResolver& resolver);

        /**
         * @brief Validate dependency graph for circular dependencies
         * @param dependencies Map of resource dependencies
         * @return Vector of validation issues found
         */
        std::vector<ValidationIssue> ValidateDependencyGraph(
            const std::unordered_map<std::string, std::unordered_set<std::string>>& dependencies);

        /**
         * @brief Validate resource type compatibility
         * @param resourceName Resource name
         * @param actualType Actual resource type
         * @param expectedType Expected resource type
         * @return Validation issue if types are incompatible, empty optional otherwise
         */
        std::optional<ValidationIssue> ValidateTypeCompatibility(
            const std::string& resourceName, ShaderResourceType actualType, ShaderResourceType expectedType);

        /**
         * @brief Validate resource binding points for conflicts
         * @param bindings Map of resource names to binding points
         * @return Vector of validation issues found
         */
        std::vector<ValidationIssue> ValidateBindingConflicts(
            const std::unordered_map<std::string, u32>& bindings);

        /**
         * @brief Validate resource lifetime and scope consistency
         * @param hierarchy Resource hierarchy to check
         * @return Vector of validation issues found
         */
        std::vector<ValidationIssue> ValidateResourceLifetime(const ResourceHierarchy& hierarchy);

        /**
         * @brief Validate performance characteristics
         * @param hierarchy Resource hierarchy to analyze
         * @return Vector of validation issues found
         */
        std::vector<ValidationIssue> ValidatePerformance(const ResourceHierarchy& hierarchy);

        /**
         * @brief Check if a resource satisfies all dependency rules
         * @param resourceName Resource to check
         * @return true if all rules are satisfied
         */
        bool SatisfiesDependencyRules(const std::string& resourceName);

        /**
         * @brief Get dependency requirements for a resource
         * @param resourceName Resource name
         * @return Dependency rule if found, nullptr otherwise
         */
        const DependencyRule* GetDependencyRule(const std::string& resourceName) const;

        /**
         * @brief Get all validation issues from the last validation run
         * @return Vector of all issues found
         */
        const std::vector<ValidationIssue>& GetLastValidationIssues() const { return m_LastValidationIssues; }

        /**
         * @brief Get validation issues by severity
         * @param severity Severity level to filter by
         * @return Vector of issues with the specified severity
         */
        std::vector<ValidationIssue> GetIssuesBySeverity(ValidationSeverity severity) const;

        /**
         * @brief Get validation issues by category
         * @param category Category to filter by
         * @return Vector of issues in the specified category
         */
        std::vector<ValidationIssue> GetIssuesByCategory(ValidationCategory category) const;

        /**
         * @brief Clear all validation issues
         */
        void ClearValidationIssues();

        /**
         * @brief Set validation options
         * @param validatePerformance Enable performance validation
         * @param validateCompatibility Enable compatibility validation
         * @param validateLifetime Enable lifetime validation
         * @param strictMode Treat warnings as errors
         */
        void SetValidationOptions(bool validatePerformance, bool validateCompatibility,
                                bool validateLifetime, bool strictMode);

        /**
         * @brief Get validation statistics
         */
        struct ValidationStatistics
        {
            u32 TotalIssues = 0;
            u32 InfoIssues = 0;
            u32 WarningIssues = 0;
            u32 ErrorIssues = 0;
            u32 FatalIssues = 0;
            u32 ResourcesValidated = 0;
            u32 DependencyRules = 0;
            u32 CompatibilityRules = 0;
            f32 LastValidationTimeMs = 0.0f;
        };

        ValidationStatistics GetStatistics() const;

        /**
         * @brief Get string representation of validation severity
         * @param severity Severity to convert
         * @return String representation
         */
        static const char* GetSeverityString(ValidationSeverity severity);

        /**
         * @brief Get string representation of validation category
         * @param category Category to convert
         * @return String representation
         */
        static const char* GetCategoryString(ValidationCategory category);

        /**
         * @brief Render ImGui debug interface
         */
        void RenderDebugInterface();

        // Built-in validation rules

        /**
         * @brief Create default dependency rules for common resource patterns
         */
        void AddDefaultDependencyRules();

        /**
         * @brief Create default compatibility rules for type validation
         */
        void AddDefaultCompatibilityRules();

    private:
        // Validation context
        ValidationContext m_Context;
        
        // Validation rules
        std::unordered_map<std::string, DependencyRule> m_DependencyRules;
        std::unordered_map<std::string, CompatibilityRule> m_CompatibilityRules;
        
        // Validation results
        std::vector<ValidationIssue> m_LastValidationIssues;
        
        // Validation timing
        std::chrono::steady_clock::time_point m_LastValidationStart;
        f32 m_LastValidationTimeMs = 0.0f;
        
        // Statistics
        u32 m_ResourcesValidated = 0;
        
        // Initialization state
        bool m_Initialized = false;

        /**
         * @brief Add a validation issue to the results
         * @param issue Issue to add
         */
        void AddValidationIssue(const ValidationIssue& issue);

        /**
         * @brief Validate a single resource node
         * @param node Resource node to validate
         * @return Vector of validation issues
         */
        std::vector<ValidationIssue> ValidateResourceNode(const ResourceNode& node);

        /**
         * @brief Check for circular dependencies using DFS
         * @param dependencies Dependency graph
         * @param node Current node being visited
         * @param visited Set of visited nodes
         * @param recursionStack Current recursion stack
         * @param cycle Output vector for cycle path
         * @return true if cycle detected
         */
        bool DetectCircularDependency(
            const std::unordered_map<std::string, std::unordered_set<std::string>>& dependencies,
            const std::string& node,
            std::unordered_set<std::string>& visited,
            std::unordered_set<std::string>& recursionStack,
            std::vector<std::string>& cycle);

        /**
         * @brief Validate binding point usage
         * @param bindings Map of bindings to validate
         * @param resourceType Type of resources using these bindings
         * @return Vector of validation issues
         */
        std::vector<ValidationIssue> ValidateBindingPointUsage(
            const std::unordered_map<std::string, u32>& bindings,
            const std::string& resourceType);

        /**
         * @brief Check if resource types are compatible
         * @param type1 First type
         * @param type2 Second type
         * @return true if types are compatible
         */
        bool AreTypesCompatible(ShaderResourceType type1, ShaderResourceType type2);

        /**
         * @brief Validate resource scope hierarchy consistency
         * @param node Resource node to validate
         * @param hierarchy Parent hierarchy
         * @return Vector of validation issues
         */
        std::vector<ValidationIssue> ValidateResourceScopeConsistency(
            const ResourceNode& node, const ResourceHierarchy& hierarchy);

        /**
         * @brief Analyze performance implications of resource usage
         * @param node Resource node to analyze
         * @return Vector of performance-related validation issues
         */
        std::vector<ValidationIssue> AnalyzeResourcePerformance(const ResourceNode& node);

        /**
         * @brief Start validation timing
         */
        void StartValidationTiming();

        /**
         * @brief End validation timing and update statistics
         */
        void EndValidationTiming();
    };
}
