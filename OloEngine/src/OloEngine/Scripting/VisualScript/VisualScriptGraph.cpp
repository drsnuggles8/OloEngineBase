#include "OloEnginePCH.h"
#include "VisualScriptGraph.h"

#include <algorithm>

namespace OloEngine::VisualScript
{
    const std::string& VisualScriptNode::GetProperty(std::string_view key, const std::string& fallback) const
    {
        // Heterogeneous lookup — m_Properties is keyed with std::less<> precisely
        // so this takes the string_view directly instead of materialising a
        // temporary std::string on every property read (SonarQube cpp:S6045).
        const auto it = m_Properties.find(key);
        return it == m_Properties.end() ? fallback : it->second;
    }

    void VisualScriptNode::SetProperty(std::string key, std::string value)
    {
        m_Properties[std::move(key)] = std::move(value);
    }

    VisualScriptNode& VisualScriptGraph::AddNode(std::string typeName, glm::vec2 position)
    {
        VisualScriptNode& node = m_Nodes.emplace_back();
        node.m_Id = m_NextNodeId++;
        node.m_TypeName = std::move(typeName);
        node.m_Position = position;
        return node;
    }

    LinkId VisualScriptGraph::AddLink(NodeId sourceNode, std::string sourcePin, NodeId targetNode, std::string targetPin)
    {
        VisualScriptLink& link = m_Links.emplace_back();
        link.m_Id = m_NextLinkId++;
        link.m_SourceNode = sourceNode;
        link.m_SourcePin = std::move(sourcePin);
        link.m_TargetNode = targetNode;
        link.m_TargetPin = std::move(targetPin);
        return link.m_Id;
    }

    bool VisualScriptGraph::RemoveNode(NodeId id)
    {
        const auto before = m_Nodes.size();
        std::erase_if(m_Nodes, [id](const VisualScriptNode& node)
                      { return node.m_Id == id; });
        if (m_Nodes.size() == before)
        {
            return false;
        }
        // Drop the wires too — a link to a deleted node is exactly the dangling
        // endpoint the compiler would reject, and leaving it behind turns a delete
        // into a load failure later.
        std::erase_if(m_Links, [id](const VisualScriptLink& link)
                      { return link.m_SourceNode == id || link.m_TargetNode == id; });
        return true;
    }

    bool VisualScriptGraph::RemoveLink(LinkId id)
    {
        const auto before = m_Links.size();
        std::erase_if(m_Links, [id](const VisualScriptLink& link)
                      { return link.m_Id == id; });
        return m_Links.size() != before;
    }

    VisualScriptNode* VisualScriptGraph::FindNode(NodeId id)
    {
        const auto it = std::ranges::find(m_Nodes, id, &VisualScriptNode::m_Id);
        return it == m_Nodes.end() ? nullptr : &(*it);
    }

    const VisualScriptNode* VisualScriptGraph::FindNode(NodeId id) const
    {
        const auto it = std::ranges::find(m_Nodes, id, &VisualScriptNode::m_Id);
        return it == m_Nodes.end() ? nullptr : &(*it);
    }

    const VisualScriptGraph* VisualScriptAsset::FindFunction(std::string_view name) const
    {
        const auto it = std::ranges::find(m_Functions, name, &VisualScriptGraph::m_Name);
        return it == m_Functions.end() ? nullptr : &(*it);
    }

    const VisualScriptVariable* VisualScriptAsset::FindVariable(std::string_view name) const
    {
        const auto it = std::ranges::find(m_Variables, name, &VisualScriptVariable::m_Name);
        return it == m_Variables.end() ? nullptr : &(*it);
    }

    bool VisualScriptAsset::EqualsForTest(const VisualScriptAsset& other) const
    {
        return m_EventGraph == other.m_EventGraph && m_Functions == other.m_Functions && m_Variables == other.m_Variables && m_NodeBudgetPerTick == other.m_NodeBudgetPerTick;
    }

} // namespace OloEngine::VisualScript
