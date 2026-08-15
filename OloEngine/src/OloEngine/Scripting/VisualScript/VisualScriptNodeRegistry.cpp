#include "OloEnginePCH.h"
#include "VisualScriptNodeRegistry.h"

#include "OloEngine/Core/Log.h"

#include <algorithm>
#include <mutex>

namespace OloEngine::VisualScript
{
    NodeRegistry& NodeRegistry::Get()
    {
        static NodeRegistry s_Instance;
        return s_Instance;
    }

    bool NodeRegistry::Register(NodeTypeDescriptor descriptor)
    {
        if (descriptor.m_TypeName.empty())
        {
            OLO_CORE_ERROR("[VisualScript] Refusing to register a node type with an empty type name");
            return false;
        }
        if (!descriptor.m_Evaluate && !descriptor.m_Execute)
        {
            OLO_CORE_ERROR("[VisualScript] Node type '{}' has neither an Evaluate nor an Execute body", descriptor.m_TypeName);
            return false;
        }
        if (descriptor.m_DisplayName.empty())
        {
            descriptor.m_DisplayName = descriptor.m_TypeName;
        }
        std::string key = descriptor.m_TypeName;
        m_Types.insert_or_assign(std::move(key), std::move(descriptor));
        m_SortedDirty = true;
        return true;
    }

    const NodeTypeDescriptor* NodeRegistry::Find(std::string_view typeName) const
    {
        const auto it = m_Types.find(typeName);
        return it == m_Types.end() ? nullptr : &it->second;
    }

    const std::vector<const NodeTypeDescriptor*>& NodeRegistry::GetSorted() const
    {
        if (m_SortedDirty)
        {
            m_Sorted.clear();
            m_Sorted.reserve(m_Types.size());
            for (const auto& [name, descriptor] : m_Types)
            {
                m_Sorted.push_back(&descriptor);
            }
            std::ranges::sort(m_Sorted, [](const NodeTypeDescriptor* a, const NodeTypeDescriptor* b)
                              {
                                  if (a->m_Category != b->m_Category)
                                  {
                                      return a->m_Category < b->m_Category;
                                  }
                                  return a->m_DisplayName < b->m_DisplayName; });
            m_SortedDirty = false;
        }
        return m_Sorted;
    }

    void NodeRegistry::EnsureStandardLibrary()
    {
        // std::call_once rather than a plain bool: the asset serializer can be
        // driven from the asset-system worker thread while the game thread is
        // compiling a graph, and a half-populated registry would look like a
        // missing node type (a hard compile error) rather than a race.
        static std::once_flag s_Once;
        std::call_once(s_Once, []
                       {
            NodeRegistry& registry = Get();
            RegisterEventNodes(registry);
            RegisterFlowNodes(registry);
            RegisterMathNodes(registry);
            RegisterVariableNodes(registry);
            RegisterEntityNodes(registry);
            RegisterUtilityNodes(registry);
            RegisterFunctionNodes(registry);
            RegisterScriptBridgeNodes(registry);
            OLO_CORE_INFO("[VisualScript] Standard node library registered: {} node types", registry.GetCount()); });
    }

} // namespace OloEngine::VisualScript
