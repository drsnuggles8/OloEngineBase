#pragma once

#include "OloEngine/AI/BehaviorTree/BTNode.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
	// Serializable node data for BT assets
	struct BTNodeData
	{
		UUID ID;
		std::string TypeName;     // e.g. "Sequence", "Selector", "Wait", etc.
		std::string Name;
		std::vector<UUID> ChildIDs;

		// Properties stored as string key-value pairs for generic serialization
		std::unordered_map<std::string, std::string> Properties;
	};

	class BehaviorTreeAsset : public Asset
	{
	public:
		BehaviorTreeAsset() = default;
		~BehaviorTreeAsset() override = default;

		static AssetType GetStaticType()
		{
			return AssetType::BehaviorTree;
		}
		AssetType GetAssetType() const override
		{
			return GetStaticType();
		}

		const std::vector<BTNodeData>& GetNodes() const
		{
			return m_Nodes;
		}
		std::vector<BTNodeData>& GetNodes()
		{
			return m_Nodes;
		}

		UUID GetRootNodeID() const
		{
			return m_RootNodeID;
		}
		void SetRootNodeID(UUID id)
		{
			m_RootNodeID = id;
		}

		void AddNode(BTNodeData node)
		{
			m_Nodes.push_back(std::move(node));
		}

		const BTNodeData* FindNode(UUID id) const
		{
			for (auto const& node : m_Nodes)
			{
				if (node.ID == id)
				{
					return &node;
				}
			}
			return nullptr;
		}

		// Blackboard key definitions (name -> type hint string)
		std::unordered_map<std::string, std::string> BlackboardKeyDefs;

	private:
		std::vector<BTNodeData> m_Nodes;
		UUID m_RootNodeID = 0;
	};
} // namespace OloEngine
