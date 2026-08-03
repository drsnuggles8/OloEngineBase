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
        // Hand the slot back before the backend object dies — the budget holds a raw
        // pointer to this host and would otherwise drive a destroyed source.
        ReleaseVoice();
        UninitializeDSP();
        ::ma_sound_uninit(m_Sound.get());
        m_Sound = nullptr;
    }

    Audio::VoiceParams AudioSource::BuildVoiceParams() const
    {
        Audio::VoiceParams params;
        params.Priority = m_Config.Priority;
        params.Volume = m_Config.VolumeMultiplier;
        params.Pitch = m_Config.PitchMultiplier;
        params.Looping = m_Config.Looping;
        params.Spatialized = m_Spatialization;
        params.Position = m_Position;
        params.MinDistance = m_Config.MinDistance;
        params.MaxDistance = m_Config.MaxDistance;
        params.DurationSeconds = GetLengthSeconds();
        return params;
    }

    void AudioSource::ReleaseVoice() const
    {
        // Exchange, not load-then-store: two threads racing to release must not both hand
        // the same handle back (the second Release would be a no-op today, but the pattern
        // is the one that stays correct if handles are ever recycled).
        const Audio::VoiceHandle handle = m_VoiceHandle.exchange(Audio::kInvalidVoiceHandle, std::memory_order_acq_rel);
        if (handle != Audio::kInvalidVoiceHandle)
        {
            Audio::VoiceManager::Get().Release(handle);
        }
    }

    void AudioSource::SyncVoiceParams() const
    {
        const Audio::VoiceHandle handle = m_VoiceHandle.load(std::memory_order_relaxed);
        if (handle != Audio::kInvalidVoiceHandle)
        {
            Audio::VoiceManager::Get().UpdateParams(handle, BuildVoiceParams());
        }
    }

    void AudioSource::Play() const
    {
        // Re-triggering an already-registered source restarts it: drop the old voice so
        // the budget doesn't end up holding two records for one backend sound (the second
        // of which it could never stop).
        ReleaseVoice();
        const Audio::VoiceHandle handle = Audio::VoiceManager::Get().Acquire(this, BuildVoiceParams());
        m_VoiceHandle.store(handle, std::memory_order_release);
        if (handle == Audio::kInvalidVoiceHandle)
        {
            // Budget unavailable (only possible if Acquire was handed a null host, which
            // cannot happen here) — fall back to unmanaged playback rather than silence.
            ::ma_sound_seek_to_pcm_frame(m_Sound.get(), 0);
            ::ma_sound_start(m_Sound.get());
        }
        // Otherwise Acquire has already driven OnVoiceStart if this voice won a slot;
        // if it did not, the source is virtual and starts when one frees.
    }

    void AudioSource::Pause() const
    {
        // The voice stays registered — a paused source is still logically owned by its
        // entity and must resume where it left off — but it is marked paused so the budget
        // hands its slot to something audible AND, crucially, never calls OnVoiceStart on
        // it. Without the paused flag the budget could promote this voice and restart
        // playback the game explicitly stopped.
        ::ma_sound_stop(m_Sound.get());
        const Audio::VoiceHandle handle = m_VoiceHandle.load(std::memory_order_relaxed);
        if (handle != Audio::kInvalidVoiceHandle)
        {
            Audio::VoiceManager::Get().SetVoicePaused(handle, true);
        }
    }

    void AudioSource::UnPause() const
    {
        const Audio::VoiceHandle handle = m_VoiceHandle.load(std::memory_order_relaxed);
        if (handle == Audio::kInvalidVoiceHandle)
        {
            // Never registered (budget unavailable) — unmanaged resume.
            ::ma_sound_start(m_Sound.get());
            return;
        }

        // Clearing the pause re-enters the contest. If this voice lost its slot while
        // paused and now wins one back, the budget drives OnVoiceStart itself (seeking to
        // the retained position).
        Audio::VoiceManager::Get().SetVoicePaused(handle, false);

        // But a voice that was paused while UNCONTENDED never left VoiceState::Playing —
        // nobody wanted its slot, so the budget had no transition to emit and will emit
        // none now. Nothing would restart the backend that Pause() stopped, leaving the
        // source silently dead. Restarting here covers that case; for a voice the budget
        // just promoted this is a harmless second start on an already-running sound.
        if (Audio::VoiceManager::Get().IsAudible(handle))
        {
            ::ma_sound_start(m_Sound.get());
        }
    }

    void AudioSource::Stop() const
    {
        ReleaseVoice();
        ::ma_sound_stop(m_Sound.get());
        ::ma_sound_seek_to_pcm_frame(m_Sound.get(), 0);
    }

    bool AudioSource::IsPlaying() const
    {
        // Virtualized counts as playing — see the header. A stolen looping ambience is
        // still "playing" from the game's point of view; only the mixer disagrees.
        const Audio::VoiceHandle handle = m_VoiceHandle.load(std::memory_order_acquire);
        if (handle != Audio::kInvalidVoiceHandle && Audio::VoiceManager::Get().IsActive(handle))
        {
            return true;
        }
        return ::ma_sound_is_playing(m_Sound.get());
    }

    bool AudioSource::IsAudible() const
    {
        const Audio::VoiceHandle handle = m_VoiceHandle.load(std::memory_order_acquire);
        if (handle == Audio::kInvalidVoiceHandle)
        {
            return ::ma_sound_is_playing(m_Sound.get());
        }
        return Audio::VoiceManager::Get().IsAudible(handle);
    }

    bool AudioSource::IsVirtualized() const
    {
        const Audio::VoiceHandle handle = m_VoiceHandle.load(std::memory_order_acquire);
        return handle != Audio::kInvalidVoiceHandle && Audio::VoiceManager::Get().IsVirtual(handle);
    }

    f64 AudioSource::GetLengthSeconds() const
    {
        if (m_LengthSecondsCache >= 0.0)
        {
            return m_LengthSecondsCache;
        }

        f32 length = 0.0f;
        if (::ma_sound_get_length_in_seconds(m_Sound.get(), &length) != MA_SUCCESS || !std::isfinite(length) || length < 0.0f)
        {
            // 0 = "unknown length" to the budget: such a voice is never auto-retired, so a
            // clip whose length the backend cannot report is simply owned by its caller.
            m_LengthSecondsCache = 0.0;
        }
        else
        {
            m_LengthSecondsCache = static_cast<f64>(length);
        }
        return m_LengthSecondsCache;
    }

    f64 AudioSource::GetPlaybackPositionSeconds() const
    {
        const Audio::VoiceHandle handle = m_VoiceHandle.load(std::memory_order_acquire);
        if (handle != Audio::kInvalidVoiceHandle && Audio::VoiceManager::Get().IsActive(handle))
        {
            return Audio::VoiceManager::Get().GetPlaybackPosition(handle);
        }
        return OnVoiceQueryPosition();
    }

    bool AudioSource::OnVoiceStart(f64 positionSeconds) const
    {
        // Resume at the retained position rather than from zero — this is what makes a
        // devirtualized loop come back in phase instead of restarting (issue #730,
        // acceptance criterion 2).
        const u32 sampleRate = ::ma_engine_get_sample_rate(static_cast<ma_engine*>(AudioEngine::GetEngine()));
        const f64 safePosition = (std::isfinite(positionSeconds) && positionSeconds > 0.0) ? positionSeconds : 0.0;
        const u64 frame = (sampleRate > 0) ? static_cast<u64>(safePosition * static_cast<f64>(sampleRate)) : 0ull;
        ::ma_sound_seek_to_pcm_frame(m_Sound.get(), frame);
        return ::ma_sound_start(m_Sound.get()) == MA_SUCCESS;
    }

    f64 AudioSource::OnVoiceStop() const
    {
        // Read the cursor BEFORE stopping: miniaudio leaves it where it was, but reading
        // first keeps this correct regardless of any future stop-resets-cursor behavior.
        const f64 position = OnVoiceQueryPosition();
        ::ma_sound_stop(m_Sound.get());
        return position;
    }

    f64 AudioSource::OnVoiceQueryPosition() const
    {
        f32 cursor = 0.0f;
        if (::ma_sound_get_cursor_in_seconds(m_Sound.get(), &cursor) != MA_SUCCESS || !std::isfinite(cursor) || cursor < 0.0f)
        {
            return -1.0;
        }
        return static_cast<f64>(cursor);
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
        // Mirror the config so the voice budget can re-score this source at any time
        // without reaching back through the owning component. The push to the budget
        // happens at the END of this function, not here: BuildVoiceParams reads
        // m_Spatialization, which is only updated further down, so syncing now would
        // publish the previous spatialization flag alongside the new config.
        m_Config = config;

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

        // DSP filter parameters — only init chain if parameters deviate from bypass defaults
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

        // If DSP is already initialized, always push current values
        if (m_DSPInitialized)
        {
            SetLowPassCutoff(config.LowPassCutoff);
            SetHighPassCutoff(config.HighPassCutoff);
            SetReverbSend(config.ReverbSend);
        }

        // Last: m_Spatialization is settled by now, so the budget gets the whole new
        // config in one consistent push.
        SyncVoiceParams();
    }

    void AudioSource::SetPriority(const f32 priority) const
    {
        m_Config.Priority = std::isfinite(priority) ? std::clamp(priority, 0.0f, 1.0f) : 0.5f;
        SyncVoiceParams();
    }

    void AudioSource::SetVolume(const f32 volume) const
    {
        // Volume reaches here from YAML, script and the network, and it now feeds the
        // voice score as well as the mixer — so reject a non-finite value rather than
        // letting it through (CLAUDE.md / cpp-coding-quality §2, same convention as
        // SetPriority and SoundGraphSound::SetVolume).
        if (!std::isfinite(volume))
        {
            OLO_CORE_WARN("AudioSource::SetVolume - ignoring non-finite volume; keeping {}", m_Config.VolumeMultiplier);
            return;
        }

        // Gain feeds the voice score, so every mutator that the budget ranks on has to
        // re-sync — a source faded to silence should become the next steal victim.
        m_Config.VolumeMultiplier = volume;
        SyncVoiceParams();
        ::ma_sound_set_volume(m_Sound.get(), volume);
    }

    void AudioSource::SetPitch(const f32 pitch) const
    {
        if (!std::isfinite(pitch))
        {
            OLO_CORE_WARN("AudioSource::SetPitch - ignoring non-finite pitch; keeping {}", m_Config.PitchMultiplier);
            return;
        }

        m_Config.PitchMultiplier = pitch;
        SyncVoiceParams();
        ::ma_sound_set_pitch(m_Sound.get(), pitch);
    }

    void AudioSource::SetLooping(const bool state) const
    {
        m_Config.Looping = state;
        SyncVoiceParams();
        ::ma_sound_set_looping(m_Sound.get(), state);
    }

    void AudioSource::SetSpatialization(const bool state)
    {
        m_Spatialization = state;
        m_Config.Spatialization = state;
        SyncVoiceParams();
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
        m_Config.MinDistance = minDistance;
        SyncVoiceParams();
        ::ma_sound_set_min_distance(m_Sound.get(), minDistance);
    }

    void AudioSource::SetMaxDistance(const f32 maxDistance) const
    {
        m_Config.MaxDistance = maxDistance;
        SyncVoiceParams();
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
        // Scene::UpdateAudio pushes this every frame for every source; keeping the mirror
        // fresh is what makes distance-based stealing track a moving listener/emitter
        // instead of ranking on wherever the sound was when it started.
        m_Position = position;
        SyncVoiceParams();
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

        auto* soundNode = &m_Sound->engineNode.baseNode;

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

        u32 numChannels = ::ma_node_get_output_channels(chainTail, 0);
        numChannels = std::max(numChannels, 2u); // Ensure at least stereo for reverb send
        ma_splitter_node_config splitterConfig = ::ma_splitter_node_config_init(numChannels);

        m_SplitterNode = new ::ma_splitter_node();
        ma_result result = ::ma_splitter_node_init(
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
            ma_uint8 oldInputBus = chainTail->pOutputBuses[0].inputNodeInputBusIndex;

            // Splitter bus 0 (dry) → old destination
            result = ::ma_node_attach_output_bus(m_SplitterNode, 0, oldOutput, oldInputBus);
            if (result != MA_SUCCESS)
            {
                OLO_CORE_ERROR("[AudioSource] Splitter dry-bus attach failed for: {}", m_Path);
                ::ma_splitter_node_uninit(AsSplitter(m_SplitterNode),
                                          &engine->pResourceManager->config.allocationCallbacks);
                delete AsSplitter(m_SplitterNode);
                m_SplitterNode = nullptr;
            }

            if (m_SplitterNode)
            {
                // chainTail → splitter input
                result = ::ma_node_attach_output_bus(chainTail, 0, m_SplitterNode, 0);
                if (result != MA_SUCCESS)
                {
                    OLO_CORE_ERROR("[AudioSource] Chain-to-splitter attach failed for: {}", m_Path);
                    ::ma_splitter_node_uninit(AsSplitter(m_SplitterNode),
                                              &engine->pResourceManager->config.allocationCallbacks);
                    delete AsSplitter(m_SplitterNode);
                    m_SplitterNode = nullptr;
                }
            }

            if (m_SplitterNode)
            {
                // Bus 0 volume = 1.0 (main output)
                ::ma_node_set_output_bus_volume(m_SplitterNode, 0, 1.0f);
                // Bus 1 volume = 0.0 (muted until reverb send is set)
                ::ma_node_set_output_bus_volume(m_SplitterNode, 1, 0.0f);

                // Connect bus 1 to master reverb if available
                auto* masterReverb = AudioEngine::GetMasterReverb();
                if (masterReverb && masterReverb->GetNode())
                {
                    result = ::ma_node_attach_output_bus(m_SplitterNode, 1, masterReverb->GetNode(), 0);
                    if (result != MA_SUCCESS)
                    {
                        OLO_CORE_WARN("[AudioSource] Failed to attach reverb send for: {}", m_Path);
                    }
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
            const auto* engine = static_cast<ma_engine*>(AudioEngine::GetEngine());
            ::ma_splitter_node_uninit(AsSplitter(m_SplitterNode),
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
            sendLevel = std::clamp(sendLevel, 0.0f, 1.0f);
            ::ma_node_set_output_bus_volume(m_SplitterNode, 1, sendLevel);
        }
    }
} // namespace OloEngine
