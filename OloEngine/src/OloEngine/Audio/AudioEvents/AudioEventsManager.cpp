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

        if (!m_Registry->Contains(command))
        {
            OLO_CORE_WARN("AudioEventsManager: CommandID {} not found in registry", command.ID);
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

            // Ensure an ActiveEvent entry exists so ExecuteAction can find it
            auto& active = m_ActiveEvents[eventInfo.EventID];
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
                        active.Sources.push_back({ action.AudioFilepath, std::move(source) });
                    }
                    else
                    {
                        OLO_CORE_WARN("AudioEventsManager: Audio file not found: {}", path.string());
                    }
                }
            }

            // Remove entry if no sources were created (Stop-only triggers, etc.)
            if (active.Sources.empty())
            {
                m_ActiveEvents.erase(eventInfo.EventID);
            }
        }

        // Update spatial positions for active event sources
        if (m_PositionResolver)
        {
            for (auto& [id, active] : m_ActiveEvents)
            {
                if (active.ObjectID == 0)
                {
                    continue;
                }
                if (glm::vec3 pos{}; m_PositionResolver(active.ObjectID, pos))
                {
                    for (const auto& entry : active.Sources)
                    {
                        entry.Source->SetPosition(pos);
                    }
                }
            }
        }

        // Clean up finished events (non-looping sources that finished playing)
        std::erase_if(m_ActiveEvents, [](auto& pair)
                      {
            auto& active = pair.second;
            std::erase_if(active.Sources, [](const auto& entry)
            {
                return !entry.Source->IsPlaying();
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
                for (auto& [id, active] : m_ActiveEvents)
                {
                    if (action.Context == ActionContext::GameObject && active.ObjectID != objectID)
                    {
                        continue;
                    }
                    for (const auto& entry : active.Sources)
                    {
                        if (!action.AudioFilepath.empty() && entry.Filepath != action.AudioFilepath)
                        {
                            continue;
                        }
                        entry.Source->Stop();
                    }
                }
                break;
            }

            case ActionType::Pause:
            {
                for (auto& [id, active] : m_ActiveEvents)
                {
                    if (action.Context == ActionContext::GameObject && active.ObjectID != objectID)
                    {
                        continue;
                    }
                    for (const auto& entry : active.Sources)
                    {
                        if (!action.AudioFilepath.empty() && entry.Filepath != action.AudioFilepath)
                        {
                            continue;
                        }
                        entry.Source->Pause();
                    }
                }
                break;
            }

            case ActionType::Resume:
            {
                for (auto& [id, active] : m_ActiveEvents)
                {
                    if (action.Context == ActionContext::GameObject && active.ObjectID != objectID)
                    {
                        continue;
                    }
                    for (const auto& entry : active.Sources)
                    {
                        if (!action.AudioFilepath.empty() && entry.Filepath != action.AudioFilepath)
                        {
                            continue;
                        }
                        entry.Source->UnPause();
                    }
                }
                break;
            }

            default:
                OLO_CORE_WARN("AudioEventsManager::ExecuteAction: Unknown ActionType {} for objectID={}, eventID={}",
                              static_cast<int>(action.Type), objectID, eventID);
                break;
        }
    }

    void AudioEventsManager::StopEvent(u64 eventID)
    {
        // Cancel pending events before they fire in Update()
        std::erase_if(m_PendingEvents, [eventID](const EventInfo& info)
                      { return info.EventID == eventID; });

        if (auto it = m_ActiveEvents.find(eventID); it != m_ActiveEvents.end())
        {
            for (const auto& entry : it->second.Sources)
            {
                entry.Source->Stop();
            }
            m_ActiveEvents.erase(it);
        }
    }

    void AudioEventsManager::PauseEvent(u64 eventID)
    {
        if (auto it = m_ActiveEvents.find(eventID); it != m_ActiveEvents.end())
        {
            for (const auto& entry : it->second.Sources)
            {
                entry.Source->Pause();
            }
        }
    }

    void AudioEventsManager::ResumeEvent(u64 eventID)
    {
        if (auto it = m_ActiveEvents.find(eventID); it != m_ActiveEvents.end())
        {
            for (const auto& entry : it->second.Sources)
            {
                entry.Source->UnPause();
            }
        }
    }

    void AudioEventsManager::StopAllEvents()
    {
        for (auto& [id, active] : m_ActiveEvents)
        {
            for (const auto& entry : active.Sources)
            {
                entry.Source->Stop();
            }
        }
        m_ActiveEvents.clear();
        m_PendingEvents.clear();
    }

    bool AudioEventsManager::IsEventActive(u64 eventID) const
    {
        return m_ActiveEvents.contains(eventID);
    }

    u32 AudioEventsManager::GetActiveEventCount() const
    {
        return static_cast<u32>(m_ActiveEvents.size());
    }

} // namespace OloEngine::Audio
