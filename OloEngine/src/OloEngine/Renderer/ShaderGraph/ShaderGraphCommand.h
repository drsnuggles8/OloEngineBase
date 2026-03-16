#pragma once

#include "OloEngine/Core/UUID.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraph.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphNode.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphLink.h"

#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace OloEngine
{
    /// Base class for undoable shader graph commands.
    class ShaderGraphCommand
    {
      public:
        virtual ~ShaderGraphCommand() = default;

        /// Execute or re-execute the command
        virtual void Execute(ShaderGraph& graph) = 0;

        /// Undo the command
        virtual void Undo(ShaderGraph& graph) = 0;

        /// Human-readable description for the undo/redo menu
        [[nodiscard]] virtual std::string GetDescription() const = 0;
    };

    /// Command to add a new node to the graph
    class AddNodeCommand final : public ShaderGraphCommand
    {
      public:
        AddNodeCommand(const std::string& typeName, const glm::vec2& position)
            : m_TypeName(typeName), m_Position(position)
        {
        }

        void Execute(ShaderGraph& graph) override
        {
            auto node = CreateShaderGraphNode(m_TypeName);
            if (!node)
                return;
            node->EditorPosition = m_Position;
            if (m_NodeID != 0)
                node->ID = m_NodeID;
            else
                m_NodeID = node->ID;
            graph.AddNode(std::move(node));
        }

        void Undo(ShaderGraph& graph) override
        {
            graph.RemoveNode(m_NodeID);
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return "Add " + m_TypeName;
        }

        [[nodiscard]] UUID GetNodeID() const
        {
            return m_NodeID;
        }

      private:
        std::string m_TypeName;
        glm::vec2 m_Position;
        UUID m_NodeID = 0;
    };

    /// Command to remove a node and its connected links.
    /// Captures the entire node + links state for undo restoration.
    class RemoveNodeCommand final : public ShaderGraphCommand
    {
      public:
        explicit RemoveNodeCommand(UUID nodeID)
            : m_NodeID(nodeID)
        {
        }

        void Execute(ShaderGraph& graph) override
        {
            // Snapshot the node for undo
            const auto* node = graph.FindNode(m_NodeID);
            if (!node)
                return;

            // Save a copy of the node state
            m_SavedTypeName = node->TypeName;
            m_SavedPosition = node->EditorPosition;
            m_SavedParameterName = node->ParameterName;
            m_SavedInputs = node->Inputs;
            m_SavedOutputs = node->Outputs;

            // Save all links connected to this node
            m_SavedLinks.clear();
            for (const auto& pin : node->Inputs)
            {
                if (const auto* link = graph.GetLinkForInputPin(pin.ID))
                    m_SavedLinks.push_back(*link);
            }
            for (const auto& pin : node->Outputs)
            {
                for (const auto* link : graph.GetLinksForOutputPin(pin.ID))
                    m_SavedLinks.push_back(*link);
            }

            graph.RemoveNode(m_NodeID);
        }

        void Undo(ShaderGraph& graph) override
        {
            // Recreate the node
            auto node = CreateShaderGraphNode(m_SavedTypeName);
            if (!node)
                return;

            node->ID = m_NodeID;
            node->EditorPosition = m_SavedPosition;
            node->ParameterName = m_SavedParameterName;
            node->Inputs = m_SavedInputs;
            node->Outputs = m_SavedOutputs;

            graph.AddNode(std::move(node));

            // Recreate the saved links (bypass validation — we know they were valid)
            for (const auto& link : m_SavedLinks)
                graph.AddLink(link.OutputPinID, link.InputPinID);
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return "Remove " + m_SavedTypeName;
        }

      private:
        UUID m_NodeID;
        std::string m_SavedTypeName;
        glm::vec2 m_SavedPosition{};
        std::string m_SavedParameterName;
        std::vector<ShaderGraphPin> m_SavedInputs;
        std::vector<ShaderGraphPin> m_SavedOutputs;
        std::vector<ShaderGraphLink> m_SavedLinks;
    };

    /// Command to add a link between two pins
    class AddLinkCommand final : public ShaderGraphCommand
    {
      public:
        AddLinkCommand(UUID outputPinID, UUID inputPinID)
            : m_OutputPinID(outputPinID), m_InputPinID(inputPinID)
        {
        }

        void Execute(ShaderGraph& graph) override
        {
            // Save any existing link to this input (AddLink replaces it)
            if (const auto* existing = graph.GetLinkForInputPin(m_InputPinID))
            {
                m_ReplacedLink = *existing;
                m_HadPreviousLink = true;
            }

            auto* link = graph.AddLink(m_OutputPinID, m_InputPinID);
            if (link)
                m_LinkID = link->ID;
        }

        void Undo(ShaderGraph& graph) override
        {
            graph.RemoveLink(m_LinkID);

            // Restore previously replaced link
            if (m_HadPreviousLink)
                graph.AddLink(m_ReplacedLink.OutputPinID, m_ReplacedLink.InputPinID);
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return "Add Connection";
        }

      private:
        UUID m_OutputPinID;
        UUID m_InputPinID;
        UUID m_LinkID = 0;
        ShaderGraphLink m_ReplacedLink;
        bool m_HadPreviousLink = false;
    };

    /// Command to remove a link
    class RemoveLinkCommand final : public ShaderGraphCommand
    {
      public:
        explicit RemoveLinkCommand(UUID linkID)
            : m_LinkID(linkID)
        {
        }

        void Execute(ShaderGraph& graph) override
        {
            // Save the link for undo
            const auto* link = graph.FindLink(m_LinkID);
            if (link)
                m_SavedLink = *link;
            graph.RemoveLink(m_LinkID);
        }

        void Undo(ShaderGraph& graph) override
        {
            graph.AddLink(m_SavedLink.OutputPinID, m_SavedLink.InputPinID);
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return "Remove Connection";
        }

      private:
        UUID m_LinkID;
        ShaderGraphLink m_SavedLink;
    };

    /// Command to move a node to a new position
    class MoveNodeCommand final : public ShaderGraphCommand
    {
      public:
        MoveNodeCommand(UUID nodeID, const glm::vec2& oldPosition, const glm::vec2& newPosition)
            : m_NodeID(nodeID), m_OldPosition(oldPosition), m_NewPosition(newPosition)
        {
        }

        void Execute(ShaderGraph& graph) override
        {
            if (auto* node = graph.FindNode(m_NodeID))
                node->EditorPosition = m_NewPosition;
        }

        void Undo(ShaderGraph& graph) override
        {
            if (auto* node = graph.FindNode(m_NodeID))
                node->EditorPosition = m_OldPosition;
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return "Move Node";
        }

      private:
        UUID m_NodeID;
        glm::vec2 m_OldPosition;
        glm::vec2 m_NewPosition;
    };

    /// Command to change a pin's default value
    class ChangePinValueCommand final : public ShaderGraphCommand
    {
      public:
        ChangePinValueCommand(UUID pinID, ShaderGraphPinValue oldValue, ShaderGraphPinValue newValue)
            : m_PinID(pinID), m_OldValue(std::move(oldValue)), m_NewValue(std::move(newValue))
        {
        }

        void Execute(ShaderGraph& graph) override
        {
            if (auto* pin = graph.FindPin(m_PinID))
                pin->DefaultValue = m_NewValue;
        }

        void Undo(ShaderGraph& graph) override
        {
            if (auto* pin = graph.FindPin(m_PinID))
                pin->DefaultValue = m_OldValue;
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return "Change Value";
        }

      private:
        UUID m_PinID;
        ShaderGraphPinValue m_OldValue;
        ShaderGraphPinValue m_NewValue;
    };

    /// Command to rename a parameter node
    class RenameParameterCommand final : public ShaderGraphCommand
    {
      public:
        RenameParameterCommand(UUID nodeID, std::string oldName, std::string newName)
            : m_NodeID(nodeID), m_OldName(std::move(oldName)), m_NewName(std::move(newName))
        {
        }

        void Execute(ShaderGraph& graph) override
        {
            if (auto* node = graph.FindNode(m_NodeID))
                node->ParameterName = m_NewName;
        }

        void Undo(ShaderGraph& graph) override
        {
            if (auto* node = graph.FindNode(m_NodeID))
                node->ParameterName = m_OldName;
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return "Rename Parameter";
        }

      private:
        UUID m_NodeID;
        std::string m_OldName;
        std::string m_NewName;
    };

    /// Manages the undo/redo stack for a shader graph editor session
    class ShaderGraphCommandHistory
    {
      public:
        /// Execute a command and push it onto the undo stack.
        /// Clears the redo stack.
        void Execute(Scope<ShaderGraphCommand> command, ShaderGraph& graph)
        {
            command->Execute(graph);
            m_UndoStack.push_back(std::move(command));
            m_RedoStack.clear();
        }

        /// Undo the last command
        void Undo(ShaderGraph& graph)
        {
            if (m_UndoStack.empty())
                return;

            auto& command = m_UndoStack.back();
            command->Undo(graph);
            m_RedoStack.push_back(std::move(command));
            m_UndoStack.pop_back();
        }

        /// Redo the last undone command
        void Redo(ShaderGraph& graph)
        {
            if (m_RedoStack.empty())
                return;

            auto& command = m_RedoStack.back();
            command->Execute(graph);
            m_UndoStack.push_back(std::move(command));
            m_RedoStack.pop_back();
        }

        [[nodiscard]] bool CanUndo() const
        {
            return !m_UndoStack.empty();
        }
        [[nodiscard]] bool CanRedo() const
        {
            return !m_RedoStack.empty();
        }

        [[nodiscard]] std::string GetUndoDescription() const
        {
            return m_UndoStack.empty() ? "" : m_UndoStack.back()->GetDescription();
        }

        [[nodiscard]] std::string GetRedoDescription() const
        {
            return m_RedoStack.empty() ? "" : m_RedoStack.back()->GetDescription();
        }

        void Clear()
        {
            m_UndoStack.clear();
            m_RedoStack.clear();
        }

      private:
        std::vector<Scope<ShaderGraphCommand>> m_UndoStack;
        std::vector<Scope<ShaderGraphCommand>> m_RedoStack;
    };

} // namespace OloEngine
