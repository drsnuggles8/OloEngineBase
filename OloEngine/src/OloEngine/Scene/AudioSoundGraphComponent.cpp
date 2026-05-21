#include "OloEnginePCH.h"
#include "Components.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphSound.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphSource.h"

#include <choc/containers/choc_Value.h>

// AudioSoundGraphComponent gameplay parameter helpers. Implementations live here so the
// Components.h header doesn't need to drag SoundGraphSource.h / miniaudio into every TU
// that touches a component (which is most of the engine). The Sound pointer is only set
// by Scene::InitAudioRuntime once the graph has compiled + instantiated; before that
// these helpers return false so gameplay can detect not-yet-playable graphs.

namespace OloEngine
{
    namespace
    {
        Audio::SoundGraph::SoundGraphSource* GetLiveSource(const AudioSoundGraphComponent& comp)
        {
            if (!comp.Sound)
                return nullptr;
            return comp.Sound->GetSource();
        }
    }

    bool AudioSoundGraphComponent::SetParameter(const std::string& name, f32 value)
    {
        auto* source = GetLiveSource(*this);
        if (!source)
            return false;
        return source->SetParameter(name, choc::value::createFloat32(value));
    }

    bool AudioSoundGraphComponent::SetParameter(const std::string& name, i32 value)
    {
        auto* source = GetLiveSource(*this);
        if (!source)
            return false;
        return source->SetParameter(name, choc::value::createInt32(value));
    }

    bool AudioSoundGraphComponent::SetParameter(const std::string& name, bool value)
    {
        auto* source = GetLiveSource(*this);
        if (!source)
            return false;
        return source->SetParameter(name, choc::value::createBool(value));
    }
} // namespace OloEngine
