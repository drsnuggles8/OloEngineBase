#include "OloEnginePCH.h"
#include "SoundGraphAsset.h"

#include <algorithm>
#include <unordered_set>

namespace OloEngine
{
    bool SoundGraphAsset::HasNode(const UUID& nodeId) const
    {
        for (const auto& node : Nodes)
        {
            if (node.ID == nodeId)
                return true;
        }
        return false;
    }

    SoundGraphNodeData* SoundGraphAsset::GetNode(const UUID& nodeId)
    {
        for (auto& node : Nodes)
        {
            if (node.ID == nodeId)
                return &node;
        }
        return nullptr;
    }

    const SoundGraphNodeData* SoundGraphAsset::GetNode(const UUID& nodeId) const
    {
        for (const auto& node : Nodes)
        {
            if (node.ID == nodeId)
                return &node;
        }
        return nullptr;
    }

    bool SoundGraphAsset::AddNode(const SoundGraphNodeData& node)
    {
        // Ensure node ID is unique
        if (!HasNode(node.ID))
        {
            Nodes.push_back(node);
            return true;
        }
        return false;
    }

    bool SoundGraphAsset::RemoveNode(const UUID& nodeId)
    {
        auto it = std::find_if(Nodes.begin(), Nodes.end(),
            [nodeId](const SoundGraphNodeData& node) { return node.ID == nodeId; });
        
        if (it != Nodes.end())
        {
            Nodes.erase(it);
            
            // Remove all connections involving this node
            Connections.erase(
                std::remove_if(Connections.begin(), Connections.end(),
                    [nodeId](const SoundGraphConnection& conn) {
                        return conn.SourceNodeID == nodeId || conn.TargetNodeID == nodeId;
                    }),
                Connections.end()
            );
            
            return true;
        }
        return false;
    }

    bool SoundGraphAsset::AddConnection(const SoundGraphConnection& connection)
    {
        // Validate nodes exist
        if (HasNode(connection.SourceNodeID) && HasNode(connection.TargetNodeID))
        {
            Connections.push_back(connection);
            return true;
        }
        return false;
    }

    bool SoundGraphAsset::RemoveConnection(const UUID& sourceNodeId, const std::string& sourceEndpoint,
                                         const UUID& targetNodeId, const std::string& targetEndpoint,
                                         bool isEvent)
    {
        auto it = std::find_if(Connections.begin(), Connections.end(),
            [&](const SoundGraphConnection& conn) {
                return conn.SourceNodeID == sourceNodeId &&
                       conn.SourceEndpoint == sourceEndpoint &&
                       conn.TargetNodeID == targetNodeId &&
                       conn.TargetEndpoint == targetEndpoint &&
                       conn.IsEvent == isEvent;
            });
        
        if (it != Connections.end())
        {
            Connections.erase(it);
            return true;
        }
        return false;
    }

    void SoundGraphAsset::Clear()
    {
        Name.clear();
        Description.clear();
        Nodes.clear();
        Connections.clear();
        GraphInputs.clear();
        GraphOutputs.clear();
        LocalVariables.clear();
        // CompiledPrototype.Reset(); // TODO: Uncomment when SoundGraphPrototype is implemented
        m_WaveSources.clear();
    }

    bool SoundGraphAsset::IsValid() const
    {
        // Basic validation
        if (Nodes.empty())
            return false;

        // Check for nodes with duplicate IDs
        std::unordered_set<UUID> nodeIds;
        for (const auto& node : Nodes)
        {
            if (nodeIds.find(node.ID) != nodeIds.end())
                return false; // Duplicate node ID found
            nodeIds.insert(node.ID);
        }

        // Validate all connections reference existing nodes
        for (const auto& connection : Connections)
        {
            if (!HasNode(connection.SourceNodeID) || !HasNode(connection.TargetNodeID))
                return false;
        }

        return true;
    }

    std::vector<std::string> SoundGraphAsset::GetValidationErrors() const
    {
        std::vector<std::string> errors;

        if (Nodes.empty())
            errors.push_back("Sound graph has no nodes");

        // Check for nodes with duplicate IDs
        std::unordered_set<UUID> nodeIds;
        for (const auto& node : Nodes)
        {
            if (nodeIds.find(node.ID) != nodeIds.end())
                errors.push_back("Duplicate node ID: " + std::to_string(static_cast<u64>(node.ID)));
            nodeIds.insert(node.ID);
        }

        // Validate connections
        for (const auto& connection : Connections)
        {
            if (!HasNode(connection.SourceNodeID))
                errors.push_back("Connection references non-existent source node: " + std::to_string(static_cast<u64>(connection.SourceNodeID)));
            
            if (!HasNode(connection.TargetNodeID))
                errors.push_back("Connection references non-existent target node: " + std::to_string(static_cast<u64>(connection.TargetNodeID)));
        }

        return errors;
    }

} // namespace OloEngine