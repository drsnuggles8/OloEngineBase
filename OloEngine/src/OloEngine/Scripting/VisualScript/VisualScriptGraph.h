#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptTypes.h"

#include <glm/glm.hpp>

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::VisualScript
{
    //==============================================================================
    /// One placed node.
    ///
    /// A node stores only what the *author* chose: which type it is, where it sits,
    /// the literal values on its unconnected input pins, and a small string
    /// property bag for the handful of node types whose signature depends on
    /// authoring (which variable a Get/Set touches, which event a CustomEvent
    /// listens for, how many outputs a Sequence has). Everything else — pin lists,
    /// types, behavior — comes from the registry at compile time, so a node's
    /// signature can be corrected without rewriting saved graphs.
    struct VisualScriptNode
    {
        NodeId m_Id = kInvalidNodeId;
        std::string m_TypeName;
        glm::vec2 m_Position{ 0.0f, 0.0f };

        /// Literal values for input data pins, keyed by PIN NAME (never index —
        /// see VisualScriptLink). Absent entries fall back to the descriptor's
        /// default.
        std::map<std::string, PinValue> m_PinDefaults;

        /// Authoring properties consumed by the pin resolver of a few node types.
        /// `std::map` (not unordered) so the serialized order is deterministic and
        /// a save → load → save round trip is byte-identical.
        std::map<std::string, std::string> m_Properties;

        [[nodiscard]] const std::string& GetProperty(std::string_view key, const std::string& fallback) const;
        void SetProperty(std::string key, std::string value);

        [[nodiscard]] bool operator==(const VisualScriptNode& other) const = default;
    };

    //==============================================================================
    /// One wire. Endpoints are (node id, pin NAME) rather than (node id, pin index)
    /// on purpose: inserting a pin into the middle of a node type's signature would
    /// otherwise silently rewire every saved graph that used it.
    struct VisualScriptLink
    {
        LinkId m_Id = kInvalidLinkId;
        NodeId m_SourceNode = kInvalidNodeId;
        std::string m_SourcePin;
        NodeId m_TargetNode = kInvalidNodeId;
        std::string m_TargetPin;

        [[nodiscard]] bool operator==(const VisualScriptLink& other) const = default;
    };

    //==============================================================================
    /// A blackboard variable, or a function parameter/result (the same shape — a
    /// function's locals are just a graph-scoped blackboard).
    struct VisualScriptVariable
    {
        std::string m_Name;
        PinType m_Type = PinType::Float;
        PinValue m_DefaultValue = PinValue::MakeFloat(0.0f);

        [[nodiscard]] bool operator==(const VisualScriptVariable& other) const = default;
    };

    //==============================================================================
    /// A single node graph: either the asset's event graph or one sub-graph
    /// function. Functions carry their own parameter/result lists, which is what
    /// gives Function.Entry / Function.Return / Function.Call their pin layouts.
    struct VisualScriptGraph
    {
        std::string m_Name = "EventGraph";
        std::vector<VisualScriptNode> m_Nodes;
        std::vector<VisualScriptLink> m_Links;

        /// Empty for the event graph; the signature for a function graph.
        std::vector<VisualScriptVariable> m_Inputs;
        std::vector<VisualScriptVariable> m_Outputs;

        NodeId m_NextNodeId = 1;
        LinkId m_NextLinkId = 1;

        //-- Authoring helpers (also used by the tests and, later, the panel) ------
        VisualScriptNode& AddNode(std::string typeName, glm::vec2 position = { 0.0f, 0.0f });
        LinkId AddLink(NodeId sourceNode, std::string sourcePin, NodeId targetNode, std::string targetPin);
        bool RemoveNode(NodeId id);
        bool RemoveLink(LinkId id);

        [[nodiscard]] VisualScriptNode* FindNode(NodeId id);
        [[nodiscard]] const VisualScriptNode* FindNode(NodeId id) const;

        [[nodiscard]] bool operator==(const VisualScriptGraph& other) const = default;
    };

    //==============================================================================
    /// The authored artefact. One event graph, N sub-graph functions, and the
    /// asset-scoped blackboard whose defaults a VisualScriptComponent may override
    /// per entity.
    class VisualScriptAsset : public Asset
    {
      public:
        static AssetType GetStaticType()
        {
            return AssetType::VisualScript;
        }
        AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

        VisualScriptGraph m_EventGraph;
        std::vector<VisualScriptGraph> m_Functions;
        std::vector<VisualScriptVariable> m_Variables;

        /// Per-tick execution budget for one instance of this graph. A graph that
        /// exceeds it is halted for the tick with one log line — the guard against
        /// an authored `While(true)`, which is otherwise a hard hang with no stack
        /// to look at.
        u32 m_NodeBudgetPerTick = 10000;

        [[nodiscard]] const VisualScriptGraph* FindFunction(std::string_view name) const;
        [[nodiscard]] const VisualScriptVariable* FindVariable(std::string_view name) const;

        /// Deep value comparison — used by the serializer round-trip test and by
        /// hot-reload to skip a no-op reload.
        [[nodiscard]] bool EqualsForTest(const VisualScriptAsset& other) const;
    };

} // namespace OloEngine::VisualScript
