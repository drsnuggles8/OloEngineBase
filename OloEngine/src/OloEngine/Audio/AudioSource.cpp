#include "OloEnginePCH.h"
#include "AudioSource.h"

#include "miniaudio.h"

#include "AudioEngine.h"
#include "OloEngine/Audio/DSP/LowPassFilter.h"
#include "OloEngine/Audio/DSP/HighPassFilter.h"
#include "OloEngine/Audio/DSP/Reverb.h"

namespace OloEngine
{
    static ma_splitter_node* AsSplitter(void* p)
    {
        return static_cast<ma_splitter_node*>(p);
    }
    AudioSource::AudioSource(const char* filepath)
        : m_Path(filepath)
    {
        m_Sound = CreateScope<ma_sound>();

        ma_result result = ::ma_sound_init_from_file((ma_engine*)AudioEngine::GetEngine(), filepath, MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, nullptr, m_Sound.get());
        if (result != MA_SUCCESS)
        {
            OLO_CORE_ERROR("Failed to initialize sound: {}", filepath);
        }
    }

    AudioSource::~AudioSource()
    {
        UninitializeDSP();
        ::ma_sound_uninit(m_Sound.get());
        m_Sound = nullptr;
    }

    void AudioSource::Play() const
    {
        ::ma_sound_seek_to_pcm_frame(m_Sound.get(), 0);
        ::ma_sound_start(m_Sound.get());
    }

    void AudioSource::Pause() const
    {
        ::ma_sound_stop(m_Sound.get());
    }

    void AudioSource::UnPause() const
    {
        ::ma_sound_start(m_Sound.get());
    }

    void AudioSource::Stop() const
    {
        ::ma_sound_stop(m_Sound.get());
        ::ma_sound_seek_to_pcm_frame(m_Sound.get(), 0);
    }

    bool AudioSource::IsPlaying() const
    {
        return ::ma_sound_is_playing(m_Sound.get());
    }

    [[nodiscard("Store this!")]] static ma_attenuation_model GetAttenuationModel(const AttenuationModelType model)
    {
        switch (model)
        {
            case AttenuationModelType::None:
                return ma_attenuation_model_none;
            case AttenuationModelType::Inverse:
                return ma_attenuation_model_inverse;
            case AttenuationModelType::Linear:
                return ma_attenuation_model_linear;
            case AttenuationModelType::Exponential:
                return ma_attenuation_model_exponential;
        }

        return ma_attenuation_model_none;
    }

    void AudioSource::SetConfig(const AudioSourceConfig& config)
    {
        ma_sound* sound = m_Sound.get();
        ::ma_sound_set_volume(sound, config.VolumeMultiplier);
        ::ma_sound_set_pitch(sound, config.PitchMultiplier);
        ::ma_sound_set_looping(sound, config.Looping);

        if (m_Spatialization != config.Spatialization)
        {
            m_Spatialization = config.Spatialization;
            ::ma_sound_set_spatialization_enabled(sound, config.Spatialization);
        }

        if (config.Spatialization)
        {
            ::ma_sound_set_attenuation_model(sound, GetAttenuationModel(config.AttenuationModel));
            ::ma_sound_set_rolloff(sound, config.RollOff);
            ::ma_sound_set_min_gain(sound, config.MinGain);
            ::ma_sound_set_max_gain(sound, config.MaxGain);
            ::ma_sound_set_min_distance(sound, config.MinDistance);
            ::ma_sound_set_max_distance(sound, config.MaxDistance);

            ::ma_sound_set_cone(sound, config.ConeInnerAngle, config.ConeOuterAngle, config.ConeOuterGain);
            ::ma_sound_set_doppler_factor(sound, glm::max(config.DopplerFactor, 0.0f));
        }
        else
        {
            ::ma_sound_set_attenuation_model(sound, ma_attenuation_model_none);
        }

        // DSP filter parameters
        if (config.LowPassCutoff < 1.0f)
        {
            SetLowPassCutoff(config.LowPassCutoff);
        }
        if (config.HighPassCutoff > 0.0f)
        {
            SetHighPassCutoff(config.HighPassCutoff);
        }
        if (config.ReverbSend > 0.0f)
        {
            SetReverbSend(config.ReverbSend);
        }
    }

    void AudioSource::SetVolume(const f32 volume) const
    {
        ::ma_sound_set_volume(m_Sound.get(), volume);
    }

    void AudioSource::SetPitch(const f32 pitch) const
    {
        ::ma_sound_set_pitch(m_Sound.get(), pitch);
    }

    void AudioSource::SetLooping(const bool state) const
    {
        ::ma_sound_set_looping(m_Sound.get(), state);
    }

    void AudioSource::SetSpatialization(const bool state)
    {
        m_Spatialization = state;
        ::ma_sound_set_spatialization_enabled(m_Sound.get(), state);
    }

    void AudioSource::SetAttenuationModel(const AttenuationModelType type) const
    {
        if (m_Spatialization)
        {
            ::ma_sound_set_attenuation_model(m_Sound.get(), GetAttenuationModel(type));
        }
        else
        {
            ::ma_sound_set_attenuation_model(m_Sound.get(), GetAttenuationModel(AttenuationModelType::None));
        }
    }

    void AudioSource::SetRollOff(const f32 rollOff) const
    {
        ::ma_sound_set_rolloff(m_Sound.get(), rollOff);
    }

    void AudioSource::SetMinGain(const f32 minGain) const
    {
        ::ma_sound_set_min_gain(m_Sound.get(), minGain);
    }

    void AudioSource::SetMaxGain(const f32 maxGain) const
    {
        ::ma_sound_set_max_gain(m_Sound.get(), maxGain);
    }

    void AudioSource::SetMinDistance(const f32 minDistance) const
    {
        ::ma_sound_set_min_distance(m_Sound.get(), minDistance);
    }

    void AudioSource::SetMaxDistance(const f32 maxDistance) const
    {
        ::ma_sound_set_max_distance(m_Sound.get(), maxDistance);
    }

    void AudioSource::SetCone(const f32 innerAngle, const f32 outerAngle, const f32 outerGain) const
    {
        ::ma_sound_set_cone(m_Sound.get(), innerAngle, outerAngle, outerGain);
    }

    void AudioSource::SetDopplerFactor(const f32 factor) const
    {
        ::ma_sound_set_doppler_factor(m_Sound.get(), glm::max(factor, 0.0f));
    }

    void AudioSource::SetPosition(const glm::vec3& position) const
    {
        ::ma_sound_set_position(m_Sound.get(), position.x, position.y, position.z);
    }

    void AudioSource::SetDirection(const glm::vec3& forward) const
    {
        ::ma_sound_set_direction(m_Sound.get(), forward.x, forward.y, forward.z);
    }

    void AudioSource::SetVelocity(const glm::vec3& velocity) const
    {
        ::ma_sound_set_velocity(m_Sound.get(), velocity.x, velocity.y, velocity.z);
    }

    void AudioSource::InitializeDSP()
    {
        if (m_DSPInitialized)
        {
            return;
        }

        auto* engine = static_cast<ma_engine*>(AudioEngine::GetEngine());
        if (!engine)
        {
            return;
        }

        auto* soundNode = reinterpret_cast<ma_node_base*>(m_Sound.get());

        // Insert LPF after the sound node
        m_LowPassFilter = CreateScope<Audio::DSP::LowPassFilter>();
        if (!m_LowPassFilter->Initialize(engine, soundNode))
        {
            OLO_CORE_ERROR("[AudioSource] Failed to initialize low-pass filter for: {}", m_Path);
            m_LowPassFilter = nullptr;
        }

        // Insert HPF after LPF (or after sound if LPF failed)
        auto* insertAfter = m_LowPassFilter ? m_LowPassFilter->GetNode() : soundNode;
        m_HighPassFilter = CreateScope<Audio::DSP::HighPassFilter>();
        if (!m_HighPassFilter->Initialize(engine, insertAfter))
        {
            OLO_CORE_ERROR("[AudioSource] Failed to initialize high-pass filter for: {}", m_Path);
            m_HighPassFilter = nullptr;
        }

        // Insert splitter after the last filter for reverb send routing
        // Chain: Sound → LPF → HPF → Splitter → Bus 0 (endpoint), Bus 1 (master reverb)
        auto* chainTail = m_HighPassFilter  ? m_HighPassFilter->GetNode()
                          : m_LowPassFilter ? m_LowPassFilter->GetNode()
                                            : soundNode;

        u32 numChannels = ma_node_get_output_channels(chainTail, 0);
        ma_splitter_node_config splitterConfig = ma_splitter_node_config_init(numChannels);

        m_SplitterNode = new ma_splitter_node();
        ma_result result = ma_splitter_node_init(
            chainTail->pNodeGraph,
            &splitterConfig,
            &engine->pResourceManager->config.allocationCallbacks,
            AsSplitter(m_SplitterNode));

        if (result != MA_SUCCESS)
        {
            OLO_CORE_ERROR("[AudioSource] Failed to init splitter node for: {}", m_Path);
            delete AsSplitter(m_SplitterNode);
            m_SplitterNode = nullptr;
        }
        else
        {
            // Store the node that chainTail was connected to (the endpoint)
            auto* oldOutput = chainTail->pOutputBuses[0].pInputNode;

            // Splitter bus 0 (dry) → old destination
            result = ma_node_attach_output_bus(m_SplitterNode, 0, oldOutput, 0);
            OLO_CORE_ASSERT(result == MA_SUCCESS);

            // chainTail → splitter input
            result = ma_node_attach_output_bus(chainTail, 0, m_SplitterNode, 0);
            OLO_CORE_ASSERT(result == MA_SUCCESS);

            // Bus 0 volume = 1.0 (main output)
            ma_node_set_output_bus_volume(m_SplitterNode, 0, 1.0f);
            // Bus 1 volume = 0.0 (muted until reverb send is set)
            ma_node_set_output_bus_volume(m_SplitterNode, 1, 0.0f);

            // Connect bus 1 to master reverb if available
            auto* masterReverb = AudioEngine::GetMasterReverb();
            if (masterReverb && masterReverb->GetNode())
            {
                result = ma_node_attach_output_bus(m_SplitterNode, 1, masterReverb->GetNode(), 0);
                if (result != MA_SUCCESS)
                {
                    OLO_CORE_WARN("[AudioSource] Failed to attach reverb send for: {}", m_Path);
                }
            }
        }

        m_DSPInitialized = true;
        OLO_CORE_TRACE("[AudioSource] DSP chain initialized for: {}", m_Path);
    }

    void AudioSource::UninitializeDSP()
    {
        // Uninitialize in reverse order
        if (m_SplitterNode)
        {
            auto* engine = static_cast<ma_engine*>(AudioEngine::GetEngine());
            ma_splitter_node_uninit(AsSplitter(m_SplitterNode),
                                    engine ? &engine->pResourceManager->config.allocationCallbacks : nullptr);
            delete AsSplitter(m_SplitterNode);
            m_SplitterNode = nullptr;
        }
        if (m_HighPassFilter)
        {
            m_HighPassFilter->Uninitialize();
            m_HighPassFilter = nullptr;
        }
        if (m_LowPassFilter)
        {
            m_LowPassFilter->Uninitialize();
            m_LowPassFilter = nullptr;
        }
        m_DSPInitialized = false;
    }

    void AudioSource::SetLowPassCutoff(f32 normalizedCutoff)
    {
        if (!m_DSPInitialized)
        {
            InitializeDSP();
        }
        if (m_LowPassFilter)
        {
            m_LowPassFilter->SetCutoffValue(static_cast<double>(normalizedCutoff));
        }
    }

    void AudioSource::SetHighPassCutoff(f32 normalizedCutoff)
    {
        if (!m_DSPInitialized)
        {
            InitializeDSP();
        }
        if (m_HighPassFilter)
        {
            m_HighPassFilter->SetCutoffValue(static_cast<double>(normalizedCutoff));
        }
    }

    void AudioSource::SetReverbSend(f32 sendLevel)
    {
        if (!m_DSPInitialized)
        {
            InitializeDSP();
        }
        if (m_SplitterNode)
        {
            ma_node_set_output_bus_volume(m_SplitterNode, 1, sendLevel);
        }
    }
} // namespace OloEngine
