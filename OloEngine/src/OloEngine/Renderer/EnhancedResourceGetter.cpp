#include "OloEnginePCH.h"
#include "EnhancedResourceGetter.h"
#include "ResourceConverter.h"
#include "Platform/OpenGL/OpenGLResourceDeclaration.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine
{
    bool ResourceAvailabilityChecker::IsConvertibleType(ShaderResourceType from, ShaderResourceType to)
    {
        // Same type is always convertible
        if (from == to)
            return true;
        
        // Check specific conversion rules
        switch (from)
        {
            case ShaderResourceType::UniformBuffer:
                return to == ShaderResourceType::UniformBufferArray;
                
            case ShaderResourceType::StorageBuffer:
                return to == ShaderResourceType::StorageBufferArray;
                
            case ShaderResourceType::Texture2D:
                return to == ShaderResourceType::Texture2DArray;
                
            case ShaderResourceType::TextureCube:
                return to == ShaderResourceType::TextureCubeArray;
                
            default:
                return false;
        }
    }

    bool EnhancedResourceGetter::CanConvertWithDeclaration(
        const OpenGLResourceDeclaration::ResourceInfo& resourceInfo,
        ShaderResourceType targetType)
    {
        // Same type is always convertible
        if (resourceInfo.Type == targetType)
            return true;
        
        // Check array conversions with declaration metadata
        if (resourceInfo.IsArray)
        {
            // Array to non-array conversion (get first element)
            switch (resourceInfo.Type)
            {
                case ShaderResourceType::UniformBufferArray:
                    return targetType == ShaderResourceType::UniformBuffer;
                case ShaderResourceType::StorageBufferArray:
                    return targetType == ShaderResourceType::StorageBuffer;
                case ShaderResourceType::Texture2DArray:
                    return targetType == ShaderResourceType::Texture2D;
                case ShaderResourceType::TextureCubeArray:
                    return targetType == ShaderResourceType::TextureCube;
                default:
                    break;
            }
        }
        else
        {
            // Non-array to array conversion
            switch (resourceInfo.Type)
            {
                case ShaderResourceType::UniformBuffer:
                    return targetType == ShaderResourceType::UniformBufferArray;
                case ShaderResourceType::StorageBuffer:
                    return targetType == ShaderResourceType::StorageBufferArray;
                case ShaderResourceType::Texture2D:
                    return targetType == ShaderResourceType::Texture2DArray;
                case ShaderResourceType::TextureCube:
                    return targetType == ShaderResourceType::TextureCubeArray;
                default:
                    break;
            }
        }
        
        return false;
    }

    const char* EnhancedResourceGetter::GetAccessPatternName(OpenGLResourceDeclaration::AccessPattern pattern)
    {
        switch (pattern)
        {
            case OpenGLResourceDeclaration::AccessPattern::ReadOnly: return "ReadOnly";
            case OpenGLResourceDeclaration::AccessPattern::WriteOnly: return "WriteOnly";
            case OpenGLResourceDeclaration::AccessPattern::ReadWrite: return "ReadWrite";
            case OpenGLResourceDeclaration::AccessPattern::Static: return "Static";
            case OpenGLResourceDeclaration::AccessPattern::Dynamic: return "Dynamic";
            case OpenGLResourceDeclaration::AccessPattern::Streaming: return "Streaming";
            default: return "Unknown";
        }
    }

    const char* EnhancedResourceGetter::GetUsageFrequencyName(OpenGLResourceDeclaration::UsageFrequency frequency)
    {
        switch (frequency)
        {
            case OpenGLResourceDeclaration::UsageFrequency::Never: return "Never";
            case OpenGLResourceDeclaration::UsageFrequency::Rare: return "Rare";
            case OpenGLResourceDeclaration::UsageFrequency::Normal: return "Normal";
            case OpenGLResourceDeclaration::UsageFrequency::Frequent: return "Frequent";
            case OpenGLResourceDeclaration::UsageFrequency::Constant: return "Constant";
            default: return "Unknown";
        }
    }

    ResourceInfoExtended EnhancedResourceGetter::GetResourceInfo(const UniformBufferRegistry& registry,
                                                               const std::string& name,
                                                               const std::string& passName)
    {
        ResourceInfoExtended info;
        info.Name = name;
        
        // Get registry information
        const auto* binding = registry.GetResourceBinding(name);
        if (binding)
        {
            info.Type = binding->Type;
            info.RegistryBinding = binding->BindingPoint;
            info.RegistrySet = binding->Set;
            info.IsArray = binding->IsArray;
            info.ArraySize = binding->ArraySize;
            info.IsBound = registry.IsResourceBound(name);
        }
        
        // Get declaration information if available
        const auto* declaration = registry.GetOpenGLDeclaration(passName);
        if (declaration)
        {
            const auto* resourceInfo = declaration->GetResource(name);
            if (resourceInfo)
            {
                info.HasDeclaration = true;
                info.DeclarationBinding = resourceInfo->Binding;
                info.DeclarationSet = resourceInfo->Set;
                info.EstimatedSize = resourceInfo->EstimatedMemoryUsage;
                info.IsOptional = resourceInfo->IsOptional;
                info.AccessPattern = GetAccessPatternName(resourceInfo->Access);
                info.UsageFrequency = GetUsageFrequencyName(resourceInfo->Frequency);
                
                // Validate compatibility
                if (binding)
                {
                    if (binding->Type != resourceInfo->Type)
                    {
                        if (!CanConvertWithDeclaration(*resourceInfo, binding->Type))
                        {
                            info.IsCompatible = false;
                            info.Issues.push_back("Type mismatch: registry has " + 
                                                std::string(ResourceAvailabilityChecker::GetResourceTypeName(binding->Type)) +
                                                ", declaration expects " + 
                                                std::string(ResourceAvailabilityChecker::GetResourceTypeName(resourceInfo->Type)));
                        }
                    }
                    
                    if (binding->BindingPoint != resourceInfo->Binding)
                    {
                        info.Issues.push_back("Binding mismatch: registry uses " + 
                                            std::to_string(binding->BindingPoint) +
                                            ", declaration expects " + 
                                            std::to_string(resourceInfo->Binding));
                    }
                    
                    if (binding->Set != resourceInfo->Set)
                    {
                        info.Issues.push_back("Set mismatch: registry uses " + 
                                            std::to_string(binding->Set) +
                                            ", declaration expects " + 
                                            std::to_string(resourceInfo->Set));
                    }
                }
                else
                {
                    info.Issues.push_back("Resource declared but not found in registry");
                }
            }
        }
        
        // Final validation
        info.IsValid = info.IsBound && (info.Issues.empty() || info.IsOptional);
        
        return info;
    }



    std::vector<ResourceValidationIssue> EnhancedResourceGetter::ValidateResourcesAgainstDeclarations(
        const UniformBufferRegistry& registry,
        const std::string& passName)
    {
        std::vector<ResourceValidationIssue> issues;
        
        const auto* declaration = registry.GetOpenGLDeclaration(passName);
        if (!declaration)
        {
            ResourceValidationIssue issue(
                ResourceValidationIssue::Severity::Warning,
                "",
                "MissingDeclaration",
                "No OpenGL declaration found for pass '" + passName + "'"
            );
            issue.PassName = passName;
            issues.push_back(issue);
            return issues;
        }
        
        const auto& declarationData = declaration->GetDeclaration();
        
        // Check each resource in the declaration
        for (const auto& resource : declarationData.Resources)
        {
            auto info = GetResourceInfo(registry, resource.Name, passName);
            
            // Convert issues to validation issues
            for (const auto& issueText : info.Issues)
            {
                ResourceValidationIssue::Severity severity = ResourceValidationIssue::Severity::Warning;
                
                // Determine severity based on issue type
                if (issueText.find("Type mismatch") != std::string::npos)
                {
                    severity = resource.IsOptional ? ResourceValidationIssue::Severity::Warning 
                                                  : ResourceValidationIssue::Severity::Error;
                }
                else if (issueText.find("not found") != std::string::npos)
                {
                    severity = resource.IsOptional ? ResourceValidationIssue::Severity::Info 
                                                  : ResourceValidationIssue::Severity::Error;
                }
                else if (issueText.find("mismatch") != std::string::npos)
                {
                    severity = ResourceValidationIssue::Severity::Warning;
                }
                
                ResourceValidationIssue issue(
                    severity,
                    resource.Name,
                    "DeclarationMismatch",
                    issueText
                );
                issue.PassName = passName;
                issue.ExpectedBinding = resource.Binding;
                issue.ActualBinding = info.RegistryBinding;
                issue.ExpectedSet = resource.Set;
                issue.ActualSet = info.RegistrySet;
                
                // Add suggestions
                if (issueText.find("Type mismatch") != std::string::npos)
                {
                    issue.Suggestion = "Consider using smart conversion or rebinding with correct type";
                }
                else if (issueText.find("Binding mismatch") != std::string::npos)
                {
                    issue.Suggestion = "Update binding point in shader or registry to match declaration";
                }
                else if (issueText.find("Set mismatch") != std::string::npos)
                {
                    issue.Suggestion = "Update descriptor set assignment to match declaration";
                }
                else if (issueText.find("not found") != std::string::npos)
                {
                    issue.Suggestion = "Bind the resource using registry.SetResource() or mark as optional";
                }
                
                issues.push_back(issue);
            }
        }
        
        // Check for resources in registry that are not in declaration
        const auto& registryBindings = registry.GetBindings();
        for (const auto& [name, binding] : registryBindings)
        {
            if (!declaration->HasResource(name))
            {
                ResourceValidationIssue issue(
                    ResourceValidationIssue::Severity::Info,
                    name,
                    "ExtraResource",
                    "Resource '" + name + "' is bound in registry but not declared in OpenGL declaration"
                );
                issue.PassName = passName;
                issue.ActualBinding = binding.BindingPoint;
                issue.ActualSet = binding.Set;
                issue.Suggestion = "Remove unused resource binding or add to declaration if intended";
                issues.push_back(issue);
            }
        }
        
        OLO_CORE_INFO("Validated {} resources against declaration '{}', found {} issues", 
                     declarationData.Resources.size(), passName, issues.size());
        
        return issues;
    }
}
