#include "OloEnginePCH.h"
#include "OloEngine/Audio/AudioEvents/AudioPlayback.h"
#include "OloEngine/Audio/AudioEvents/AudioEventsManager.h"

namespace OloEngine::Audio
{
    AudioEventsManager* AudioPlayback::s_Manager = nullptr;

    u64 AudioPlayback::PostTrigger(CommandID command, u64 objectID)
    {
        if (!s_Manager)
        {
            OLO_CORE_WARN("AudioPlayback: Events manager not initialized");
            return 0;
        }
        return s_Manager->PostTrigger(command, objectID);
    }

    u64 AudioPlayback::PostTriggerByName(std::string_view eventName, u64 objectID)
    {
        return PostTrigger(CommandID::FromString(eventName), objectID);
    }

    void AudioPlayback::StopEvent(u64 eventID)
    {
        if (s_Manager)
        {
            s_Manager->StopEvent(eventID);
        }
    }

    void AudioPlayback::PauseEvent(u64 eventID)
    {
        if (s_Manager)
        {
            s_Manager->PauseEvent(eventID);
        }
    }

    void AudioPlayback::ResumeEvent(u64 eventID)
    {
        if (s_Manager)
        {
            s_Manager->ResumeEvent(eventID);
        }
    }

    void AudioPlayback::StopAll()
    {
        if (s_Manager)
        {
            s_Manager->StopAllEvents();
        }
    }

    bool AudioPlayback::IsEventActive(u64 eventID)
    {
        if (!s_Manager)
        {
            return false;
        }
        return s_Manager->IsEventActive(eventID);
    }

    void AudioPlayback::SetManager(AudioEventsManager* manager)
    {
        s_Manager = manager;
    }

} // namespace OloEngine::Audio
