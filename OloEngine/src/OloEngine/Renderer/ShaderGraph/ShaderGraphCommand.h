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

        /// Execute or re-execute the command. Returns true if the command mutated the graph.
        virtual bool Execute(ShaderGraph& graph) = 0;

        /// Undo the command
        virtual void Undo(ShaderGraph& graph) = 0;

        /// Human-readable description for the undo/redo menu
        [[nodiscard]] virtual std::string GetDescription() const = 0;
    };

    /// Optional properties to apply when a node is created (e.g., during paste)
    struct AddNodeProperties
    {
        std::string ParameterName;
        std::string CustomFunctionBody;
        glm::ivec3 WorkgroupSize{ 16, 16, 1 };
        int BufferBinding = 0;
        std::vector<ShaderGraphPinValue> InputDefaultValues;
        bool HasProperties = false;
    };

    /// Command to add a new node to the graph
    class AddNodeCommand final : public ShaderGraphCommand
    {
      public:
        AddNodeCommand(const std::string& typeName, const glm::vec2& position)
            : m_TypeName(typeName), m_Position(position)
        {
        }

        AddNodeCommand(const std::string& typeName, const glm::vec2& position, AddNodeProperties properties)
            : m_TypeName(typeName), m_Position(position), m_Properties(std::move(properties))
        {
            m_Properties.HasProperties = true;
        }

        bool Execute(ShaderGraph& graph) override
        {
            auto node = CreateShaderGraphNode(m_TypeName);
            if (!node)
                return false;
            node->EditorPosition = m_Position;
            if (m_NodeID != 0)
                node->ID = m_NodeID;
            else
                m_NodeID = node->ID;

            if (m_Properties.HasProperties)
            {
                node->ParameterName = m_Properties.ParameterName;
                node->CustomFunctionBody = m_Properties.CustomFunctionBody;
                node->WorkgroupSize = m_Properties.WorkgroupSize;
                node->BufferBinding = m_Properties.BufferBinding;
                for (size_t i = 0; i < node->Inputs.size() && i < m_Properties.InputDefaultValues.size(); ++i)
                    node->Inputs[i].DefaultValue = m_Properties.InputDefaultValues[i];
            }

            graph.AddNode(std::move(node));
            return true;
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
        AddNodeProperties m_Properties;
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

        bool Execute(ShaderGraph& graph) override
        {
            // Snapshot the node for undo
            const auto* node = graph.FindNode(m_NodeID);
            if (!node)
                return false;

            // Save a copy of the node state
            m_SavedTypeName = node->TypeName;
            m_SavedPosition = node->EditorPosition;
            m_SavedParameterName = node->ParameterName;
            m_SavedCustomFunctionBody = node->CustomFunctionBody;
            m_SavedWorkgroupSize = node->WorkgroupSize;
            m_SavedBufferBinding = node->BufferBinding;
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
            return true;
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
            node->CustomFunctionBody = m_SavedCustomFunctionBody;
            node->WorkgroupSize = m_SavedWorkgroupSize;
            node->BufferBinding = m_SavedBufferBinding;
            node->Inputs = m_SavedInputs;
            node->Outputs = m_SavedOutputs;

            graph.AddNode(std::move(node));

            // Restore links with their original IDs (bypass validation — they were valid before removal)
            for (const auto& link : m_SavedLinks)
                graph.m_Links.push_back(link);
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
        std::string m_SavedCustomFunctionBody;
        glm::ivec3 m_SavedWorkgroupSize{ 16, 16, 1 };
        int m_SavedBufferBinding = 0;
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

        bool Execute(ShaderGraph& graph) override
        {
            // Save any existing link to this input (AddLink replaces it)
            if (const auto* existing = graph.GetLinkForInputPin(m_InputPinID))
            {
                m_ReplacedLink = *existing;
                m_HadPreviousLink = true;
            }

            auto* link = graph.AddLink(m_OutputPinID, m_InputPinID);
            if (link)
            {
                m_LinkID = link->ID;
                return true;
            }
            return false;
        }

        void Undo(ShaderGraph& graph) override
        {
            graph.RemoveLink(m_LinkID);

            // Restore previously replaced link with its original ID
            if (m_HadPreviousLink)
                graph.RestoreLink(m_ReplacedLink.ID, m_ReplacedLink.OutputPinID, m_ReplacedLink.InputPinID);
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

        bool Execute(ShaderGraph& graph) override
        {
            // Save the link for undo
            const auto* link = graph.FindLink(m_LinkID);
            if (link)
                m_SavedLink = *link;
            return graph.RemoveLink(m_LinkID);
        }

        void Undo(ShaderGraph& graph) override
        {
            graph.RestoreLink(m_SavedLink.ID, m_SavedLink.OutputPinID, m_SavedLink.InputPinID);
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

        bool Execute(ShaderGraph& graph) override
        {
            auto* node = graph.FindNode(m_NodeID);
            if (!node)
                return false;
            node->EditorPosition = m_NewPosition;
            return true;
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

    /// Compound command that groups multiple sub-commands as a single undoable operation
    class CompoundShaderGraphCommand final : public ShaderGraphCommand
    {
      public:
        explicit CompoundShaderGraphCommand(std::string description)
            : m_Description(std::move(description))
        {
        }

        void Add(Scope<ShaderGraphCommand> command)
        {
            m_Commands.push_back(std::move(command));
        }

        bool Execute(ShaderGraph& graph) override
        {
            for (auto& cmd : m_Commands)
                cmd->Execute(graph);
            return true;
        }

        void Undo(ShaderGraph& graph) override
        {
            for (auto it = m_Commands.rbegin(); it != m_Commands.rend(); ++it)
                (*it)->Undo(graph);
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return m_Description;
        }

      private:
        std::string m_Description;
        std::vector<Scope<ShaderGraphCommand>> m_Commands;
    };

    /// Command to change a pin's default value
    class ChangePinValueCommand final : public ShaderGraphCommand
    {
      public:
        ChangePinValueCommand(UUID pinID, ShaderGraphPinValue oldValue, ShaderGraphPinValue newValue)
            : m_PinID(pinID), m_OldValue(std::move(oldValue)), m_NewValue(std::move(newValue))
        {
        }

        bool Execute(ShaderGraph& graph) override
        {
            auto* pin = graph.FindPin(m_PinID);
            if (!pin)
                return false;
            pin->DefaultValue = m_NewValue;
            return true;
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

        bool Execute(ShaderGraph& graph) override
        {
            auto* node = graph.FindNode(m_NodeID);
            if (!node)
                return false;
            node->ParameterName = m_NewName;
            return true;
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

    /// Command to change a custom function node's GLSL body
    class SetCustomFunctionBodyCommand final : public ShaderGraphCommand
    {
      public:
        SetCustomFunctionBodyCommand(UUID nodeID, std::string oldBody, std::string newBody)
            : m_NodeID(nodeID), m_OldBody(std::move(oldBody)), m_NewBody(std::move(newBody))
        {
        }

        bool Execute(ShaderGraph& graph) override
        {
            auto* node = graph.FindNode(m_NodeID);
            if (!node)
                return false;
            node->CustomFunctionBody = m_NewBody;
            return true;
        }

        void Undo(ShaderGraph& graph) override
        {
            if (auto* node = graph.FindNode(m_NodeID))
                node->CustomFunctionBody = m_OldBody;
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return "Set Custom Function";
        }

      private:
        UUID m_NodeID;
        std::string m_OldBody;
        std::string m_NewBody;
    };

    /// Command to change a compute output node's workgroup size
    class SetWorkgroupSizeCommand final : public ShaderGraphCommand
    {
      public:
        SetWorkgroupSizeCommand(UUID nodeID, glm::ivec3 oldSize, glm::ivec3 newSize)
            : m_NodeID(nodeID), m_OldSize(oldSize), m_NewSize(newSize)
        {
        }

        bool Execute(ShaderGraph& graph) override
        {
            auto* node = graph.FindNode(m_NodeID);
            if (!node)
                return false;
            node->WorkgroupSize = m_NewSize;
            return true;
        }

        void Undo(ShaderGraph& graph) override
        {
            if (auto* node = graph.FindNode(m_NodeID))
                node->WorkgroupSize = m_OldSize;
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return "Set Workgroup Size";
        }

      private:
        UUID m_NodeID;
        glm::ivec3 m_OldSize;
        glm::ivec3 m_NewSize;
    };

    /// Command to change a buffer node's binding index
    class SetBufferBindingCommand final : public ShaderGraphCommand
    {
      public:
        SetBufferBindingCommand(UUID nodeID, int oldBinding, int newBinding)
            : m_NodeID(nodeID), m_OldBinding(oldBinding), m_NewBinding(newBinding)
        {
        }

        bool Execute(ShaderGraph& graph) override
        {
            auto* node = graph.FindNode(m_NodeID);
            if (!node)
                return false;
            node->BufferBinding = m_NewBinding;
            return true;
        }

        void Undo(ShaderGraph& graph) override
        {
            if (auto* node = graph.FindNode(m_NodeID))
                node->BufferBinding = m_OldBinding;
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return "Set Buffer Binding";
        }

      private:
        UUID m_NodeID;
        int m_OldBinding;
        int m_NewBinding;
    };

    /// Manages the undo/redo stack for a shader graph editor session
    class ShaderGraphCommandHistory
    {
      public:
        /// Execute a command and push it onto the undo stack.
        /// Clears the redo stack.
        void Execute(Scope<ShaderGraphCommand> command, ShaderGraph& graph)
        {
            if (!command->Execute(graph))
                return;
            m_UndoStack.push_back(std::move(command));
            m_RedoStack.clear();
        }

        /// Push a command that has already been applied to the graph.
        /// Used for ImGui interactive edits where the widget directly mutated the graph.
        void PushExecuted(Scope<ShaderGraphCommand> command)
        {
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
