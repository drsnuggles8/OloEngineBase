#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Dialogue/DialogueTypes.h"

#include <vector>

namespace OloEngine
{
    class DialogueTreeAsset : public Asset
    {
    public:
        DialogueTreeAsset() = default;
        ~DialogueTreeAsset() override = default;

        static AssetType GetStaticType() { return AssetType::DialogueTree; }
        AssetType GetAssetType() const override { return GetStaticType(); }

        const std::vector<DialogueNodeData>& GetNodes() const { return m_Nodes; }
        const std::vector<DialogueConnection>& GetConnections() const { return m_Connections; }
        UUID GetRootNodeID() const { return m_RootNodeID; }

        const DialogueNodeData* FindNode(UUID id) const
        {
            for (const auto& node : m_Nodes)
            {
                if (node.ID == id)
                    return &node;
            }
            return nullptr;
        }

        std::vector<DialogueConnection> GetConnectionsFrom(UUID nodeID, const std::string& port = "") const
        {
            std::vector<DialogueConnection> result;
            for (const auto& conn : m_Connections)
            {
                if (conn.SourceNodeID == nodeID && (port.empty() || conn.SourcePort == port))
                    result.push_back(conn);
            }
            return result;
        }

        // Writable access for tests and serialization
        std::vector<DialogueNodeData>& GetNodesWritable() { return m_Nodes; }
        std::vector<DialogueConnection>& GetConnectionsWritable() { return m_Connections; }
        void SetRootNodeID(UUID id) { m_RootNodeID = id; }

    private:
        std::vector<DialogueNodeData> m_Nodes;
        std::vector<DialogueConnection> m_Connections;
        UUID m_RootNodeID = 0;

        friend class DialogueTreeSerializer;
    };

} // namespace OloEngine
