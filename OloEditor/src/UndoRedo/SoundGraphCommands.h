#pragma once

#include "EditorCommand.h"
#include "OloEngine/Asset/SoundGraphAsset.h"
#include "OloEngine/Core/Ref.h"

#include <functional>
#include <string>
#include <utility>

namespace OloEngine
{
    // Snapshot-based undo/redo for the SoundGraph editor. Mirrors the animation graph
    // command shape: store full Ref<SoundGraphAsset> deep clones for both old and new
    // state, and let the panel-owned apply callback swap the snapshot into the panel's
    // active asset. The command clones again on every Execute/Undo so repeated
    // round-trips through the stack don't drift the canonical snapshots.
    class SoundGraphChangeCommand : public EditorCommand
    {
      public:
        using ApplyFn = std::function<void(Ref<SoundGraphAsset>)>;

        SoundGraphChangeCommand(Ref<SoundGraphAsset> oldState, Ref<SoundGraphAsset> newState,
                                ApplyFn applyFn, std::string description = "Sound Graph Change")
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
        Ref<SoundGraphAsset> m_OldState;
        Ref<SoundGraphAsset> m_NewState;
        ApplyFn m_ApplyFn;
        std::string m_Description;
    };
} // namespace OloEngine
