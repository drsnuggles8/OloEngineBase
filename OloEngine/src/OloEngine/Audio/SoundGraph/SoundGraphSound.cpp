#include "OloEnginePCH.h"
#include "SoundGraphSound.h"
#include "SoundGraphSource.h"
#include "SoundGraph.h"
#include "OloEngine/Audio/AudioEngine.h"
#include "OloEngine/Asset/AssetManager.h"

#include <algorithm>

namespace OloEngine
{
    using EPlayState = SoundPlayState;

    //==============================================================================
    SoundGraphSound::SoundGraphSound()
    {
    }

    SoundGraphSound::~SoundGraphSound()
    {
        ReleaseResources();
    }

    bool SoundGraphSound::InitializeAudioCallback()
    {
        m_Source = CreateScope<SoundGraphSource>();
        
        // Initialize the source with default configuration
        if (!m_Source->Init(48000, 512))  // 48kHz, 512 samples buffer
        {
            OLO_CORE_ERROR("Failed to initialize SoundGraphSource");
            return false;
        }

        return true;
    }

    bool SoundGraphSound::InitializeDataSource(const Ref<SoundConfig>& config, AudioEngine* audioEngine, const Ref<Audio::SoundGraph::SoundGraph>& source)
    {
        OLO_CORE_ASSERT(!IsPlaying(), "Cannot initialize while playing");
        OLO_CORE_ASSERT(!m_bIsReadyToPlay, "Already initialized");
        OLO_CORE_ASSERT(source, "SoundGraph source is null");

        // Reset finished flag
        m_bFinished = false;

        if (!config->DataSourceAsset)
        {
            OLO_CORE_ERROR("No data source asset provided");
            return false;
        }

        m_DebugName = "SoundGraphSound_" + std::to_string(config->DataSourceAsset);

        const bool callbackInitialized = InitializeAudioCallback();
        if (!callbackInitialized)
        {
            OLO_CORE_ERROR("Failed to initialize Audio Callback for SoundGraphSound");
            return false;
        }

        // Suspend processing while we set up
        m_Source->SuspendProcessing(true);
        
        // Set the sound graph instance
        m_Source->ReplacePlayer(source);
        
        // Initialize effects chain
        InitializeEffects(config);

        // Set base volume and pitch
        m_Volume = static_cast<f64>(config->VolumeMultiplier);
        m_Pitch = static_cast<f64>(config->PitchMultiplier);

        // Apply volume and pitch to the source
        m_Source->SetVolume(static_cast<f32>(m_Volume));
        m_Source->SetPitch(static_cast<f32>(m_Pitch));

        SetLooping(config->bLooping);
        
        // Resume processing
        m_Source->SuspendProcessing(false);

        m_bIsReadyToPlay = true;
        return m_bIsReadyToPlay;
    }

    void SoundGraphSound::InitializeEffects(const Ref<SoundConfig>& config)
    {
        // Store normalized filter values
        m_LowPassValue = config->LPFilterValue;
        m_HighPassValue = config->HPFilterValue;
        
        // Apply initial filter settings to the source
        if (m_Source)
        {
            m_Source->SetLowPassFilter(m_LowPassValue);
            m_Source->SetHighPassFilter(m_HighPassValue);
        }
    }

    void SoundGraphSound::ReleaseResources()
    {
        if (m_Source)
        {
            m_Source->SuspendProcessing(true);
            m_Source->ReleaseResources();
            m_Source.reset();
        }

        m_bIsReadyToPlay = false;
        m_PlayState = SoundPlayState::Stopped;
        m_NextPlayState = SoundPlayState::Stopped;
    }

    //==============================================================================
    /// Sound Source Interface

    bool SoundGraphSound::Play()
    {
        if (!IsReadyToPlay())
        {
            OLO_CORE_WARN("SoundGraphSound::Play() - Sound is not ready to play");
            return false;
        }

        if (m_PlayState == SoundPlayState::Playing)
        {
            return true; // Already playing
        }

        if (m_Source)
        {
            m_Source->Play();
        }

        m_PlayState = SoundPlayState::Playing;
        m_NextPlayState = SoundPlayState::Playing;
        m_bFinished = false;

        // Notify callback if set
        if (m_OnPlaybackComplete)
        {
            // Store callback for when playback finishes
        }

        return true;
    }

    bool SoundGraphSound::Stop()
    {
        return StopNow(StopOptions::NotifyPlaybackComplete | StopOptions::ResetPlaybackPosition) != -1;
    }

    bool SoundGraphSound::Pause()
    {
        if (m_PlayState != SoundPlayState::Playing)
            return false;

        if (m_Source)
        {
            m_Source->Pause();
        }

        m_PlayState = SoundPlayState::Pausing;
        m_NextPlayState = SoundPlayState::Pausing;
        
        return true;
    }

    bool SoundGraphSound::IsPlaying() const
    {
        return m_PlayState == SoundPlayState::Playing;
    }

    //==============================================================================
    /// Property Setters/Getters

    void SoundGraphSound::SetVolume(f32 newVolume)
    {
        m_Volume = static_cast<f64>(std::clamp(newVolume, 0.0f, 1.0f));
        
        if (m_Source)
        {
            m_Source->SetVolume(static_cast<f32>(m_Volume));
        }
    }

    void SoundGraphSound::SetPitch(f32 newPitch)
    {
        m_Pitch = static_cast<f64>(std::clamp(newPitch, 0.1f, 4.0f)); // Reasonable pitch range
        
        if (m_Source)
        {
            m_Source->SetPitch(static_cast<f32>(m_Pitch));
        }
    }

    void SoundGraphSound::SetLooping(bool looping)
    {
        m_bLooping = looping;
        
        if (m_Source)
        {
            m_Source->SetLooping(looping);
        }
    }

    f32 SoundGraphSound::GetVolume() const
    {
        return static_cast<f32>(m_Volume);
    }

    f32 SoundGraphSound::GetPitch() const
    {
        return static_cast<f32>(m_Pitch);
    }

    void SoundGraphSound::SetLowPassFilter(f32 value)
    {
        m_LowPassValue = std::clamp(value, 0.0f, 1.0f);
        
        if (m_Source)
        {
            m_Source->SetLowPassFilter(m_LowPassValue);
        }
    }

    void SoundGraphSound::SetHighPassFilter(f32 value)
    {
        m_HighPassValue = std::clamp(value, 0.0f, 1.0f);
        
        if (m_Source)
        {
            m_Source->SetHighPassFilter(m_HighPassValue);
        }
    }

    //==============================================================================
    /// Parameter Interface

    void SoundGraphSound::SetParameter(u32 parameterID, f32 value)
    {
        if (m_Source)
        {
            m_Source->SetParameter(parameterID, value);
        }
    }

    void SoundGraphSound::SetParameter(u32 parameterID, i32 value)
    {
        if (m_Source)
        {
            m_Source->SetParameter(parameterID, value);
        }
    }

    void SoundGraphSound::SetParameter(u32 parameterID, bool value)
    {
        if (m_Source)
        {
            m_Source->SetParameter(parameterID, value);
        }
    }

    //==============================================================================
    /// Fade Control

    bool SoundGraphSound::FadeIn(f32 duration, f32 targetVolume)
    {
        if (duration <= 0.0f)
        {
            SetVolume(targetVolume);
            return true;
        }

        m_bIsFading = true;
        m_FadeStartVolume = GetCurrentFadeVolume();
        m_FadeTargetVolume = std::clamp(targetVolume, 0.0f, 1.0f);
        m_FadeDuration = duration;
        m_FadeCurrentTime = 0.0f;

        return true;
    }

    bool SoundGraphSound::FadeOut(f32 duration, f32 targetVolume)
    {
        if (duration <= 0.0f)
        {
            SetVolume(targetVolume);
            if (targetVolume <= 0.0f)
            {
                Stop();
            }
            return true;
        }

        m_bIsFading = true;
        m_FadeStartVolume = GetCurrentFadeVolume();
        m_FadeTargetVolume = std::clamp(targetVolume, 0.0f, 1.0f);
        m_FadeDuration = duration;
        m_FadeCurrentTime = 0.0f;

        return true;
    }

    //==============================================================================
    /// 3D Audio

    void SoundGraphSound::SetLocation(const glm::vec3& location, const glm::vec3& orientation)
    {
        m_Position = location;
        m_Orientation = orientation;
        
        if (m_Source)
        {
            m_Source->SetLocation(location);
            m_Source->SetOrientation(orientation);
        }
    }

    void SoundGraphSound::SetVelocity(const glm::vec3& velocity)
    {
        m_Velocity = velocity;
        
        if (m_Source)
        {
            m_Source->SetVelocity(velocity);
        }
    }

    //==============================================================================
    /// Status and Update

    void SoundGraphSound::Update(Timestep ts)
    {
        f32 deltaTime = ts.GetSeconds();
        
        if (m_bIsFading)
        {
            m_FadeCurrentTime += deltaTime;
            
            if (m_FadeCurrentTime >= m_FadeDuration)
            {
                // Fade completed
                m_bIsFading = false;
                SetVolume(m_FadeTargetVolume);
                
                // If faded to zero, stop playback
                if (m_FadeTargetVolume <= 0.0f)
                {
                    Stop();
                }
            }
            else
            {
                // Interpolate volume
                f32 t = m_FadeCurrentTime / m_FadeDuration;
                f32 currentVolume = m_FadeStartVolume + (m_FadeTargetVolume - m_FadeStartVolume) * t;
                SetVolume(currentVolume);
            }
        }

        // Update stop fade timer
        if (m_StopFadeTime > 0.0)
        {
            m_StopFadeTime -= deltaTime;
            if (m_StopFadeTime <= 0.0)
            {
                StopNow(StopOptions::NotifyPlaybackComplete | StopOptions::ResetPlaybackPosition);
            }
        }

        // Update source
        if (m_Source)
        {
            m_Source->Update(deltaTime);
            
            // Check if source finished playback
            if (m_Source->IsFinished() && !m_bFinished)
            {
                m_bFinished = true;
                m_PlayState = SoundPlayState::Stopped;
                
                if (m_OnPlaybackComplete)
                {
                    m_OnPlaybackComplete();
                }
            }
        }
    }

    bool SoundGraphSound::IsFinished() const
    {
        return m_bFinished && !IsPlaying();
    }

    bool SoundGraphSound::IsStopping() const
    {
        return m_PlayState == SoundPlayState::Stopping;
    }

    f32 SoundGraphSound::GetCurrentFadeVolume() const
    {
        if (m_bIsFading && m_FadeDuration > 0.0f)
        {
            f32 t = std::clamp(m_FadeCurrentTime / m_FadeDuration, 0.0f, 1.0f);
            return m_FadeStartVolume + (m_FadeTargetVolume - m_FadeStartVolume) * t;
        }
        return static_cast<f32>(m_Volume);
    }

    f32 SoundGraphSound::GetPriority() const
    {
        f32 basePriority = static_cast<f32>(m_Priority) / 255.0f; // Normalize to 0-1
        f32 volumeMultiplier = GetCurrentFadeVolume();
        return basePriority * volumeMultiplier;
    }

    f32 SoundGraphSound::GetPlaybackPercentage() const
    {
        if (m_Source)
        {
            return m_Source->GetPlaybackPercentage();
        }
        return 0.0f;
    }

    //==============================================================================
    /// Private Methods

    bool SoundGraphSound::StopFade(u64 numSamples)
    {
        // Convert samples to seconds (assuming 48kHz sample rate)
        f32 fadeTimeSeconds = static_cast<f32>(numSamples) / 48000.0f;
        return StopFade(static_cast<i32>(fadeTimeSeconds * 1000.0f)); // Convert to milliseconds
    }

    bool SoundGraphSound::StopFade(i32 milliseconds)
    {
        if (milliseconds <= 0)
        {
            return StopNow(StopOptions::NotifyPlaybackComplete | StopOptions::ResetPlaybackPosition) != -1;
        }

        m_StopFadeTime = static_cast<f64>(milliseconds) / 1000.0; // Convert to seconds
        m_PlayState = SoundPlayState::Stopping;
        
        // Start fade out
        return FadeOut(static_cast<f32>(m_StopFadeTime), 0.0f);
    }

    i32 SoundGraphSound::StopNow(u16 options)
    {
        if (m_Source)
        {
            m_Source->Stop();
        }

        if (options & StopOptions::ResetPlaybackPosition)
        {
            // Reset playback position to beginning
            if (m_Source)
            {
                m_Source->Reset();
            }
        }

        m_PlayState = SoundPlayState::Stopped;
        m_NextPlayState = SoundPlayState::Stopped;
        m_bIsFading = false;
        m_StopFadeTime = 0.0;

        if (options & StopOptions::NotifyPlaybackComplete)
        {
            m_bFinished = true;
            if (m_OnPlaybackComplete)
            {
                m_OnPlaybackComplete();
            }
        }

        return 0; // Return voice ID (placeholder)
    }

    //==============================================================================
    /// Utility Functions

    f32 SoundGraphSound::NormalizedToFrequency(f32 normalizedValue)
    {
        // Convert normalized value (0-1) to frequency (20Hz - 20kHz)
        normalizedValue = std::clamp(normalizedValue, 0.0f, 1.0f);
        return 20.0f * std::pow(1000.0f, normalizedValue); // Logarithmic scale
    }

    f32 SoundGraphSound::FrequencyToNormalized(f32 frequency)
    {
        // Convert frequency to normalized value (0-1)
        frequency = std::clamp(frequency, 20.0f, 20000.0f);
        return std::log(frequency / 20.0f) / std::log(1000.0f);
    }

} // namespace OloEngine