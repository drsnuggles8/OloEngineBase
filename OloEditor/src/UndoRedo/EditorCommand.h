#pragma once

#include "OloEngine/Core/Base.h"

#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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
            for (const auto& cmd : m_Commands)
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

        // Document operations (new/save) establish a checkpoint that belongs to
        // their resulting document. Undo restores the outgoing document's exact
        // checkpoint; ordinary edits retain the existing save-point semantics.
        void Execute(std::unique_ptr<EditorCommand> command, bool markSaved = false)
        {
            command->Execute();
            if (!m_RedoStack.empty() && m_SavePointValid && m_SavePointVersion > m_Version)
            {
                m_SavePointValid = false;
            }
            const SavePoint previousSavePoint{ m_SavePointVersion, m_SavePointValid };
            m_UndoStack.push_back({ std::move(command), markSaved ? std::optional(previousSavePoint) : std::nullopt });
            m_RedoStack.clear();
            ++m_Version;
            if (markSaved)
                MarkSaved();

            // Limit history size
            while (m_UndoStack.size() > MaxHistorySize)
            {
                m_UndoStack.pop_front();
                TrimSavePoint();
            }
        }

        // Push a command that has already been applied (e.g. ImGui widget already changed the value)
        void PushAlreadyExecuted(std::unique_ptr<EditorCommand> command)
        {
            if (!m_RedoStack.empty() && m_SavePointValid && m_SavePointVersion > m_Version)
            {
                m_SavePointValid = false;
            }
            m_UndoStack.push_back({ std::move(command), std::nullopt });
            m_RedoStack.clear();
            ++m_Version;

            while (m_UndoStack.size() > MaxHistorySize)
            {
                m_UndoStack.pop_front();
                TrimSavePoint();
            }
        }

        void Undo()
        {
            if (m_UndoStack.empty())
            {
                return;
            }

            // A guarded disk restore can refuse an external modification. Do
            // not remove its entry or alter dirty state when the command throws.
            m_UndoStack.back().Command->Undo();
            auto entry = std::move(m_UndoStack.back());
            m_UndoStack.pop_back();
            --m_Version;
            if (entry.PreviousSavePoint)
            {
                m_SavePointVersion = entry.PreviousSavePoint->Version;
                m_SavePointValid = entry.PreviousSavePoint->Valid;
            }
            m_RedoStack.push_back(std::move(entry));
            TrimSavePoint();
        }

        void Redo()
        {
            if (m_RedoStack.empty())
            {
                return;
            }

            m_RedoStack.back().Command->Execute();
            auto entry = std::move(m_RedoStack.back());
            m_RedoStack.pop_back();
            ++m_Version;
            if (entry.PreviousSavePoint)
                MarkSaved();
            m_UndoStack.push_back(std::move(entry));
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
            return m_UndoStack.empty() ? "" : m_UndoStack.back().Command->GetDescription();
        }

        [[nodiscard]] std::string GetRedoDescription() const
        {
            return m_RedoStack.empty() ? "" : m_RedoStack.back().Command->GetDescription();
        }

        // Save-point tracking for unsaved-changes detection
        void MarkSaved()
        {
            m_SavePointVersion = m_Version;
            m_SavePointValid = true;
        }

        [[nodiscard]] bool IsDirty() const
        {
            if (!m_SavePointValid)
            {
                return true;
            }
            return m_Version != m_SavePointVersion;
        }

        void Clear()
        {
            m_UndoStack.clear();
            m_RedoStack.clear();
            m_Version = 0;
            m_SavePointVersion = 0;
            m_SavePointValid = true;
        }

      private:
        struct SavePoint
        {
            std::size_t Version;
            bool Valid;
        };

        struct Entry
        {
            std::unique_ptr<EditorCommand> Command;
            std::optional<SavePoint> PreviousSavePoint;
        };

        void TrimSavePoint()
        {
            // When oldest entry is discarded, check if save point is still reachable
            if (m_SavePointValid)
            {
                // Minimum version reachable via undo = m_Version - m_UndoStack.size()
                std::size_t const minReachable = m_Version - m_UndoStack.size();
                if (m_SavePointVersion < minReachable)
                {
                    m_SavePointValid = false;
                }
            }
        }

        std::deque<Entry> m_UndoStack;
        std::deque<Entry> m_RedoStack;
        std::size_t m_Version = 0;
        std::size_t m_SavePointVersion = 0;
        bool m_SavePointValid = true;
    };
} // namespace OloEngine
