#pragma once

#include "OloEngine/Audio/AudioEvents/AudioCommands.h"
#include "OloEngine/Audio/AudioSource.h"
#include "OloEngine/Core/Timestep.h"

#include <functional>
#include <glm/vec3.hpp>
#include <unordered_map>
#include <vector>

namespace OloEngine::Audio
{
    class AudioCommandRegistry;

    struct EventInfo
    {
        u64 EventID = 0;   // Auto-incrementing unique ID for this event instance
        CommandID Command; // Which trigger command to invoke
        u64 ObjectID = 0;  // Entity UUID that posted the event (0 = global)
    };

    /// Runtime dispatcher: receives posted events, resolves them to commands, executes actions.
    class AudioEventsManager
    {
      public:
        /// Callback: given an objectID (entity UUID), writes position into outPos.
        /// Returns true if entity exists, false otherwise.
        using PositionResolver = std::function<bool(u64 objectID, glm::vec3& outPos)>;

        void Init(AudioCommandRegistry* registry);
        void Shutdown();

        /// Set callback for resolving entity positions (called by Scene on init).
        void SetPositionResolver(PositionResolver resolver);

        /// Post an event (called from game/script layer, queued for processing).
        /// Returns a unique EventID for later control.
        u64 PostTrigger(CommandID command, u64 objectID = 0);

        /// Process queued events (called once per frame from Scene::OnUpdateRuntime).
        void Update(Timestep ts);

        /// Stop a specific active event and its associated audio sources.
        void StopEvent(u64 eventID);

        /// Pause a specific active event's audio sources.
        void PauseEvent(u64 eventID);

        /// Resume a specific active event's audio sources.
        void ResumeEvent(u64 eventID);

        /// Stop all active events and clean up.
        void StopAllEvents();

        /// Check whether a specific event is still active.
        [[nodiscard]] bool IsEventActive(u64 eventID) const;

        /// Get the number of currently active events.
        [[nodiscard]] u32 GetActiveEventCount() const;

      private:
        void ExecuteAction(const TriggerAction& action, u64 objectID, u64 eventID);

        AudioCommandRegistry* m_Registry = nullptr;
        PositionResolver m_PositionResolver;
        std::vector<EventInfo> m_PendingEvents;

        struct ActiveEvent
        {
            u64 EventID = 0;
            u64 ObjectID = 0;

            struct SourceEntry
            {
                std::string Filepath; // Relative asset path used to create the source
                Ref<AudioSource> Source;
            };
            std::vector<SourceEntry> Sources;
        };
        std::unordered_map<u64, ActiveEvent> m_ActiveEvents;

        u64 m_NextEventID = 1;
    };

} // namespace OloEngine::Audio
