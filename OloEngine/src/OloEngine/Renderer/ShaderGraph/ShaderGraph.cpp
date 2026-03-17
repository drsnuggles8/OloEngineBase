#include "OloEnginePCH.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraph.h"

#include <algorithm>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace OloEngine
{
    // ─────────────────────────────────────────────────────────────
    //  Node management
    // ─────────────────────────────────────────────────────────────

    ShaderGraphNode* ShaderGraph::AddNode(Scope<ShaderGraphNode> node)
    {
        OLO_PROFILE_FUNCTION();

        if (!node)
            return nullptr;
        auto* ptr = node.get();
        m_Nodes.push_back(std::move(node));
        return ptr;
    }

    bool ShaderGraph::RemoveNode(UUID nodeID)
    {
        OLO_PROFILE_FUNCTION();

        // Remove all links connected to this node's pins
        auto* node = FindNode(nodeID);
        if (!node)
            return false;

        // Collect pin IDs from the node
        std::unordered_set<u64> pinIDs;
        for (const auto& pin : node->Inputs)
            pinIDs.insert(static_cast<u64>(pin.ID));
        for (const auto& pin : node->Outputs)
            pinIDs.insert(static_cast<u64>(pin.ID));

        // Remove links referencing those pins
        std::erase_if(m_Links, [&pinIDs](const ShaderGraphLink& link)
                      { return pinIDs.contains(static_cast<u64>(link.OutputPinID)) || pinIDs.contains(static_cast<u64>(link.InputPinID)); });

        // Remove the node
        std::erase_if(m_Nodes, [nodeID](const Scope<ShaderGraphNode>& n)
                      { return n->ID == nodeID; });
        return true;
    }

    ShaderGraphNode* ShaderGraph::FindNode(UUID nodeID)
    {
        for (auto& node : m_Nodes)
        {
            if (node->ID == nodeID)
                return node.get();
        }
        return nullptr;
    }

    const ShaderGraphNode* ShaderGraph::FindNode(UUID nodeID) const
    {
        for (const auto& node : m_Nodes)
        {
            if (node->ID == nodeID)
                return node.get();
        }
        return nullptr;
    }

    ShaderGraphPin* ShaderGraph::FindPin(UUID pinID)
    {
        for (auto& node : m_Nodes)
        {
            if (auto* pin = node->FindPin(pinID))
                return pin;
        }
        return nullptr;
    }

    const ShaderGraphPin* ShaderGraph::FindPin(UUID pinID) const
    {
        for (const auto& node : m_Nodes)
        {
            if (const auto* pin = node->FindPin(pinID))
                return pin;
        }
        return nullptr;
    }

    ShaderGraphNode* ShaderGraph::FindNodeByPinID(UUID pinID)
    {
        for (auto& node : m_Nodes)
        {
            if (node->FindPin(pinID))
                return node.get();
        }
        return nullptr;
    }

    const ShaderGraphNode* ShaderGraph::FindNodeByPinID(UUID pinID) const
    {
        for (const auto& node : m_Nodes)
        {
            if (node->FindPin(pinID))
                return node.get();
        }
        return nullptr;
    }

    // ─────────────────────────────────────────────────────────────
    //  Link management
    // ─────────────────────────────────────────────────────────────

    ShaderGraphLink* ShaderGraph::AddLink(UUID outputPinID, UUID inputPinID)
    {
        OLO_PROFILE_FUNCTION();

        // Validate pins exist
        const auto* outputPin = FindPin(outputPinID);
        const auto* inputPin = FindPin(inputPinID);
        if (!outputPin || !inputPin)
            return nullptr;

        // Validate directions
        if (outputPin->Direction != ShaderGraphPinDirection::Output || inputPin->Direction != ShaderGraphPinDirection::Input)
            return nullptr;

        // Validate type compatibility
        if (!CanConvertPinType(outputPin->Type, inputPin->Type))
            return nullptr;

        // Check for cycles
        if (WouldCreateCycle(outputPinID, inputPinID))
            return nullptr;

        // Remove any existing link to this input pin (each input has at most one connection)
        std::erase_if(m_Links, [inputPinID](const ShaderGraphLink& link)
                      { return link.InputPinID == inputPinID; });

        // Create the link
        m_Links.emplace_back(UUID(), outputPinID, inputPinID);
        return &m_Links.back();
    }

    bool ShaderGraph::RemoveLink(UUID linkID)
    {
        OLO_PROFILE_FUNCTION();

        auto it = std::find_if(m_Links.begin(), m_Links.end(),
                               [linkID](const ShaderGraphLink& link)
                               { return link.ID == linkID; });
        if (it == m_Links.end())
            return false;
        m_Links.erase(it);
        return true;
    }

    ShaderGraphLink* ShaderGraph::FindLink(UUID linkID)
    {
        for (auto& link : m_Links)
        {
            if (link.ID == linkID)
                return &link;
        }
        return nullptr;
    }

    const ShaderGraphLink* ShaderGraph::FindLink(UUID linkID) const
    {
        for (const auto& link : m_Links)
        {
            if (link.ID == linkID)
                return &link;
        }
        return nullptr;
    }

    const ShaderGraphLink* ShaderGraph::GetLinkForInputPin(UUID inputPinID) const
    {
        for (const auto& link : m_Links)
        {
            if (link.InputPinID == inputPinID)
                return &link;
        }
        return nullptr;
    }

    std::vector<const ShaderGraphLink*> ShaderGraph::GetLinksForOutputPin(UUID outputPinID) const
    {
        std::vector<const ShaderGraphLink*> result;
        for (const auto& link : m_Links)
        {
            if (link.OutputPinID == outputPinID)
                result.push_back(&link);
        }
        return result;
    }

    const ShaderGraphPin* ShaderGraph::GetConnectedOutputPin(UUID inputPinID) const
    {
        const auto* link = GetLinkForInputPin(inputPinID);
        if (!link)
            return nullptr;
        return FindPin(link->OutputPinID);
    }

    // ─────────────────────────────────────────────────────────────
    //  Graph analysis
    // ─────────────────────────────────────────────────────────────

    bool ShaderGraph::WouldCreateCycle(UUID outputPinID, UUID inputPinID) const
    {
        // Find the nodes
        const auto* outputNode = FindNodeByPinID(outputPinID);
        const auto* inputNode = FindNodeByPinID(inputPinID);
        if (!outputNode || !inputNode)
            return false;

        // Self-connection is a cycle
        if (outputNode->ID == inputNode->ID)
            return true;

        // BFS from the input node's outputs to see if we can reach the output node
        // If the output node is reachable from the input node, adding this link creates a cycle
        std::unordered_set<u64> visited;
        std::queue<u64> frontier;
        frontier.push(static_cast<u64>(inputNode->ID));

        while (!frontier.empty())
        {
            u64 currentID = frontier.front();
            frontier.pop();

            if (currentID == static_cast<u64>(outputNode->ID))
                return true;

            if (visited.contains(currentID))
                continue;
            visited.insert(currentID);

            // Find all nodes downstream of current
            const auto* currentNode = FindNode(UUID(currentID));
            if (!currentNode)
                continue;

            for (const auto& pin : currentNode->Outputs)
            {
                auto outputLinks = GetLinksForOutputPin(pin.ID);
                for (const auto* link : outputLinks)
                {
                    const auto* downstreamNode = FindNodeByPinID(link->InputPinID);
                    if (downstreamNode && !visited.contains(static_cast<u64>(downstreamNode->ID)))
                        frontier.push(static_cast<u64>(downstreamNode->ID));
                }
            }
        }
        return false;
    }

    ShaderGraphValidationResult ShaderGraph::Validate() const
    {
        OLO_PROFILE_FUNCTION();

        ShaderGraphValidationResult result;

        // Check for exactly one output node (PBROutput or ComputeOutput)
        const ShaderGraphNode* outputNode = nullptr;
        int pbrCount = 0;
        int computeCount = 0;
        for (const auto& node : m_Nodes)
        {
            if (node->TypeName == ShaderGraphNodeTypes::PBROutput)
            {
                outputNode = node.get();
                ++pbrCount;
            }
            else if (node->TypeName == ShaderGraphNodeTypes::ComputeOutput)
            {
                outputNode = node.get();
                ++computeCount;
            }
        }

        int outputCount = pbrCount + computeCount;
        if (outputCount == 0)
        {
            result.IsValid = false;
            result.Errors.push_back("Graph must have exactly one output node (PBROutput or ComputeOutput)");
        }
        else if (outputCount > 1)
        {
            result.IsValid = false;
            result.Errors.push_back("Graph must have exactly one output node, found " + std::to_string(outputCount));
        }

        // Check for cycles via topological sort
        auto order = GetTopologicalOrder();
        if (order.empty() && !m_Nodes.empty())
        {
            result.IsValid = false;
            result.Errors.push_back("Graph contains a cycle");
        }

        // Warn about disconnected required inputs on the output node
        if (outputNode && outputNode->TypeName == ShaderGraphNodeTypes::PBROutput)
        {
            const auto* albedoPin = outputNode->FindPinByName("Albedo", ShaderGraphPinDirection::Input);
            if (albedoPin && !GetLinkForInputPin(albedoPin->ID))
                result.Warnings.push_back("PBROutput 'Albedo' input is not connected, will use default value");
        }

        // Check for parameter name uniqueness
        std::unordered_map<std::string, int> paramNames;
        for (const auto& node : m_Nodes)
        {
            if (!node->ParameterName.empty())
            {
                paramNames[node->ParameterName]++;
            }
        }
        for (const auto& [name, count] : paramNames)
        {
            if (count > 1)
            {
                result.IsValid = false;
                result.Errors.push_back("Duplicate parameter name: '" + name + "' used by " + std::to_string(count) + " nodes");
            }
        }

        return result;
    }

    std::vector<const ShaderGraphNode*> ShaderGraph::GetTopologicalOrder() const
    {
        OLO_PROFILE_FUNCTION();

        if (m_Nodes.empty())
            return {};

        // Build adjacency: for each node, which nodes depend on it (downstream)
        std::unordered_map<u64, std::vector<u64>> adjacency;
        std::unordered_map<u64, int> inDegree;

        for (const auto& node : m_Nodes)
        {
            u64 id = static_cast<u64>(node->ID);
            if (!inDegree.contains(id))
                inDegree[id] = 0;
        }

        // Build edges: outputNode → inputNode
        for (const auto& link : m_Links)
        {
            const auto* outputNode = FindNodeByPinID(link.OutputPinID);
            const auto* inputNode = FindNodeByPinID(link.InputPinID);
            if (!outputNode || !inputNode)
                continue;

            u64 fromID = static_cast<u64>(outputNode->ID);
            u64 toID = static_cast<u64>(inputNode->ID);
            adjacency[fromID].push_back(toID);
            inDegree[toID]++;
        }

        // Kahn's algorithm
        std::queue<u64> frontier;
        for (const auto& [id, degree] : inDegree)
        {
            if (degree == 0)
                frontier.push(id);
        }

        std::vector<const ShaderGraphNode*> sorted;
        sorted.reserve(m_Nodes.size());

        while (!frontier.empty())
        {
            u64 current = frontier.front();
            frontier.pop();

            const auto* node = FindNode(UUID(current));
            if (node)
                sorted.push_back(node);

            if (auto it = adjacency.find(current); it != adjacency.end())
            {
                for (u64 downstream : it->second)
                {
                    if (--inDegree[downstream] == 0)
                        frontier.push(downstream);
                }
            }
        }

        // If we didn't visit all nodes, there's a cycle
        if (sorted.size() != m_Nodes.size())
            return {};

        return sorted;
    }

    const ShaderGraphNode* ShaderGraph::FindOutputNode() const
    {
        for (const auto& node : m_Nodes)
        {
            if (node->TypeName == ShaderGraphNodeTypes::PBROutput ||
                node->TypeName == ShaderGraphNodeTypes::ComputeOutput)
                return node.get();
        }
        return nullptr;
    }

} // namespace OloEngine
