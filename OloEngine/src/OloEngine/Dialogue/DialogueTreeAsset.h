#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Dialogue/DialogueTypes.h"
#include "OloEngine/Containers/LinkedList.h"

#include <unordered_map>
#include <string_view>

namespace OloEngine
{
    class DialogueTreeAsset : public Asset
    {
      public:
        DialogueTreeAsset() = default;
        ~DialogueTreeAsset() override = default;

        static AssetType GetStaticType()
        {
            return AssetType::DialogueTree;
        }
        AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

        const TDoubleLinkedList<DialogueNodeData>& GetNodes() const
        {
            return m_Nodes;
        }
        const TArray<DialogueConnection>& GetConnections() const
        {
            return m_Connections;
        }
        UUID GetRootNodeID() const
        {
            return m_RootNodeID;
        }

        const DialogueNodeData* FindNode(UUID id) const
        {
            if (auto it = m_NodeIndex.find(id); it != m_NodeIndex.end())
            {
                return it->second;
            }
            return nullptr;
        }

        TArray<DialogueConnection> GetConnectionsFrom(UUID nodeID, std::string_view port = {}) const
        {
            TArray<DialogueConnection> result;
            for (const auto& conn : m_Connections)
            {
                if (conn.SourceNodeID == nodeID && (port.empty() || conn.SourcePort.ToView() == port))
                    result.Add(conn);
            }
            return result;
        }

        // Writable access for tests and serialization
        TDoubleLinkedList<DialogueNodeData>& GetNodesWritable()
        {
            return m_Nodes;
        }
        TArray<DialogueConnection>& GetConnectionsWritable()
        {
            return m_Connections;
        }
        void SetRootNodeID(UUID id)
        {
            m_RootNodeID = id;
        }

        void RebuildNodeIndex()
        {
            m_NodeIndex.clear();
            for (auto& node : m_Nodes)
            {
                m_NodeIndex[node.ID] = &node;
            }
        }

      private:
        // Property maps remain standard containers under the reference-stability
        // audit gate. Stable list nodes avoid relocating those maps entirely.
        TDoubleLinkedList<DialogueNodeData> m_Nodes;
        TArray<DialogueConnection> m_Connections;
        UUID m_RootNodeID = 0;
        std::unordered_map<UUID, const DialogueNodeData*> m_NodeIndex;

        friend class DialogueTreeSerializer;
    };

} // namespace OloEngine
