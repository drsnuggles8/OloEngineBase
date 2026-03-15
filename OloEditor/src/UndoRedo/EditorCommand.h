#pragma once

#include "OloEngine/Core/Base.h"

#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace OloEngine
{
    // Base class for all undoable editor operations
    class EditorCommand
    {
      public:
        virtual ~EditorCommand() = default;

        virtual void Execute() = 0;
        virtual void Undo() = 0;

        [[nodiscard]] virtual std::string GetDescription() const = 0;
    };

    // Groups multiple commands into a single undoable operation
    class CompoundCommand : public EditorCommand
    {
      public:
        explicit CompoundCommand(std::string description)
            : m_Description(std::move(description))
        {
        }

        void Add(std::unique_ptr<EditorCommand> command)
        {
            m_Commands.push_back(std::move(command));
        }

        void Execute() override
        {
            for (auto& cmd : m_Commands)
            {
                cmd->Execute();
            }
        }

        void Undo() override
        {
            // Undo in reverse order
            for (auto it = m_Commands.rbegin(); it != m_Commands.rend(); ++it)
            {
                (*it)->Undo();
            }
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return m_Description;
        }

        [[nodiscard]] bool IsEmpty() const
        {
            return m_Commands.empty();
        }

      private:
        std::vector<std::unique_ptr<EditorCommand>> m_Commands;
        std::string m_Description;
    };

    // Manages a stack of undoable/redoable commands
    class CommandHistory
    {
      public:
        static constexpr std::size_t MaxHistorySize = 128;

        void Execute(std::unique_ptr<EditorCommand> command)
        {
            command->Execute();
            m_UndoStack.push_back(std::move(command));
            m_RedoStack.clear();

            // Limit history size
            while (m_UndoStack.size() > MaxHistorySize)
            {
                m_UndoStack.pop_front();
            }
        }

        // Push a command that has already been applied (e.g. ImGui widget already changed the value)
        void PushAlreadyExecuted(std::unique_ptr<EditorCommand> command)
        {
            m_UndoStack.push_back(std::move(command));
            m_RedoStack.clear();

            while (m_UndoStack.size() > MaxHistorySize)
            {
                m_UndoStack.pop_front();
            }
        }

        void Undo()
        {
            if (m_UndoStack.empty())
            {
                return;
            }

            auto command = std::move(m_UndoStack.back());
            m_UndoStack.pop_back();
            command->Undo();
            m_RedoStack.push_back(std::move(command));
        }

        void Redo()
        {
            if (m_RedoStack.empty())
            {
                return;
            }

            auto command = std::move(m_RedoStack.back());
            m_RedoStack.pop_back();
            command->Execute();
            m_UndoStack.push_back(std::move(command));
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
        std::deque<std::unique_ptr<EditorCommand>> m_UndoStack;
        std::deque<std::unique_ptr<EditorCommand>> m_RedoStack;
    };
} // namespace OloEngine
