#include "OloEnginePCH.h"
#include "ResourceValidator.h"
#include "OloEngine/Core/Log.h"

#include <imgui.h>
#include <algorithm>
#include <chrono>

namespace OloEngine
{
    void ResourceValidator::Initialize()
    {
        if (m_Initialized)
        {
            OLO_CORE_WARN("ResourceValidator already initialized");
            return;
        }

        m_DependencyRules.clear();
        m_CompatibilityRules.clear();
        m_LastValidationIssues.clear();
        m_ResourcesValidated = 0;
        m_LastValidationTimeMs = 0.0f;
        m_Initialized = true;

        // Add default rules
        AddDefaultDependencyRules();
        AddDefaultCompatibilityRules();

        OLO_CORE_TRACE("ResourceValidator initialized");
    }

    void ResourceValidator::Shutdown()
    {
        m_DependencyRules.clear();
        m_CompatibilityRules.clear();
        m_LastValidationIssues.clear();
        m_Initialized = false;

        OLO_CORE_TRACE("ResourceValidator shutdown");
    }

    void ResourceValidator::SetValidationContext(const ValidationContext& context)
    {
        m_Context = context;
    }

    void ResourceValidator::AddDependencyRule(const DependencyRule& rule)
    {
        m_DependencyRules[rule.ResourceName] = rule;
        OLO_CORE_TRACE("Added dependency rule for resource '{0}'", rule.ResourceName);
    }

    void ResourceValidator::AddCompatibilityRule(const CompatibilityRule& rule)
    {
        m_CompatibilityRules[rule.Name] = rule;
        OLO_CORE_TRACE("Added compatibility rule '{0}'", rule.Name);
    }

    void ResourceValidator::RemoveDependencyRule(const std::string& resourceName)
    {
        m_DependencyRules.erase(resourceName);
    }

    void ResourceValidator::RemoveCompatibilityRule(const std::string& ruleName)
    {
        m_CompatibilityRules.erase(ruleName);
    }

    std::vector<ValidationIssue> ResourceValidator::ValidateResource(const std::string& resourceName)
    {
        if (!m_Initialized)
        {
            OLO_CORE_ERROR("ResourceValidator not initialized");
            return {};
        }

        StartValidationTiming();
        std::vector<ValidationIssue> issues;

        if (m_Context.Hierarchy)
        {
            const ResourceNode* node = m_Context.Hierarchy->GetResource(resourceName);
            if (node)
            {
                auto nodeIssues = ValidateResourceNode(*node);
                issues.insert(issues.end(), nodeIssues.begin(), nodeIssues.end());
            }
            else
            {
                ValidationIssue issue(ValidationSeverity::Warning, ValidationCategory::ResourceBinding,
                                    resourceName, "Resource not found in hierarchy");
                issues.push_back(issue);
            }
        }

        m_ResourcesValidated++;
        EndValidationTiming();

        return issues;
    }

    std::vector<ValidationIssue> ResourceValidator::ValidateHierarchy(const ResourceHierarchy& hierarchy)
    {
        StartValidationTiming();
        std::vector<ValidationIssue> issues;

        // Get all resources and validate each one
        auto stats = hierarchy.GetStatistics();
        m_ResourcesValidated = stats.TotalResources;

        // Validate dependency graph
        std::unordered_map<std::string, std::unordered_set<std::string>> dependencies;
        auto allResources = hierarchy.GetResourcesInDependencyOrder();
        
        for (const ResourceNode* node : allResources)
        {
            // Validate individual node
            auto nodeIssues = ValidateResourceNode(*node);
            issues.insert(issues.end(), nodeIssues.begin(), nodeIssues.end());

            // Build dependency map for graph validation
            dependencies[node->Name] = node->Dependencies;
        }

        // Validate the dependency graph
        auto depIssues = ValidateDependencyGraph(dependencies);
        issues.insert(issues.end(), depIssues.begin(), depIssues.end());

        // Validate lifetime consistency
        if (m_Context.ValidateLifetime)
        {
            auto lifetimeIssues = ValidateResourceLifetime(hierarchy);
            issues.insert(issues.end(), lifetimeIssues.begin(), lifetimeIssues.end());
        }

        // Validate performance characteristics
        if (m_Context.ValidatePerformance)
        {
            auto perfIssues = ValidatePerformance(hierarchy);
            issues.insert(issues.end(), perfIssues.begin(), perfIssues.end());
        }

        EndValidationTiming();
        m_LastValidationIssues = issues;

        OLO_CORE_TRACE("Validated hierarchy: {0} resources, {1} issues found", stats.TotalResources, issues.size());
        return issues;
    }

    std::vector<ValidationIssue> ResourceValidator::ValidateRegistry(const UniformBufferRegistry& registry)
    {
        StartValidationTiming();
        std::vector<ValidationIssue> issues;

        // Get all bindings
        const auto& bindings = registry.GetBindings();
        const auto& boundResources = registry.GetBoundResources();

        // Validate binding point conflicts
        std::unordered_map<std::string, u32> bindingPoints;
        for (const auto& [name, binding] : bindings)
        {
            bindingPoints[name] = binding.BindingPoint;
        }

        auto bindingIssues = ValidateBindingConflicts(bindingPoints);
        issues.insert(issues.end(), bindingIssues.begin(), bindingIssues.end());

        // Validate type consistency
        for (const auto& [name, binding] : bindings)
        {
            auto resourceIt = boundResources.find(name);
            if (resourceIt != boundResources.end())
            {
                // Extract type from variant
                ShaderResourceType actualType = ShaderResourceType::None;
                if (std::holds_alternative<Ref<UniformBuffer>>(resourceIt->second))
                    actualType = ShaderResourceType::UniformBuffer;
                else if (std::holds_alternative<Ref<StorageBuffer>>(resourceIt->second))
                    actualType = ShaderResourceType::StorageBuffer;
                else if (std::holds_alternative<Ref<Texture2D>>(resourceIt->second))
                    actualType = ShaderResourceType::Texture2D;
                else if (std::holds_alternative<Ref<TextureCubemap>>(resourceIt->second))
                    actualType = ShaderResourceType::TextureCube;
                // Add more types as needed

                auto typeIssue = ValidateTypeCompatibility(name, actualType, binding.Type);
                if (typeIssue.has_value())
                {
                    issues.push_back(typeIssue.value());
                }
            }
            else if (binding.IsActive)
            {
                ValidationIssue issue(ValidationSeverity::Error, ValidationCategory::ResourceBinding,
                                    name, "Active binding has no bound resource");
                issue.BindingPoint = binding.BindingPoint;
                issue.ShaderName = m_Context.CurrentShader;
                issues.push_back(issue);
            }
        }

        m_ResourcesValidated += static_cast<u32>(bindings.size());
        EndValidationTiming();

        return issues;
    }

    std::vector<ValidationIssue> ResourceValidator::ValidateResolver(const DeferredResourceResolver& resolver)
    {
        StartValidationTiming();
        std::vector<ValidationIssue> issues;

        auto stats = resolver.GetStatistics();

        // Check for excessive failed requests
        if (stats.FailedRequests > 0)
        {
            f32 failureRate = static_cast<f32>(stats.FailedRequests) / 
                             static_cast<f32>(stats.FailedRequests + stats.ResolvedRequests);
            
            if (failureRate > 0.2f) // More than 20% failure rate
            {
                ValidationIssue issue(ValidationSeverity::Warning, ValidationCategory::Performance,
                                    "DeferredResolver", "High resource resolution failure rate");
                issue.Details = "Failure rate: " + std::to_string(failureRate * 100.0f) + "%";
                issue.Suggestion = "Check resource resolver functions and dependency setup";
                issues.push_back(issue);
            }
        }

        // Check for excessive deferred requests
        if (stats.DeferredRequests > stats.ResolvedRequests)
        {
            ValidationIssue issue(ValidationSeverity::Warning, ValidationCategory::DependencyGraph,
                                "DeferredResolver", "Many requests are being deferred");
            issue.Details = "Deferred: " + std::to_string(stats.DeferredRequests) + 
                           ", Resolved: " + std::to_string(stats.ResolvedRequests);
            issue.Suggestion = "Check for missing dependencies or circular dependency chains";
            issues.push_back(issue);
        }

        // Check resolution performance
        if (stats.AverageResolutionTimeMs > 10.0f) // More than 10ms average
        {
            ValidationIssue issue(ValidationSeverity::Warning, ValidationCategory::Performance,
                                "DeferredResolver", "Slow resource resolution performance");
            issue.Details = "Average resolution time: " + std::to_string(stats.AverageResolutionTimeMs) + "ms";
            issue.Suggestion = "Optimize resource resolver functions or use background resolution";
            issues.push_back(issue);
        }

        EndValidationTiming();
        return issues;
    }

    std::vector<ValidationIssue> ResourceValidator::ValidateDependencyGraph(
        const std::unordered_map<std::string, std::unordered_set<std::string>>& dependencies)
    {
        std::vector<ValidationIssue> issues;

        // Check for circular dependencies
        std::unordered_set<std::string> visited;
        std::unordered_set<std::string> recursionStack;
        std::vector<std::string> cycle;

        for (const auto& [nodeName, deps] : dependencies)
        {
            if (visited.find(nodeName) == visited.end())
            {
                if (DetectCircularDependency(dependencies, nodeName, visited, recursionStack, cycle))
                {
                    ValidationIssue issue(ValidationSeverity::Error, ValidationCategory::DependencyGraph,
                                        nodeName, "Circular dependency detected");
                    
                    std::string cycleStr = "Cycle: ";
                    for (size_t i = 0; i < cycle.size(); ++i)
                    {
                        cycleStr += cycle[i];
                        if (i < cycle.size() - 1) cycleStr += " -> ";
                    }
                    issue.Details = cycleStr;
                    issue.Suggestion = "Remove one of the dependencies to break the cycle";
                    
                    issues.push_back(issue);
                    break; // One circular dependency report is usually enough
                }
            }
        }

        // Check for missing dependencies
        for (const auto& [nodeName, deps] : dependencies)
        {
            for (const std::string& depName : deps)
            {
                if (dependencies.find(depName) == dependencies.end())
                {
                    ValidationIssue issue(ValidationSeverity::Warning, ValidationCategory::DependencyGraph,
                                        nodeName, "Dependency not found in graph");
                    issue.Details = "Missing dependency: " + depName;
                    issue.Suggestion = "Ensure all dependencies are properly registered";
                    issues.push_back(issue);
                }
            }
        }

        return issues;
    }

    std::optional<ValidationIssue> ResourceValidator::ValidateTypeCompatibility(
        const std::string& resourceName, ShaderResourceType actualType, ShaderResourceType expectedType)
    {
        if (!AreTypesCompatible(actualType, expectedType))
        {
            ValidationIssue issue(ValidationSeverity::Error, ValidationCategory::TypeMismatch,
                                resourceName, "Resource type mismatch");
            issue.Details = "Expected type: " + std::to_string(static_cast<u32>(expectedType)) +
                           ", Actual type: " + std::to_string(static_cast<u32>(actualType));
            issue.Suggestion = "Ensure resource type matches shader binding declaration";
            return issue;
        }
        return std::nullopt;
    }

    std::vector<ValidationIssue> ResourceValidator::ValidateBindingConflicts(
        const std::unordered_map<std::string, u32>& bindings)
    {
        std::vector<ValidationIssue> issues;
        std::unordered_map<u32, std::vector<std::string>> bindingPointUsage;

        // Group resources by binding point
        for (const auto& [resourceName, bindingPoint] : bindings)
        {
            bindingPointUsage[bindingPoint].push_back(resourceName);
        }

        // Check for conflicts
        for (const auto& [bindingPoint, resourceNames] : bindingPointUsage)
        {
            if (resourceNames.size() > 1)
            {
                ValidationIssue issue(ValidationSeverity::Error, ValidationCategory::ResourceBinding,
                                    resourceNames[0], "Binding point conflict");
                
                std::string conflictStr = "Resources sharing binding point " + std::to_string(bindingPoint) + ": ";
                for (size_t i = 0; i < resourceNames.size(); ++i)
                {
                    conflictStr += resourceNames[i];
                    if (i < resourceNames.size() - 1) conflictStr += ", ";
                }
                issue.Details = conflictStr;
                issue.BindingPoint = bindingPoint;
                issue.Suggestion = "Assign unique binding points to each resource";
                
                issues.push_back(issue);
            }
        }

        return issues;
    }

    std::vector<ValidationIssue> ResourceValidator::ValidateResourceLifetime(const ResourceHierarchy& hierarchy)
    {
        std::vector<ValidationIssue> issues;
        auto allResources = hierarchy.GetResourcesInDependencyOrder();

        for (const ResourceNode* node : allResources)
        {
            auto scopeIssues = ValidateResourceScopeConsistency(*node, hierarchy);
            issues.insert(issues.end(), scopeIssues.begin(), scopeIssues.end());
        }

        return issues;
    }

    std::vector<ValidationIssue> ResourceValidator::ValidatePerformance(const ResourceHierarchy& hierarchy)
    {
        std::vector<ValidationIssue> issues;
        auto allResources = hierarchy.GetResourcesInDependencyOrder();

        for (const ResourceNode* node : allResources)
        {
            auto perfIssues = AnalyzeResourcePerformance(*node);
            issues.insert(issues.end(), perfIssues.begin(), perfIssues.end());
        }

        return issues;
    }

    bool ResourceValidator::SatisfiesDependencyRules(const std::string& resourceName)
    {
        auto ruleIt = m_DependencyRules.find(resourceName);
        if (ruleIt == m_DependencyRules.end())
        {
            return true; // No rules to violate
        }

        const DependencyRule& rule = ruleIt->second;

        // Check if we have hierarchy context
        if (!m_Context.Hierarchy)
        {
            return false; // Can't validate without hierarchy
        }

        const ResourceNode* node = m_Context.Hierarchy->GetResource(resourceName);
        if (!node)
        {
            return false; // Resource not found
        }

        // Check required dependencies
        for (const std::string& requiredDep : rule.RequiredDependencies)
        {
            if (node->Dependencies.find(requiredDep) == node->Dependencies.end())
            {
                return false; // Missing required dependency
            }
        }

        // Check forbidden dependencies
        for (const std::string& forbiddenDep : rule.ForbiddenDependencies)
        {
            if (node->Dependencies.find(forbiddenDep) != node->Dependencies.end())
            {
                return false; // Has forbidden dependency
            }
        }

        // Check type compatibility
        if (!rule.CompatibleTypes.empty())
        {
            if (rule.CompatibleTypes.find(node->Type) == rule.CompatibleTypes.end())
            {
                return false; // Type not in compatible set
            }
        }

        // Check priority and scope
        if (node->Priority < rule.MinimumPriority)
        {
            return false; // Priority too low
        }

        if (node->Scope != rule.RequiredScope && rule.RequiredScope != ResourceScope::Frame) // Frame is most permissive
        {
            return false; // Scope mismatch
        }

        return true;
    }

    const DependencyRule* ResourceValidator::GetDependencyRule(const std::string& resourceName) const
    {
        auto it = m_DependencyRules.find(resourceName);
        return it != m_DependencyRules.end() ? &it->second : nullptr;
    }

    std::vector<ValidationIssue> ResourceValidator::GetIssuesBySeverity(ValidationSeverity severity) const
    {
        std::vector<ValidationIssue> result;
        std::copy_if(m_LastValidationIssues.begin(), m_LastValidationIssues.end(),
                    std::back_inserter(result),
                    [severity](const ValidationIssue& issue) {
                        return issue.Severity == severity;
                    });
        return result;
    }

    std::vector<ValidationIssue> ResourceValidator::GetIssuesByCategory(ValidationCategory category) const
    {
        std::vector<ValidationIssue> result;
        std::copy_if(m_LastValidationIssues.begin(), m_LastValidationIssues.end(),
                    std::back_inserter(result),
                    [category](const ValidationIssue& issue) {
                        return issue.Category == category;
                    });
        return result;
    }

    void ResourceValidator::ClearValidationIssues()
    {
        m_LastValidationIssues.clear();
    }

    void ResourceValidator::SetValidationOptions(bool validatePerformance, bool validateCompatibility,
                                                bool validateLifetime, bool strictMode)
    {
        m_Context.ValidatePerformance = validatePerformance;
        m_Context.ValidateCompatibility = validateCompatibility;
        m_Context.ValidateLifetime = validateLifetime;
        m_Context.StrictMode = strictMode;
    }

    ResourceValidator::ValidationStatistics ResourceValidator::GetStatistics() const
    {
        ValidationStatistics stats;
        stats.ResourcesValidated = m_ResourcesValidated;
        stats.DependencyRules = static_cast<u32>(m_DependencyRules.size());
        stats.CompatibilityRules = static_cast<u32>(m_CompatibilityRules.size());
        stats.LastValidationTimeMs = m_LastValidationTimeMs;

        for (const ValidationIssue& issue : m_LastValidationIssues)
        {
            stats.TotalIssues++;
            switch (issue.Severity)
            {
                case ValidationSeverity::Info:    stats.InfoIssues++;    break;
                case ValidationSeverity::Warning: stats.WarningIssues++; break;
                case ValidationSeverity::Error:   stats.ErrorIssues++;   break;
                case ValidationSeverity::Fatal:   stats.FatalIssues++;   break;
            }
        }

        return stats;
    }

    const char* ResourceValidator::GetSeverityString(ValidationSeverity severity)
    {
        switch (severity)
        {
            case ValidationSeverity::Info:    return "Info";
            case ValidationSeverity::Warning: return "Warning";
            case ValidationSeverity::Error:   return "Error";
            case ValidationSeverity::Fatal:   return "Fatal";
            default:                          return "Unknown";
        }
    }

    const char* ResourceValidator::GetCategoryString(ValidationCategory category)
    {
        switch (category)
        {
            case ValidationCategory::ResourceBinding: return "Resource Binding";
            case ValidationCategory::TypeMismatch:    return "Type Mismatch";
            case ValidationCategory::DependencyGraph: return "Dependency Graph";
            case ValidationCategory::ResourceLifetime: return "Resource Lifetime";
            case ValidationCategory::Performance:     return "Performance";
            case ValidationCategory::Compatibility:   return "Compatibility";
            default:                                  return "Unknown";
        }
    }

    void ResourceValidator::RenderDebugInterface()
    {
        if (!m_Initialized)
            return;

        ImGui::Text("Resource Validator");
        ImGui::Separator();

        // Statistics
        auto stats = GetStatistics();
        if (ImGui::CollapsingHeader("Statistics", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Resources Validated: %u", stats.ResourcesValidated);
            ImGui::Text("Dependency Rules: %u", stats.DependencyRules);
            ImGui::Text("Compatibility Rules: %u", stats.CompatibilityRules);
            ImGui::Text("Last Validation Time: %.2f ms", stats.LastValidationTimeMs);
            
            ImGui::Separator();
            ImGui::Text("Issues Found:");
            ImGui::BulletText("Info: %u", stats.InfoIssues);
            ImGui::BulletText("Warning: %u", stats.WarningIssues);
            ImGui::BulletText("Error: %u", stats.ErrorIssues);
            ImGui::BulletText("Fatal: %u", stats.FatalIssues);
            ImGui::BulletText("Total: %u", stats.TotalIssues);
        }

        // Validation options
        if (ImGui::CollapsingHeader("Validation Options"))
        {
            ImGui::Checkbox("Validate Performance", &m_Context.ValidatePerformance);
            ImGui::Checkbox("Validate Compatibility", &m_Context.ValidateCompatibility);
            ImGui::Checkbox("Validate Lifetime", &m_Context.ValidateLifetime);
            ImGui::Checkbox("Strict Mode", &m_Context.StrictMode);
        }

        // Validation issues
        if (ImGui::CollapsingHeader("Validation Issues"))
        {
            // Filter options
            static int severityFilter = -1; // -1 means all
            static int categoryFilter = -1;

            ImGui::PushItemWidth(150);
            const char* severityItems[] = { "All", "Info", "Warning", "Error", "Fatal" };
            ImGui::Combo("Severity Filter", &severityFilter, severityItems, IM_ARRAYSIZE(severityItems));
            
            ImGui::SameLine();
            const char* categoryItems[] = { "All", "Resource Binding", "Type Mismatch", "Dependency Graph", 
                                          "Resource Lifetime", "Performance", "Compatibility" };
            ImGui::Combo("Category Filter", &categoryFilter, categoryItems, IM_ARRAYSIZE(categoryItems));
            ImGui::PopItemWidth();

            ImGui::Separator();

            // Issues table
            if (ImGui::BeginTable("ValidationIssuesTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
            {
                ImGui::TableSetupColumn("Severity");
                ImGui::TableSetupColumn("Category");
                ImGui::TableSetupColumn("Resource");
                ImGui::TableSetupColumn("Message");
                ImGui::TableSetupColumn("Details");
                ImGui::TableHeadersRow();

                for (const ValidationIssue& issue : m_LastValidationIssues)
                {
                    // Apply filters
                    if (severityFilter >= 0 && static_cast<int>(issue.Severity) != severityFilter - 1)
                        continue;
                    if (categoryFilter >= 0 && static_cast<int>(issue.Category) != categoryFilter - 1)
                        continue;

                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    const char* severityStr = GetSeverityString(issue.Severity);
                    ImVec4 severityColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
                    
                    switch (issue.Severity)
                    {
                        case ValidationSeverity::Info:    severityColor = ImVec4(0.7f, 0.7f, 1.0f, 1.0f); break;
                        case ValidationSeverity::Warning: severityColor = ImVec4(1.0f, 1.0f, 0.0f, 1.0f); break;
                        case ValidationSeverity::Error:   severityColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f); break;
                        case ValidationSeverity::Fatal:   severityColor = ImVec4(1.0f, 0.0f, 0.0f, 1.0f); break;
                    }
                    
                    ImGui::TextColored(severityColor, "%s", severityStr);

                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s", GetCategoryString(issue.Category));

                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%s", issue.ResourceName.c_str());

                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%s", issue.Message.c_str());

                    ImGui::TableSetColumnIndex(4);
                    if (!issue.Details.empty())
                    {
                        if (ImGui::IsItemHovered())
                        {
                            ImGui::BeginTooltip();
                            ImGui::Text("Details: %s", issue.Details.c_str());
                            if (!issue.Suggestion.empty())
                            {
                                ImGui::Text("Suggestion: %s", issue.Suggestion.c_str());
                            }
                            ImGui::EndTooltip();
                        }
                        ImGui::Text("...");
                    }
                }

                ImGui::EndTable();
            }
        }

        // Rules management
        if (ImGui::CollapsingHeader("Rules"))
        {
            if (ImGui::TreeNode("Dependency Rules"))
            {
                for (const auto& [name, rule] : m_DependencyRules)
                {
                    if (ImGui::TreeNode(name.c_str()))
                    {
                        ImGui::Text("Required Dependencies: %zu", rule.RequiredDependencies.size());
                        ImGui::Text("Forbidden Dependencies: %zu", rule.ForbiddenDependencies.size());
                        ImGui::Text("Compatible Types: %zu", rule.CompatibleTypes.size());
                        ImGui::Text("Minimum Priority: %s", ResourceHierarchy::GetPriorityString(rule.MinimumPriority));
                        ImGui::Text("Required Scope: %s", ResourceHierarchy::GetScopeString(rule.RequiredScope));
                        ImGui::TreePop();
                    }
                }
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Compatibility Rules"))
            {
                for (const auto& [name, rule] : m_CompatibilityRules)
                {
                    ImGui::BulletText("%s (Severity: %s)", name.c_str(), GetSeverityString(rule.FailureSeverity));
                }
                ImGui::TreePop();
            }
        }

        // Controls
        ImGui::Separator();
        if (ImGui::Button("Clear Issues"))
        {
            ClearValidationIssues();
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Default Rules"))
        {
            AddDefaultDependencyRules();
            AddDefaultCompatibilityRules();
        }
    }

    void ResourceValidator::AddDefaultDependencyRules()
    {
        // Camera matrices should be resolved first
        {
            DependencyRule rule("u_ViewMatrix");
            rule.MustBeResolvedFirst = true;
            rule.MinimumPriority = ResourcePriority::System;
            rule.RequiredScope = ResourceScope::Frame;
            rule.CompatibleTypes.insert(ShaderResourceType::UniformBuffer);
            AddDependencyRule(rule);
        }

        {
            DependencyRule rule("u_ProjectionMatrix");
            rule.MustBeResolvedFirst = true;
            rule.MinimumPriority = ResourcePriority::System;
            rule.RequiredScope = ResourceScope::Frame;
            rule.CompatibleTypes.insert(ShaderResourceType::UniformBuffer);
            AddDependencyRule(rule);
        }

        // Lighting data dependencies
        {
            DependencyRule rule("u_LightData");
            rule.RequiredDependencies.insert("u_ViewMatrix");
            rule.MinimumPriority = ResourcePriority::Global;
            rule.CompatibleTypes.insert(ShaderResourceType::UniformBuffer);
            rule.CompatibleTypes.insert(ShaderResourceType::StorageBuffer);
            AddDependencyRule(rule);
        }

        // Material properties
        {
            DependencyRule rule("u_Material");
            rule.MinimumPriority = ResourcePriority::Material;
            rule.CompatibleTypes.insert(ShaderResourceType::UniformBuffer);
            AddDependencyRule(rule);
        }

        OLO_CORE_TRACE("Added default dependency rules");
    }

    void ResourceValidator::AddDefaultCompatibilityRules()
    {
        // Texture binding validation
        {
            CompatibilityRule rule("TextureBindingValidation", ValidationSeverity::Error,
                                 "Texture resources must use appropriate binding points");
            rule.ValidatorFunction = [](const ResourceNode& node, const ValidationContext& context) -> bool {
                if (node.Type == ShaderResourceType::Texture2D || node.Type == ShaderResourceType::TextureCube)
                {
                    return node.BindingPoint < 32; // Most GPUs support at least 32 texture units
                }
                return true;
            };
            AddCompatibilityRule(rule);
        }

        // Buffer size validation
        {
            CompatibilityRule rule("BufferSizeValidation", ValidationSeverity::Warning,
                                 "Large buffers may impact performance");
            rule.ValidatorFunction = [](const ResourceNode& node, const ValidationContext& context) -> bool {
                if (node.Type == ShaderResourceType::UniformBuffer)
                {
                    // UBOs larger than 64KB might be inefficient
                    return true; // We don't have size info in ResourceNode currently
                }
                return true;
            };
            AddCompatibilityRule(rule);
        }

        OLO_CORE_TRACE("Added default compatibility rules");
    }

    // Private methods implementation

    void ResourceValidator::AddValidationIssue(const ValidationIssue& issue)
    {
        m_LastValidationIssues.push_back(issue);
    }

    std::vector<ValidationIssue> ResourceValidator::ValidateResourceNode(const ResourceNode& node)
    {
        std::vector<ValidationIssue> issues;

        // Check if resource satisfies dependency rules
        if (!SatisfiesDependencyRules(node.Name))
        {
            ValidationIssue issue(ValidationSeverity::Error, ValidationCategory::DependencyGraph,
                                node.Name, "Resource violates dependency rules");
            const DependencyRule* rule = GetDependencyRule(node.Name);
            if (rule)
            {
                issue.Details = "Check required/forbidden dependencies, type compatibility, priority, and scope";
            }
            issues.push_back(issue);
        }

        // Run compatibility rules
        if (m_Context.ValidateCompatibility)
        {
            for (const auto& [ruleName, rule] : m_CompatibilityRules)
            {
                if (rule.ValidatorFunction)
                {
                    bool passed = rule.ValidatorFunction(node, m_Context);
                    if (!passed)
                    {
                        ValidationIssue issue(rule.FailureSeverity, ValidationCategory::Compatibility,
                                            node.Name, rule.FailureMessage);
                        issue.Details = "Failed compatibility rule: " + ruleName;
                        issues.push_back(issue);
                    }
                }
            }
        }

        return issues;
    }

    bool ResourceValidator::DetectCircularDependency(
        const std::unordered_map<std::string, std::unordered_set<std::string>>& dependencies,
        const std::string& node,
        std::unordered_set<std::string>& visited,
        std::unordered_set<std::string>& recursionStack,
        std::vector<std::string>& cycle)
    {
        visited.insert(node);
        recursionStack.insert(node);

        auto nodeIt = dependencies.find(node);
        if (nodeIt != dependencies.end())
        {
            for (const std::string& neighbor : nodeIt->second)
            {
                if (recursionStack.find(neighbor) != recursionStack.end())
                {
                    // Found back edge - record the cycle
                    cycle.clear();
                    cycle.push_back(neighbor);
                    cycle.push_back(node);
                    return true;
                }

                if (visited.find(neighbor) == visited.end())
                {
                    if (DetectCircularDependency(dependencies, neighbor, visited, recursionStack, cycle))
                    {
                        if (cycle.size() > 0 && cycle[0] != node)
                        {
                            cycle.push_back(node);
                        }
                        return true;
                    }
                }
            }
        }

        recursionStack.erase(node);
        return false;
    }

    std::vector<ValidationIssue> ResourceValidator::ValidateBindingPointUsage(
        const std::unordered_map<std::string, u32>& bindings,
        const std::string& resourceType)
    {
        return ValidateBindingConflicts(bindings);
    }

    bool ResourceValidator::AreTypesCompatible(ShaderResourceType type1, ShaderResourceType type2)
    {
        if (type1 == type2)
            return true;

        // Array types are compatible with their base types in some contexts
        if ((type1 == ShaderResourceType::UniformBuffer && type2 == ShaderResourceType::UniformBufferArray) ||
            (type1 == ShaderResourceType::UniformBufferArray && type2 == ShaderResourceType::UniformBuffer))
            return true;

        if ((type1 == ShaderResourceType::StorageBuffer && type2 == ShaderResourceType::StorageBufferArray) ||
            (type1 == ShaderResourceType::StorageBufferArray && type2 == ShaderResourceType::StorageBuffer))
            return true;

        if ((type1 == ShaderResourceType::Texture2D && type2 == ShaderResourceType::Texture2DArray) ||
            (type1 == ShaderResourceType::Texture2DArray && type2 == ShaderResourceType::Texture2D))
            return true;

        if ((type1 == ShaderResourceType::TextureCube && type2 == ShaderResourceType::TextureCubeArray) ||
            (type1 == ShaderResourceType::TextureCubeArray && type2 == ShaderResourceType::TextureCube))
            return true;

        return false;
    }

    std::vector<ValidationIssue> ResourceValidator::ValidateResourceScopeConsistency(
        const ResourceNode& node, const ResourceHierarchy& hierarchy)
    {
        std::vector<ValidationIssue> issues;

        // Check parent-child scope consistency
        if (!node.ParentName.empty())
        {
            const ResourceNode* parent = hierarchy.GetResource(node.ParentName);
            if (parent)
            {
                // Child scope should not be broader than parent scope
                if (static_cast<u32>(node.Scope) > static_cast<u32>(parent->Scope))
                {
                    ValidationIssue issue(ValidationSeverity::Warning, ValidationCategory::ResourceLifetime,
                                        node.Name, "Child resource has broader scope than parent");
                    issue.Details = "Child scope: " + std::string(ResourceHierarchy::GetScopeString(node.Scope)) +
                                   ", Parent scope: " + std::string(ResourceHierarchy::GetScopeString(parent->Scope));
                    issue.Suggestion = "Consider using the same scope as parent or narrower";
                    issues.push_back(issue);
                }
            }
        }

        return issues;
    }

    std::vector<ValidationIssue> ResourceValidator::AnalyzeResourcePerformance(const ResourceNode& node)
    {
        std::vector<ValidationIssue> issues;

        // Check for excessive dependencies
        if (node.Dependencies.size() > 10)
        {
            ValidationIssue issue(ValidationSeverity::Warning, ValidationCategory::Performance,
                                node.Name, "Resource has many dependencies");
            issue.Details = "Dependency count: " + std::to_string(node.Dependencies.size());
            issue.Suggestion = "Consider reducing dependencies or using batched resolution";
            issues.push_back(issue);
        }

        // Check for performance-critical priority mismatches
        if (node.Priority == ResourcePriority::System && node.Type != ShaderResourceType::UniformBuffer)
        {
            ValidationIssue issue(ValidationSeverity::Info, ValidationCategory::Performance,
                                node.Name, "System priority resource is not a uniform buffer");
            issue.Details = "System resources are typically uniform buffers for camera/view data";
            issue.Suggestion = "Consider using uniform buffer for system-level data";
            issues.push_back(issue);
        }

        return issues;
    }

    void ResourceValidator::StartValidationTiming()
    {
        m_LastValidationStart = std::chrono::steady_clock::now();
    }

    void ResourceValidator::EndValidationTiming()
    {
        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - m_LastValidationStart);
        m_LastValidationTimeMs = duration.count() / 1000.0f;
    }
}
