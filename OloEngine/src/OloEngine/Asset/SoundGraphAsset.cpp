#include "OloEnginePCH.h"
#include "SoundGraphAsset.h"

#include <algorithm>
#include <unordered_set>

namespace OloEngine
{
    bool SoundGraphAsset::HasNode(const UUID& nodeId) const
    {
        OLO_PROFILE_FUNCTION();

        for (const auto& node : m_Nodes)
        {
            if (node.ID == nodeId)
                return true;
        }
        return false;
    }

    SoundGraphNodeData* SoundGraphAsset::GetNode(const UUID& nodeId)
    {
        OLO_PROFILE_FUNCTION();
        
        for (auto& node : m_Nodes)
        {
            if (node.ID == nodeId)
                return &node;
        }
        return nullptr;
    }

    const SoundGraphNodeData* SoundGraphAsset::GetNode(const UUID& nodeId) const
    {
        OLO_PROFILE_FUNCTION();

        for (const auto& node : m_Nodes)
        {
            if (node.ID == nodeId)
                return &node;
        }
        return nullptr;
    }

    bool SoundGraphAsset::AddNode(const SoundGraphNodeData& node)
    {
        OLO_PROFILE_FUNCTION();

        // Ensure node ID is unique
        if (!HasNode(node.ID))
        {
            m_Nodes.push_back(node);
            return true;
        }
        return false;
    }

    bool SoundGraphAsset::RemoveNode(const UUID& nodeId)
    {
        OLO_PROFILE_FUNCTION();

        auto it = std::find_if(m_Nodes.begin(), m_Nodes.end(),
            [nodeId](const SoundGraphNodeData& node) { return node.ID == nodeId; });
        
        if (it != m_Nodes.end())
        {
            m_Nodes.erase(it);
            
            // Remove all connections involving this node
            m_Connections.erase(
                std::remove_if(m_Connections.begin(), m_Connections.end(),
                    [nodeId](const SoundGraphConnection& conn) {
                        return conn.SourceNodeID == nodeId || conn.TargetNodeID == nodeId;
                    }),
                m_Connections.end()
            );
            
            return true;
        }
        return false;
    }

    bool SoundGraphAsset::AddConnection(const SoundGraphConnection& connection)
    {
        OLO_PROFILE_FUNCTION();

        // Validate nodes exist
        if (HasNode(connection.SourceNodeID) && HasNode(connection.TargetNodeID))
        {
            m_Connections.push_back(connection);
            return true;
        }
        return false;
    }

    bool SoundGraphAsset::RemoveConnection(const UUID& sourceNodeId, const std::string& sourceEndpoint,
                                         const UUID& targetNodeId, const std::string& targetEndpoint,
                                         bool isEvent)
    {
        OLO_PROFILE_FUNCTION();

        auto it = std::find_if(m_Connections.begin(), m_Connections.end(),
            [&](const SoundGraphConnection& conn) {
                return conn.SourceNodeID == sourceNodeId &&
                       conn.SourceEndpoint == sourceEndpoint &&
                       conn.TargetNodeID == targetNodeId &&
                       conn.TargetEndpoint == targetEndpoint &&
                       conn.IsEvent == isEvent;
            });
        
        if (it != m_Connections.end())
        {
            m_Connections.erase(it);
            return true;
        }
        return false;
    }

    void SoundGraphAsset::Clear()
    {
        OLO_PROFILE_FUNCTION();

        m_Name.clear();
        m_Description.clear();
        m_Nodes.clear();
        m_Connections.clear();
        m_GraphInputs.clear();
        m_GraphOutputs.clear();
        m_LocalVariables.clear();
        // CompiledPrototype.Reset(); // TODO: Uncomment when SoundGraphPrototype is implemented
        m_WaveSources.clear();
    }

    bool SoundGraphAsset::IsValid() const
    {
        OLO_PROFILE_FUNCTION();

        // Basic validation
        if (m_Nodes.empty())
            return false;

        // Check for nodes with duplicate IDs
        std::unordered_set<UUID> nodeIds;
        for (const auto& node : m_Nodes)
        {
            if (nodeIds.find(node.ID) != nodeIds.end())
                return false; // Duplicate node ID found
            nodeIds.insert(node.ID);
        }

        // Validate all connections reference existing nodes
        for (const auto& connection : m_Connections)
        {
            if (!HasNode(connection.SourceNodeID) || !HasNode(connection.TargetNodeID))
                return false;
        }

        return true;
    }

    std::vector<std::string> SoundGraphAsset::GetValidationErrors() const
    {
        OLO_PROFILE_FUNCTION();
        
        std::vector<std::string> errors;

        if (m_Nodes.empty())
            errors.push_back("Sound graph has no nodes");

        // Check for nodes with duplicate IDs
        std::unordered_set<UUID> nodeIds;
        for (const auto& node : m_Nodes)
        {
            if (nodeIds.find(node.ID) != nodeIds.end())
                errors.push_back("Duplicate node ID: " + std::to_string(static_cast<u64>(node.ID)));
            nodeIds.insert(node.ID);
        }

        // Validate connections
        for (const auto& connection : m_Connections)
        {
            if (!HasNode(connection.SourceNodeID))
                errors.push_back("Connection references non-existent source node: " + std::to_string(static_cast<u64>(connection.SourceNodeID)));
            
            if (!HasNode(connection.TargetNodeID))
                errors.push_back("Connection references non-existent target node: " + std::to_string(static_cast<u64>(connection.TargetNodeID)));
        }

        return errors;
    }

} // namespace OloEngine