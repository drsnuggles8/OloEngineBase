#include "OloEnginePCH.h"
#include "SoundGraphAsset.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphPrototype.h"

#include <algorithm>
#include <unordered_set>

namespace OloEngine
{
    bool SoundGraphAsset::HasNode(const UUID& nodeId) const
    {
        OLO_PROFILE_FUNCTION();
        return m_NodeIdMap.find(nodeId) != m_NodeIdMap.end();
    }

    SoundGraphNodeData* SoundGraphAsset::GetNode(const UUID& nodeId)
    {
        OLO_PROFILE_FUNCTION();
        
        // Debug validation: ensure cache is consistent with vector
        OLO_CORE_ASSERT(m_NodeIdMap.size() == m_Nodes.size(), 
                        "Node ID map out of sync with nodes vector - did you modify m_Nodes directly without calling RebuildNodeIdMap()?");
        
        auto it = m_NodeIdMap.find(nodeId);
        if (it != m_NodeIdMap.end())
        {
            OLO_CORE_ASSERT(it->second < m_Nodes.size(), "Node ID map contains invalid index");
            return &m_Nodes[it->second];
        }
        return nullptr;
    }

    const SoundGraphNodeData* SoundGraphAsset::GetNode(const UUID& nodeId) const
    {
        OLO_PROFILE_FUNCTION();

        // Debug validation: ensure cache is consistent with vector
        OLO_CORE_ASSERT(m_NodeIdMap.size() == m_Nodes.size(), 
                        "Node ID map out of sync with nodes vector - did you modify m_Nodes directly without calling RebuildNodeIdMap()?");
        
        auto it = m_NodeIdMap.find(nodeId);
        if (it != m_NodeIdMap.end())
        {
            OLO_CORE_ASSERT(it->second < m_Nodes.size(), "Node ID map contains invalid index");
            return &m_Nodes[it->second];
        }
        return nullptr;
    }

    bool SoundGraphAsset::AddNode(const SoundGraphNodeData& node)
    {
        OLO_PROFILE_FUNCTION();

        // Ensure node ID is unique
        if (!HasNode(node.m_ID))
        {
            sizet index = m_Nodes.size();
            m_Nodes.push_back(node);
            m_NodeIdMap[node.m_ID] = index;
            return true;
        }
        return false;
    }

    bool SoundGraphAsset::RemoveNode(const UUID& nodeId)
    {
        OLO_PROFILE_FUNCTION();

        auto mapIt = m_NodeIdMap.find(nodeId);
        if (mapIt == m_NodeIdMap.end())
            return false;

        sizet indexToRemove = mapIt->second;
        
        // Erase from the vector
        m_Nodes.erase(m_Nodes.begin() + indexToRemove);
        
        // Remove from the map
        m_NodeIdMap.erase(mapIt);
        
        // Update indices in the map for all nodes after the removed one
        for (auto& [id, index] : m_NodeIdMap)
        {
            if (index > indexToRemove)
                index--;
        }
        
        // Remove all connections involving this node
        m_Connections.erase(
            std::remove_if(m_Connections.begin(), m_Connections.end(),
                [nodeId](const SoundGraphConnection& conn) {
                    return conn.m_SourceNodeID == nodeId || conn.m_TargetNodeID == nodeId;
                }),
            m_Connections.end()
        );
        
        return true;
    }

    bool SoundGraphAsset::AddConnection(const SoundGraphConnection& connection)
    {
        OLO_PROFILE_FUNCTION();

        // Validate nodes exist
        if (HasNode(connection.m_SourceNodeID) && HasNode(connection.m_TargetNodeID))
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
                return conn.m_SourceNodeID == sourceNodeId &&
                       conn.m_SourceEndpoint == sourceEndpoint &&
                       conn.m_TargetNodeID == targetNodeId &&
                       conn.m_TargetEndpoint == targetEndpoint &&
                       conn.m_IsEvent == isEvent;
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
        m_NodeIdMap.clear();
        m_Connections.clear();
        m_GraphInputs.clear();
        m_GraphOutputs.clear();
        m_LocalVariables.clear();
        m_CompiledPrototype.Reset();
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
            if (nodeIds.find(node.m_ID) != nodeIds.end())
                return false; // Duplicate node ID found
            nodeIds.insert(node.m_ID);
        }

        // Validate all connections reference existing nodes
        for (const auto& connection : m_Connections)
        {
            if (nodeIds.find(connection.m_SourceNodeID) == nodeIds.end() || 
                nodeIds.find(connection.m_TargetNodeID) == nodeIds.end())
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
            if (nodeIds.find(node.m_ID) != nodeIds.end())
                errors.push_back("Duplicate node ID: " + std::to_string(static_cast<u64>(node.m_ID)));
            nodeIds.insert(node.m_ID);
        }

        // Validate connections
        for (const auto& connection : m_Connections)
        {
            if (!HasNode(connection.m_SourceNodeID))
                errors.push_back("Connection references non-existent source node: " + std::to_string(static_cast<u64>(connection.m_SourceNodeID)));
            
            if (!HasNode(connection.m_TargetNodeID))
                errors.push_back("Connection references non-existent target node: " + std::to_string(static_cast<u64>(connection.m_TargetNodeID)));
        }

        return errors;
    }

    void SoundGraphAsset::RebuildNodeIdMap()
    {
        OLO_PROFILE_FUNCTION();
        
        m_NodeIdMap.clear();
        for (sizet i = 0; i < m_Nodes.size(); ++i)
        {
            m_NodeIdMap[m_Nodes[i].m_ID] = i;
        }
    }

    const Ref<Audio::SoundGraph::Prototype>& SoundGraphAsset::GetCompiledPrototype() const
    {
        return m_CompiledPrototype;
    }

    void SoundGraphAsset::SetCompiledPrototype(const Ref<Audio::SoundGraph::Prototype>& prototype)
    {
        m_CompiledPrototype = prototype;
    }

} // namespace OloEngine