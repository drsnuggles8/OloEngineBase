#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Progression/SkillTreeDatabase.h"

#include <unordered_set>

namespace OloEngine
{
    const SkillTreeNode* SkillTreeDatabase::FindNode(std::string_view nodeId) const
    {
        if (auto it = m_NodeIndex.find(nodeId); it != m_NodeIndex.end() && it->second < m_Nodes.size())
        {
            return &m_Nodes[it->second];
        }
        return nullptr;
    }

    void SkillTreeDatabase::RebuildIndex()
    {
        m_NodeIndex.clear();
        m_NodeIndex.reserve(m_Nodes.size());
        auto nodeCount = m_Nodes.size();
        for (sizet i = 0; i < nodeCount; ++i)
        {
            m_NodeIndex[m_Nodes[i].NodeID] = i;
        }
    }

    bool SkillTreeDatabase::Validate(std::string* outError) const
    {
        auto fail = [outError](std::string message)
        {
            if (outError)
            {
                *outError = std::move(message);
            }
            return false;
        };

        // Unique, non-empty node ids.
        std::unordered_map<std::string, sizet, StringHash, StringEqual> idToIndex;
        idToIndex.reserve(m_Nodes.size());
        auto nodeCount = m_Nodes.size();
        for (sizet i = 0; i < nodeCount; ++i)
        {
            const auto& node = m_Nodes[i];
            if (node.NodeID.empty())
            {
                return fail("node with empty NodeID");
            }
            if (!idToIndex.emplace(node.NodeID, i).second)
            {
                return fail("duplicate NodeID '" + node.NodeID + "'");
            }
        }

        // Every prerequisite must reference an existing, different node.
        for (const auto& node : m_Nodes)
        {
            for (const auto& prereq : node.Prerequisites)
            {
                if (prereq == node.NodeID)
                {
                    return fail("node '" + node.NodeID + "' lists itself as prerequisite");
                }
                if (!idToIndex.contains(prereq))
                {
                    return fail("node '" + node.NodeID + "' references unknown prerequisite '" + prereq + "'");
                }
            }
        }

        // Cycle check over the prerequisite DAG (Kahn's algorithm).
        std::vector<sizet> inDegree(m_Nodes.size(), 0);
        for (const auto& node : m_Nodes)
        {
            // Edge prereq -> node: a node's in-degree is its prerequisite count.
            sizet index = idToIndex[node.NodeID];
            inDegree[index] = node.Prerequisites.size();
        }

        std::vector<sizet> ready;
        ready.reserve(m_Nodes.size());
        for (sizet i = 0; i < nodeCount; ++i)
        {
            if (inDegree[i] == 0)
            {
                ready.push_back(i);
            }
        }

        // Dependents adjacency: prereq index -> nodes that list it.
        std::vector<std::vector<sizet>> dependents(m_Nodes.size());
        for (sizet i = 0; i < nodeCount; ++i)
        {
            for (const auto& prereq : m_Nodes[i].Prerequisites)
            {
                dependents[idToIndex[prereq]].push_back(i);
            }
        }

        sizet visited = 0;
        while (!ready.empty())
        {
            sizet index = ready.back();
            ready.pop_back();
            ++visited;
            for (sizet dependent : dependents[index])
            {
                if (--inDegree[dependent] == 0)
                {
                    ready.push_back(dependent);
                }
            }
        }

        if (visited != nodeCount)
        {
            std::string cycleNodes;
            for (sizet i = 0; i < nodeCount; ++i)
            {
                if (inDegree[i] > 0)
                {
                    if (!cycleNodes.empty())
                    {
                        cycleNodes += ", ";
                    }
                    cycleNodes += m_Nodes[i].NodeID;
                }
            }
            return fail("prerequisite cycle among nodes { " + cycleNodes + " }");
        }

        return true;
    }
} // namespace OloEngine
