#include "OloEnginePCH.h"
#include "OloEngine/Audio/AudioEvents/AudioEventsManager.h"
#include "OloEngine/Audio/AudioEvents/AudioCommandRegistry.h"
#include "OloEngine/Project/Project.h"

#include <algorithm>

namespace OloEngine::Audio
{
    void AudioEventsManager::Init(AudioCommandRegistry* registry)
    {
        m_Registry = registry;
        m_NextEventID = 1;
        m_PendingEvents.clear();
        m_ActiveEvents.clear();
        m_PositionResolver = nullptr;
    }

    void AudioEventsManager::Shutdown()
    {
        StopAllEvents();
        m_Registry = nullptr;
        m_PositionResolver = nullptr;
    }

    void AudioEventsManager::SetPositionResolver(PositionResolver resolver)
    {
        m_PositionResolver = std::move(resolver);
    }

    u64 AudioEventsManager::PostTrigger(CommandID command, u64 objectID)
    {
        if (!command.IsValid())
        {
            OLO_CORE_WARN("AudioEventsManager: Attempted to post invalid CommandID");
            return 0;
        }

        if (!m_Registry)
        {
            OLO_CORE_WARN("AudioEventsManager: No registry set");
            return 0;
        }

        u64 eventID = m_NextEventID++;
        m_PendingEvents.push_back({ eventID, command, objectID });
        return eventID;
    }

    void AudioEventsManager::Update(Timestep ts)
    {
        if (!m_Registry)
        {
            return;
        }

        // Process pending events
        auto pendingCopy = std::move(m_PendingEvents);
        m_PendingEvents.clear();

        for (const auto& eventInfo : pendingCopy)
        {
            auto* cmd = m_Registry->GetTrigger(eventInfo.Command);
            if (!cmd)
            {
                OLO_CORE_WARN("AudioEventsManager: No trigger found for CommandID {}", eventInfo.Command.ID);
                continue;
            }

            ActiveEvent active;
            active.EventID = eventInfo.EventID;
            active.ObjectID = eventInfo.ObjectID;

            for (auto& action : cmd->Actions)
            {
                ExecuteAction(action, eventInfo.ObjectID, eventInfo.EventID);

                // For Play actions, create an AudioSource and track it in the active event
                if (action.Type == ActionType::Play && !action.AudioFilepath.empty())
                {
                    auto path = Project::GetAssetFileSystemPath(action.AudioFilepath);
                    if (std::filesystem::exists(path))
                    {
                        auto source = Ref<AudioSource>::Create(path.string().c_str());
                        source->SetVolume(action.VolumeMultiplier);
                        source->SetPitch(action.PitchMultiplier);
                        source->SetLooping(action.Looping);

                        // Set initial position from entity
                        if (glm::vec3 pos{}; eventInfo.ObjectID != 0 && m_PositionResolver && m_PositionResolver(eventInfo.ObjectID, pos))
                        {
                            source->SetPosition(pos);
                        }

                        source->Play();
                        active.Sources.push_back(std::move(source));
                    }
                    else
                    {
                        OLO_CORE_WARN("AudioEventsManager: Audio file not found: {}", path.string());
                    }
                }
            }

            // Only track if there are active sources to manage
            if (!active.Sources.empty())
            {
                m_ActiveEvents.push_back(std::move(active));
            }
        }

        // Update spatial positions for active event sources
        if (m_PositionResolver)
        {
            for (auto& active : m_ActiveEvents)
            {
                if (active.ObjectID == 0)
                {
                    continue;
                }
                if (glm::vec3 pos{}; m_PositionResolver(active.ObjectID, pos))
                {
                    for (const auto& src : active.Sources)
                    {
                        src->SetPosition(pos);
                    }
                }
            }
        }

        // Clean up finished events (non-looping sources that finished playing)
        std::erase_if(m_ActiveEvents, [](ActiveEvent& active)
                      {
            std::erase_if(active.Sources, [](const Ref<AudioSource>& src)
            {
                return !src->IsPlaying();
            });
            return active.Sources.empty(); });
    }

    void AudioEventsManager::ExecuteAction(const TriggerAction& action, u64 objectID, [[maybe_unused]] u64 eventID)
    {
        switch (action.Type)
        {
            case ActionType::Play:
                // Handled in Update() after this call — source creation needs the action data
                break;

            case ActionType::Stop:
            {
                // Stop all active sources matching the audio filepath (Global context)
                for (auto& active : m_ActiveEvents)
                {
                    if (action.Context == ActionContext::GameObject && active.ObjectID != objectID)
                    {
                        continue;
                    }
                    for (const auto& src : active.Sources)
                    {
                        src->Stop();
                    }
                }
                break;
            }

            case ActionType::Pause:
            {
                for (auto& active : m_ActiveEvents)
                {
                    if (action.Context == ActionContext::GameObject && active.ObjectID != objectID)
                    {
                        continue;
                    }
                    for (const auto& src : active.Sources)
                    {
                        src->Pause();
                    }
                }
                break;
            }

            case ActionType::Resume:
            {
                for (auto& active : m_ActiveEvents)
                {
                    if (action.Context == ActionContext::GameObject && active.ObjectID != objectID)
                    {
                        continue;
                    }
                    for (const auto& src : active.Sources)
                    {
                        src->UnPause();
                    }
                }
                break;
            }
        }
    }

    void AudioEventsManager::StopEvent(u64 eventID)
    {
        for (auto& active : m_ActiveEvents)
        {
            if (active.EventID == eventID)
            {
                for (const auto& src : active.Sources)
                {
                    src->Stop();
                }
                break;
            }
        }
    }

    void AudioEventsManager::PauseEvent(u64 eventID)
    {
        for (auto& active : m_ActiveEvents)
        {
            if (active.EventID == eventID)
            {
                for (const auto& src : active.Sources)
                {
                    src->Pause();
                }
                break;
            }
        }
    }

    void AudioEventsManager::ResumeEvent(u64 eventID)
    {
        for (auto& active : m_ActiveEvents)
        {
            if (active.EventID == eventID)
            {
                for (const auto& src : active.Sources)
                {
                    src->UnPause();
                }
                break;
            }
        }
    }

    void AudioEventsManager::StopAllEvents()
    {
        for (auto& active : m_ActiveEvents)
        {
            for (const auto& src : active.Sources)
            {
                src->Stop();
            }
        }
        m_ActiveEvents.clear();
        m_PendingEvents.clear();
    }

    bool AudioEventsManager::IsEventActive(u64 eventID) const
    {
        return std::ranges::any_of(m_ActiveEvents, [eventID](const ActiveEvent& e)
                                   { return e.EventID == eventID; });
    }

    u32 AudioEventsManager::GetActiveEventCount() const
    {
        return static_cast<u32>(m_ActiveEvents.size());
    }

} // namespace OloEngine::Audio
