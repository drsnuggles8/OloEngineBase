#pragma once

#include "OloEngine/Audio/AudioEvents/CommandID.h"

#include <string>
#include <vector>

namespace OloEngine::Audio
{
    enum class ActionType : u8
    {
        Play,
        Stop,
        Pause,
        Resume
    };

    enum class ActionContext : u8
    {
        GameObject, // Scoped to the triggering entity
        Global      // Affects all instances of the target audio
    };

    struct TriggerAction
    {
        ActionType Type = ActionType::Play;
        ActionContext Context = ActionContext::GameObject;

        std::string AudioFilepath; // Path to the audio file this action targets

        f32 VolumeMultiplier = 1.0f;
        f32 PitchMultiplier = 1.0f;
        bool Looping = false;

        bool Handled = false; // Internal: tracks whether this action has been dispatched
    };

    struct TriggerCommand
    {
        std::string DebugName; // Human-readable: "PlayFootsteps"
        CommandID ID;          // CRC32 of DebugName
        std::vector<TriggerAction> Actions;

        void ResetHandledFlags()
        {
            for (auto& action : Actions)
            {
                action.Handled = false;
            }
        }
    };

} // namespace OloEngine::Audio
