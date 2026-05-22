#pragma once

#include "EditorCommand.h"
#include "OloEngine/Animation/AnimationGraph.h"
#include "OloEngine/Core/Ref.h"

#include <functional>
#include <string>
#include <utility>

namespace OloEngine
{
    // =========================================================================
    // Animation graph editor undo — snapshot-based, stores full Ref<AnimationGraph>
    // deep clones for both old and new state. The apply callback is responsible
    // for swapping the stored snapshot into the live component; the command
    // clones again on each Execute/Undo so the canonical snapshots remain
    // pristine across repeated round-trips through the undo/redo stack.
    // =========================================================================
    class AnimationGraphChangeCommand : public EditorCommand
    {
      public:
        using ApplyFn = std::function<void(Ref<AnimationGraph>)>;

        AnimationGraphChangeCommand(Ref<AnimationGraph> oldState, Ref<AnimationGraph> newState,
                                    ApplyFn applyFn, std::string description = "Animation Graph Change")
            : m_OldState(std::move(oldState)), m_NewState(std::move(newState)),
              m_ApplyFn(std::move(applyFn)), m_Description(std::move(description))
        {
        }

        void Execute() override
        {
            if (m_ApplyFn && m_NewState)
            {
                m_ApplyFn(m_NewState->Clone());
            }
        }

        void Undo() override
        {
            if (m_ApplyFn && m_OldState)
            {
                m_ApplyFn(m_OldState->Clone());
            }
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return m_Description;
        }

      private:
        Ref<AnimationGraph> m_OldState;
        Ref<AnimationGraph> m_NewState;
        ApplyFn m_ApplyFn;
        std::string m_Description;
    };
} // namespace OloEngine
