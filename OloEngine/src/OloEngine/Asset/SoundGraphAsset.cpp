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

        if (auto it = m_NodeIdMap.find(nodeId); it != m_NodeIdMap.end())
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

        if (auto it = m_NodeIdMap.find(nodeId); it != m_NodeIdMap.end())
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
                           [nodeId](const SoundGraphConnection& conn)
                           {
                               return conn.m_SourceNodeID == nodeId || conn.m_TargetNodeID == nodeId;
                           }),
            m_Connections.end());

        return true;
    }

    bool SoundGraphAsset::AddConnection(const SoundGraphConnection& connection)
    {
        OLO_PROFILE_FUNCTION();

        // Endpoint must either resolve to a real node, or be the graph-IO sentinel
        // (UUID(0) on the source side = "from graph input", UUID(0) on the target side =
        // "to graph output"). Rejecting UUID(0) here would diverge from the serializer
        // path — which simply appends to m_Connections — and silently drop every
        // node↔graph-IO wire the user can draw in the editor.
        auto endpointResolves = [this](const UUID& id)
        {
            return id == UUID(0) || HasNode(id);
        };
        if (endpointResolves(connection.m_SourceNodeID) && endpointResolves(connection.m_TargetNodeID))
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

        auto it = std::ranges::find_if(m_Connections,
                                       [&](const SoundGraphConnection& conn)
                                       {
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

        // Validate all connections reference existing nodes. UUID(0) is the graph-IO
        // sentinel — source UUID(0) means "from graph input", target UUID(0) means "to
        // graph output". AddConnection accepts these explicitly (see endpointResolves);
        // treat them as valid here too so editor-drawable IO wires don't make the asset
        // report itself invalid.
        for (const auto& connection : m_Connections)
        {
            const bool sourceOk = connection.m_SourceNodeID == UUID(0) ||
                                  nodeIds.find(connection.m_SourceNodeID) != nodeIds.end();
            const bool targetOk = connection.m_TargetNodeID == UUID(0) ||
                                  nodeIds.find(connection.m_TargetNodeID) != nodeIds.end();
            if (!sourceOk || !targetOk)
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

        // Validate connections. UUID(0) is the graph-IO sentinel — see the matching
        // comment in IsValid() above. Don't flag it as a missing node.
        for (const auto& connection : m_Connections)
        {
            if (connection.m_SourceNodeID != UUID(0) && !HasNode(connection.m_SourceNodeID))
                errors.push_back("Connection references non-existent source node: " + std::to_string(static_cast<u64>(connection.m_SourceNodeID)));

            if (connection.m_TargetNodeID != UUID(0) && !HasNode(connection.m_TargetNodeID))
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

    Ref<SoundGraphAsset> SoundGraphAsset::Clone() const
    {
        OLO_PROFILE_FUNCTION();

        auto clone = Ref<SoundGraphAsset>::Create();
        clone->m_Name = m_Name;
        clone->m_Description = m_Description;
        clone->m_Nodes = m_Nodes;
        clone->m_Connections = m_Connections;
        clone->m_GraphInputs = m_GraphInputs;
        clone->m_GraphOutputs = m_GraphOutputs;
        clone->m_LocalVariables = m_LocalVariables;
        clone->m_WaveSources = m_WaveSources;
        clone->m_Version = m_Version;
        clone->m_NodeIdMap = m_NodeIdMap;
        // Compiled prototype is derived state — leave null so the next save/compile cycle
        // produces a fresh one matching the snapshot's topology.
        return clone;
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
