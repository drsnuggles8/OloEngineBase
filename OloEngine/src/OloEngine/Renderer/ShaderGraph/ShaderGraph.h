#pragma once

#include "OloEngine/Core/UUID.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphNode.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphLink.h"

#include <string>
#include <vector>

namespace OloEngine
{
    /// Result of graph validation
    struct ShaderGraphValidationResult
    {
        bool IsValid = true;
        std::vector<std::string> Errors;
        std::vector<std::string> Warnings;
    };

    /// The shader graph data model.
    /// Owns nodes and links; provides graph manipulation and validation.
    class ShaderGraph
    {
      public:
        ShaderGraph() = default;
        ~ShaderGraph() = default;

        // ── Node management ──────────────────────────────────

        /// Add a node to the graph. Takes ownership.
        ShaderGraphNode* AddNode(Scope<ShaderGraphNode> node);

        /// Remove a node and all its connected links
        bool RemoveNode(UUID nodeID);

        /// Find a node by ID
        ShaderGraphNode* FindNode(UUID nodeID);
        const ShaderGraphNode* FindNode(UUID nodeID) const;

        /// Find a pin by ID (searches all nodes)
        ShaderGraphPin* FindPin(UUID pinID);
        const ShaderGraphPin* FindPin(UUID pinID) const;

        /// Find the node that owns a given pin
        ShaderGraphNode* FindNodeByPinID(UUID pinID);
        const ShaderGraphNode* FindNodeByPinID(UUID pinID) const;

        const std::vector<Scope<ShaderGraphNode>>& GetNodes() const
        {
            return m_Nodes;
        }

        // ── Link management ──────────────────────────────────

        /// Add a link from an output pin to an input pin.
        /// Returns nullptr if the connection is invalid (type mismatch, cycle, etc.)
        ShaderGraphLink* AddLink(UUID outputPinID, UUID inputPinID);

        /// Remove a link by ID
        bool RemoveLink(UUID linkID);

        /// Find a link by ID
        ShaderGraphLink* FindLink(UUID linkID);
        const ShaderGraphLink* FindLink(UUID linkID) const;

        /// Get the link connected to a specific input pin (each input has at most one link)
        const ShaderGraphLink* GetLinkForInputPin(UUID inputPinID) const;

        /// Get all links connected to a specific output pin
        std::vector<const ShaderGraphLink*> GetLinksForOutputPin(UUID outputPinID) const;

        /// Get the source pin connected to an input pin (follows the link)
        const ShaderGraphPin* GetConnectedOutputPin(UUID inputPinID) const;

        const std::vector<ShaderGraphLink>& GetLinks() const
        {
            return m_Links;
        }

        // ── Graph analysis ───────────────────────────────────

        /// Validate the graph (check for cycles, required connections, etc.)
        ShaderGraphValidationResult Validate() const;

        /// Returns nodes in topological order (output node last).
        /// Returns empty if the graph has cycles.
        std::vector<const ShaderGraphNode*> GetTopologicalOrder() const;

        /// Find the PBR output node (there should be exactly one)
        const ShaderGraphNode* FindOutputNode() const;

        /// Check if adding a link would create a cycle
        bool WouldCreateCycle(UUID outputPinID, UUID inputPinID) const;

        // ── Metadata ─────────────────────────────────────────

        const std::string& GetName() const
        {
            return m_Name;
        }
        void SetName(const std::string& name)
        {
            m_Name = name;
        }

      private:
        std::vector<Scope<ShaderGraphNode>> m_Nodes;
        std::vector<ShaderGraphLink> m_Links;
        std::string m_Name = "Untitled";

        friend class ShaderGraphSerializer;
        friend class ShaderGraphCompiler;
    };

} // namespace OloEngine
