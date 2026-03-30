#pragma once

#include "OloEngine/Audio/AudioEvents/CommandID.h"

#include <string_view>

namespace OloEngine::Audio
{
    class AudioEventsManager;

    /// Static convenience API for posting audio events from game/script code.
    class AudioPlayback
    {
      public:
        /// Post a trigger event by CommandID. Returns a unique EventID for later control.
        static u64 PostTrigger(CommandID command, u64 objectID = 0);

        /// Post a trigger event by name (computes CRC32 internally). Returns a unique EventID.
        static u64 PostTriggerByName(std::string_view eventName, u64 objectID = 0);

        /// Stop a specific active event.
        static void StopEvent(u64 eventID);

        /// Pause a specific active event.
        static void PauseEvent(u64 eventID);

        /// Resume a specific active event.
        static void ResumeEvent(u64 eventID);

        /// Stop all active events.
        static void StopAll();

        /// Check whether a specific event is still active.
        [[nodiscard]] static bool IsEventActive(u64 eventID);

        /// Set the backing manager (called by Scene on runtime start/stop).
        static void SetManager(AudioEventsManager* manager);

      private:
        friend class AudioEventsManager;
        static AudioEventsManager* s_Manager;
    };

} // namespace OloEngine::Audio
