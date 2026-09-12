#pragma once

#include "OloEngine/Core/Base.h"

#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
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
            m_UndoneFrom = m_Commands.size();
        }

        void Execute() override
        {
            for (const auto& cmd : m_Commands)
            {
                cmd->Execute();
            }
            m_UndoneFrom = m_Commands.size();
        }

        // Undo in reverse order, RESUMABLE.
        //
        // CommandHistory::Undo deliberately keeps an entry on the stack when its
        // undo throws, so the user can fix the cause (a scene save refuses to
        // overwrite an externally modified file) and press Ctrl-Z again. For a
        // single command that retries exactly the operation that refused. For a
        // GROUP it must not restart from the end, or every retry would re-undo
        // the members already taken back -- and with a transaction committing N
        // operations as one entry (issue #1127), a group is now the common case.
        // So remember how far the last attempt got and resume from there.
        void Undo() override
        {
            while (m_UndoneFrom > 0)
            {
                m_Commands[m_UndoneFrom - 1]->Undo();
                --m_UndoneFrom;
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

        [[nodiscard]] std::size_t Size() const
        {
            return m_Commands.size();
        }

        // Undo every member in reverse, CONTINUING past one that throws, and
        // return the first failure's message (empty when all of them were taken
        // back). Undo() above stops at the first throw, which is right for a
        // user pressing Ctrl-Z: the refusal is reported and nothing else moves.
        //
        // Rolling a transaction back is the opposite situation. One member
        // refusing -- a guarded scene-save restore that finds the file
        // externally modified does refuse -- must not strand the members
        // underneath it, or the rollback leaves exactly the half-applied state
        // the transaction existed to prevent. So this takes back everything it
        // can and REPORTS what it could not, for the caller to surface loudly
        // (issue #1127).
        [[nodiscard]] std::string UndoAll()
        {
            std::string firstFailure;
            std::size_t stillApplied = 0;
            while (m_UndoneFrom > 0)
            {
                EditorCommand& command = *m_Commands[m_UndoneFrom - 1];
                --m_UndoneFrom;
                try
                {
                    command.Undo();
                }
                catch (const std::exception& e)
                {
                    ++stillApplied;
                    if (firstFailure.empty())
                        firstFailure = command.GetDescription() + ": " + e.what();
                }
                catch (...)
                {
                    ++stillApplied;
                    if (firstFailure.empty())
                        firstFailure = command.GetDescription() + ": non-standard exception";
                }
            }
            // A member that refused is still applied, and nothing here can say
            // WHICH steps of the batch it belonged to -- one step may push zero
            // or several entries. The caller reports "we could not confirm this
            // was taken back" rather than inventing per-step attribution.
            if (!firstFailure.empty())
            {
                firstFailure = std::to_string(stillApplied) + " of " + std::to_string(m_Commands.size()) +
                               " operations could not be taken back; first: " + firstFailure;
            }
            return firstFailure;
        }

      private:
        std::vector<std::unique_ptr<EditorCommand>> m_Commands;
        std::string m_Description;
        // Members [0, m_UndoneFrom) are applied; the rest have been undone. Add()
        // keeps it at the end because a command handed to a CompoundCommand has
        // already been executed (CommandHistory::Execute runs it first, and the
        // transaction path collects already-executed commands).
        std::size_t m_UndoneFrom = 0;
    };

    // Manages a stack of undoable/redoable commands
    class CommandHistory
    {
      public:
        static constexpr std::size_t MaxHistorySize = 128;

        // Called whenever IsDirty() flips, with the new value, after the mutation
        // that flipped it is complete. The automation event bus (#1131) publishes
        // a `scene_dirty` event from here; the editor wires it once for its scene
        // history and leaves panel-local histories unwired. Runs on the thread
        // that mutated the history (the main thread), synchronously, from a
        // destructor: it must not re-enter this object and it must not throw
        // (a throw would terminate the process, which is the honest outcome for
        // a hook that cannot report a scene edit).
        std::function<void(bool dirty)> OnDirtyChanged;

        // Document operations (new/save) establish a checkpoint that belongs to
        // their resulting document. Undo restores the outgoing document's exact
        // checkpoint; ordinary edits retain the existing save-point semantics.
        void Execute(std::unique_ptr<EditorCommand> command, bool markSaved = false)
        {
            const DirtyEdge edge(*this);
            command->Execute();
            if (m_Transaction)
            {
                // Inside a transaction the entry is WITHHELD from the undo stack
                // and accumulated instead; commit publishes the whole group as one.
                // A save point requested here is deferred to commit for the same
                // reason -- an uncommitted transaction has not happened yet.
                m_Transaction->Commands->Add(std::move(command));
                m_Transaction->MarkSaved = m_Transaction->MarkSaved || markSaved;
                return;
            }
            if (!m_RedoStack.empty() && m_SavePointValid && m_SavePointVersion > m_Version)
            {
                m_SavePointValid = false;
            }
            const SavePoint previousSavePoint{ m_SavePointVersion, m_SavePointValid };
            m_UndoStack.push_back({ std::move(command), markSaved ? std::optional(previousSavePoint) : std::nullopt });
            m_RedoStack.clear();
            ++m_Version;
            if (markSaved)
                MarkSavedInternal();

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
            const DirtyEdge edge(*this);
            if (m_Transaction)
            {
                m_Transaction->Commands->Add(std::move(command));
                return;
            }
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
            RefuseInsideTransaction("Undo");
            if (m_UndoStack.empty())
            {
                return;
            }
            const DirtyEdge edge(*this);

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
            RefuseInsideTransaction("Redo");
            if (m_RedoStack.empty())
            {
                return;
            }
            const DirtyEdge edge(*this);

            m_RedoStack.back().Command->Execute();
            auto entry = std::move(m_RedoStack.back());
            m_RedoStack.pop_back();
            ++m_Version;
            if (entry.PreviousSavePoint)
                MarkSavedInternal();
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
            const DirtyEdge edge(*this);
            MarkSavedInternal();
        }

        [[nodiscard]] bool IsDirty() const
        {
            // An OPEN transaction has already applied its members to the scene
            // even though its entry is still withheld from the stack, so the
            // document genuinely differs from its save point. Without this, a
            // step that reports document state mid-batch (olo_scene_status after
            // an olo_entity_create in the same transaction) would answer "clean"
            // about a scene that has changed under it.
            //
            // The undo/redo AVAILABILITY flags deliberately still describe the
            // committed stack: mid-transaction there is nothing a caller could
            // legally undo, because Undo() refuses while a transaction is open.
            if (m_Transaction && !m_Transaction->Commands->IsEmpty())
            {
                return true;
            }
            if (!m_SavePointValid)
            {
                return true;
            }
            return m_Version != m_SavePointVersion;
        }

        // ---- transactions (issue #1127) ----------------------------------
        //
        // A transaction makes N editor operations ONE undo entry, and lets the
        // whole group be discarded if a later one fails. Between Begin and
        // Commit/Rollback, Execute() and PushAlreadyExecuted() still APPLY their
        // command -- the editor state moves exactly as it would without a
        // transaction -- but the undo entry is accumulated instead of pushed.
        //
        // Deliberately NOT implemented by pushing N entries and collapsing them
        // afterwards: MaxHistorySize trimming makes stack-depth arithmetic wrong
        // exactly when the stack is full, and a mid-transaction reader would see
        // N separate entries that are about to stop existing. Withholding them
        // has neither problem.
        //
        // Single-threaded, like the rest of this class: the caller holds the
        // main thread for the whole scope (Automation runs a transaction inside
        // ONE marshaled job), so no editor interaction can slip an unrelated
        // entry into the group.
        void BeginTransaction(std::string description)
        {
            if (m_Transaction)
                throw std::logic_error("A command-history transaction is already open: " + m_Transaction->Commands->GetDescription());
            m_Transaction.emplace(std::move(description), SavePoint{ m_SavePointVersion, m_SavePointValid });
        }

        [[nodiscard]] bool InTransaction() const
        {
            return m_Transaction.has_value();
        }

        // How many undo entries the open transaction has accumulated so far. A
        // transaction whose steps all declined to mutate records zero and
        // commits as nothing at all.
        [[nodiscard]] std::size_t TransactionSize() const
        {
            return m_Transaction ? m_Transaction->Commands->Size() : 0;
        }

        // Publish the accumulated group as ONE undo entry. An empty transaction
        // adds no entry: a batch of read-only steps must not leave a no-op the
        // user has to press Ctrl-Z past.
        void CommitTransaction()
        {
            if (!m_Transaction)
                throw std::logic_error("CommitTransaction: no command-history transaction is open.");
            const DirtyEdge edge(*this);
            Transaction scope = std::move(*m_Transaction);
            m_Transaction.reset();

            const bool markSaved = scope.MarkSaved;
            if (scope.Commands->IsEmpty())
            {
                if (markSaved)
                    MarkSavedInternal();
                return;
            }

            if (!m_RedoStack.empty() && m_SavePointValid && m_SavePointVersion > m_Version)
            {
                m_SavePointValid = false;
            }
            // The entry's "previous save point" is the one in force when the
            // TRANSACTION opened, not the live one: a step may have established
            // a save point of its own mid-transaction (an identical scene save
            // does exactly that), and recording it here would make undoing the
            // batch restore a checkpoint from inside the batch instead of the
            // one before it. Nothing can have trimmed it in between -- no entry
            // was pushed while the transaction was open.
            SavePoint previousSavePoint = scope.EntrySavePoint;
            if (!m_SavePointValid)
                previousSavePoint.Valid = false;
            m_UndoStack.push_back({ std::move(scope.Commands), markSaved ? std::optional(previousSavePoint) : std::nullopt });
            m_RedoStack.clear();
            ++m_Version;
            if (markSaved)
                MarkSavedInternal();

            while (m_UndoStack.size() > MaxHistorySize)
            {
                m_UndoStack.pop_front();
                TrimSavePoint();
            }
        }

        // Take the accumulated group back and discard it, restoring the save
        // point the transaction opened with. Returns the first member whose undo
        // REFUSED, or an empty string when the rollback was complete -- never
        // silently partial. The undo and redo stacks are untouched either way:
        // an aborted transaction did not happen, so it must not clear a redo the
        // user could still reach.
        [[nodiscard]] std::string RollbackTransaction()
        {
            if (!m_Transaction)
                throw std::logic_error("RollbackTransaction: no command-history transaction is open.");
            const DirtyEdge edge(*this);
            Transaction scope = std::move(*m_Transaction);
            m_Transaction.reset();

            std::string failure = scope.Commands->UndoAll();
            m_SavePointVersion = scope.EntrySavePoint.Version;
            m_SavePointValid = scope.EntrySavePoint.Valid;
            return failure;
        }

        void Clear()
        {
            RefuseInsideTransaction("Clear");
            const DirtyEdge edge(*this); // a dirty document cleared is a clean-again edge
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

        struct Transaction
        {
            Transaction(std::string description, SavePoint entrySavePoint)
                : Commands(std::make_unique<CompoundCommand>(std::move(description))), EntrySavePoint(entrySavePoint)
            {
            }

            std::unique_ptr<CompoundCommand> Commands;
            // The save point as it stood when the transaction opened, restored
            // verbatim on rollback.
            SavePoint EntrySavePoint;
            // Whether any member asked to establish a save point. Applied once,
            // at commit.
            bool MarkSaved = false;
        };

        // Undo, Redo and Clear rewrite the very stack an open transaction is
        // accumulating beside, so there is no coherent answer for what they mean
        // mid-transaction. Refuse rather than pick one: under Automation this
        // surfaces as a failed step that rolls the batch back cleanly, and from
        // the editor UI it is unreachable (a transaction holds the main thread).
        void RefuseInsideTransaction(const char* operation) const
        {
            if (m_Transaction)
                throw std::logic_error(std::string(operation) + " is not allowed while a command-history transaction is open.");
        }

        // Sets the save point without firing OnDirtyChanged. Every public
        // mutation holds a DirtyEdge across its whole body and fires once at the
        // end, so an Execute(markSaved) that dirties and immediately cleans in one
        // call reports no edge at all -- the state a subscriber can observe never
        // flipped.
        void MarkSavedInternal()
        {
            m_SavePointVersion = m_Version;
            m_SavePointValid = true;
        }

        // Samples IsDirty() on construction and fires OnDirtyChanged on
        // destruction if it moved. Constructed as the first statement of every
        // public mutation, so nested mutations (a save inside a transaction, a
        // command whose Execute pushes a sub-command) collapse into the outermost
        // edge. A mutation that is unwinding on an exception (a guarded undo that
        // refused, say) fires nothing: the caller sees the throw, and the hook
        // must not run from an unwinding frame.
        class DirtyEdge
        {
          public:
            explicit DirtyEdge(CommandHistory& history)
                : m_History(history), m_WasDirty(history.IsDirty()), m_Depth(history.m_EdgeDepth++),
                  m_Exceptions(std::uncaught_exceptions())
            {
            }
            ~DirtyEdge()
            {
                --m_History.m_EdgeDepth;
                if (m_Depth != 0 || std::uncaught_exceptions() != m_Exceptions)
                    return;
                const bool dirty = m_History.IsDirty();
                if (dirty != m_WasDirty && m_History.OnDirtyChanged)
                    m_History.OnDirtyChanged(dirty);
            }
            DirtyEdge(const DirtyEdge&) = delete;
            DirtyEdge& operator=(const DirtyEdge&) = delete;
            DirtyEdge(DirtyEdge&&) = delete;
            DirtyEdge& operator=(DirtyEdge&&) = delete;

          private:
            CommandHistory& m_History;
            bool m_WasDirty;
            std::size_t m_Depth;
            int m_Exceptions;
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
        std::optional<Transaction> m_Transaction;
        std::size_t m_Version = 0;
        std::size_t m_EdgeDepth = 0; // see DirtyEdge
        std::size_t m_SavePointVersion = 0;
        bool m_SavePointValid = true;
    };
} // namespace OloEngine
