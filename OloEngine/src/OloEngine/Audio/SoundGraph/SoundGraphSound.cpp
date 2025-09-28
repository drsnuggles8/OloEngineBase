#include "OloEnginePCH.h"
#include "SoundGraphSound.h"
#include "SoundGraphSource.h" 
#include "SoundGraph.h"
#include "OloEngine/Audio/AudioEngine.h"
#include "OloEngine/Asset/AssetManager.h"

#include <choc/containers/choc_Value.h>
#include <algorithm>

namespace OloEngine
{
    using EPlayState = SoundPlayState;

    //==============================================================================
    SoundGraphSound::SoundGraphSound()
    {
        // Initialize member variables to safe defaults
        m_PlayState = SoundPlayState::Stopped;
        m_NextPlayState = SoundPlayState::Stopped;
    m_IsReadyToPlay = false;
    m_IsFinished = false;
    m_IsLooping = false;
        m_Volume = 1.0;
        m_Pitch = 1.0;
    m_IsFading = false;
        m_Priority = 128;
    }

    SoundGraphSound::~SoundGraphSound()
    {
        ReleaseResources();
    }

    bool SoundGraphSound::InitializeAudioCallback()
    {
        m_Source = CreateScope<Audio::SoundGraph::SoundGraphSource>();
        return true;
    }

    // Additional overload to match header declaration
    bool SoundGraphSound::InitializeFromGraph(const Ref<Audio::SoundGraph::SoundGraph>& soundGraph)
    {
        // Stub implementation
        if (!soundGraph)
            return false;
    m_IsReadyToPlay = true;
        return true;
    }

    bool SoundGraphSound::InitializeDataSource(const std::vector<AssetHandle>& dataSources, const Ref<Audio::SoundGraph::SoundGraph>& soundGraph)  
    {
        // Stub implementation
        if (dataSources.empty() || !soundGraph)
            return false;
    m_IsReadyToPlay = true;
        return true;
    }

    void SoundGraphSound::ReleaseResources()
    {
        if (m_Source)
        {
            // Clean up - no method calls on m_Source since they don't exist
            m_Source.reset();
        }
    m_IsReadyToPlay = false;
    m_IsFinished = true;
    }

    //==============================================================================
    /// Main Sound Interface

    bool SoundGraphSound::Play()
    {
    if (!m_IsReadyToPlay)
            return false;

        m_PlayState = SoundPlayState::Playing;
    m_IsFinished = false;
        return true;
    }

    bool SoundGraphSound::Stop()
    {
        m_PlayState = SoundPlayState::Stopped;
    m_IsFinished = true;
        return true;
    }

    bool SoundGraphSound::Pause()
    {
        if (m_PlayState == SoundPlayState::Playing)
        {
            m_PlayState = SoundPlayState::Pausing;
            return true;
        }
        return false;
    }

    bool SoundGraphSound::IsPlaying() const
    {
    return m_PlayState == SoundPlayState::Playing && !m_IsFinished;
    }

    //==============================================================================
    /// Volume and Pitch Control

    void SoundGraphSound::SetVolume(f32 newVolume)
    {
        m_Volume = static_cast<f64>(std::clamp(newVolume, 0.0f, 1.0f));
        // Note: Actual volume control would be implemented via SoundGraph parameters
    }

    void SoundGraphSound::SetPitch(f32 newPitch)
    {
        m_Pitch = static_cast<f64>(std::clamp(newPitch, 0.1f, 4.0f));
        // Note: Actual pitch control would be implemented via SoundGraph parameters
    }

    void SoundGraphSound::SetLooping(bool looping)
    {
    m_IsLooping = looping;
        // Note: Actual looping would be implemented via SoundGraph parameters
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
        // Note: Actual filtering would be implemented via SoundGraph parameters
    }

    void SoundGraphSound::SetHighPassFilter(f32 value)
    {
        m_HighPassValue = std::clamp(value, 0.0f, 1.0f);
        // Note: Actual filtering would be implemented via SoundGraph parameters
    }

    //==============================================================================
    /// Parameter Interface - ONLY call SetParameter if source exists and is valid

    void SoundGraphSound::SetParameter(u32 parameterID, f32 value)
    {
        if (m_Source)
        {
            // Only call if we can verify the method exists via the source's SetParameter method
            try
            {
                m_Source->SetParameter(parameterID, choc::value::createFloat32(value));
            }
            catch (...)
            {
                // Ignore errors - this is a stub implementation
            }
        }
    }

    void SoundGraphSound::SetParameter(u32 parameterID, i32 value)
    {
        if (m_Source)
        {
            try
            {
                m_Source->SetParameter(parameterID, choc::value::createInt32(value));
            }
            catch (...)
            {
                // Ignore errors - this is a stub implementation
            }
        }
    }

    void SoundGraphSound::SetParameter(u32 parameterID, bool value)
    {
        if (m_Source)
        {
            try
            {
                m_Source->SetParameter(parameterID, choc::value::createBool(value));
            }
            catch (...)
            {
                // Ignore errors - this is a stub implementation
            }
        }
    }

    //==============================================================================
    /// Fade Control

    bool SoundGraphSound::FadeIn(f32 duration, f32 targetVolume)
    {
        if (duration <= 0.0f)
            return false;

    m_IsFading = true;
        m_FadeStartVolume = static_cast<f32>(m_Volume);
        m_FadeTargetVolume = std::clamp(targetVolume, 0.0f, 1.0f);
        m_FadeDuration = duration;
        m_FadeCurrentTime = 0.0f;

        return true;
    }

    bool SoundGraphSound::FadeOut(f32 duration, f32 targetVolume)
    {
        if (duration <= 0.0f)
            return false;

    m_IsFading = true;
        m_FadeStartVolume = static_cast<f32>(m_Volume);
        m_FadeTargetVolume = std::clamp(targetVolume, 0.0f, 1.0f);
        m_FadeDuration = duration;
        m_FadeCurrentTime = 0.0f;

        return true;
    }

    //==============================================================================
    /// 3D Audio - Stub implementations that just store values

    void SoundGraphSound::SetLocation(const glm::vec3& location)
    {
        m_Position = location;
        m_Location = location;
    }

    void SoundGraphSound::SetVelocity(const glm::vec3& velocity)
    {
        m_Velocity = velocity;
    }

	void SoundGraphSound::SetOrientation(const glm::vec3& forward, const glm::vec3& up)
	{
		(void)up;
		m_Orientation = forward; // Simplified: just use forward vector
	}    //==============================================================================
    /// Status and Update

    void SoundGraphSound::Update(f32 deltaTime)
    {
        // Handle fading
    if (m_IsFading)
        {
            m_FadeCurrentTime += deltaTime;
            
            if (m_FadeCurrentTime >= m_FadeDuration)
            {
                // Fade completed
                m_IsFading = false;
                SetVolume(m_FadeTargetVolume);
                
                // If faded to zero, stop playback
                if (m_FadeTargetVolume <= 0.0f)
                {
                    Stop();
                }
            }
            else
            {
                // Update fade volume
                f32 t = m_FadeCurrentTime / m_FadeDuration;
                f32 currentVolume = m_FadeStartVolume + (m_FadeTargetVolume - m_FadeStartVolume) * t;
                SetVolume(currentVolume);
            }
        }

        // Update source if available, but handle the fact that methods might not exist
        if (m_Source)
        {
            // Try to call Update - our SoundGraphSource does have this method
            try
            {
                m_Source->Update(static_cast<f64>(deltaTime));
                
                // Try to check if finished - our SoundGraphSource does have this method  
                if (m_Source->IsFinished() && !m_IsFinished)
                {
                    m_IsFinished = true;
                    if (m_OnPlaybackComplete)
                        m_OnPlaybackComplete();
                }
            }
            catch (...)
            {
                // Ignore errors - this is a stub implementation
            }
        }
    }

    bool SoundGraphSound::IsFinished() const
    {
    return m_IsFinished;
    }

    f32 SoundGraphSound::GetCurrentPriority() const
    {
        f32 basePriority = static_cast<f32>(m_Priority) / 255.0f;
        f32 volumeMultiplier = static_cast<f32>(m_Volume);
        
    if (m_IsFading && m_FadeDuration > 0.0f)
        {
            f32 t = std::clamp(m_FadeCurrentTime / m_FadeDuration, 0.0f, 1.0f);
            volumeMultiplier = m_FadeStartVolume + (m_FadeTargetVolume - m_FadeStartVolume) * t;
        }
        
        return basePriority * volumeMultiplier;
    }

    f32 SoundGraphSound::GetPlaybackPercentage() const
    {
        // Stub - always return 0% for now
        return 0.0f;
    }

    //==============================================================================
    /// Private Methods - All stub implementations

    bool SoundGraphSound::StopFade(u64 numSamples)
    {
        i32 milliseconds = static_cast<i32>((numSamples * 1000) / 44100);
        return StopFade(milliseconds);
    }

    bool SoundGraphSound::StopFade(i32 milliseconds)
    {
        if (milliseconds <= 0)
        {
            return StopNow() >= 0;
        }
        return FadeOut(milliseconds / 1000.0f, 0.0f);
    }

    i32 SoundGraphSound::StopNow(u16 options)
    {
        m_PlayState = SoundPlayState::Stopped;
    m_IsFading = false;
        
        if (options & StopOptions::ResetPlaybackPosition)
        {
            // Reset logic would go here
        }

        if (options & StopOptions::NotifyPlaybackComplete)
        {
            if (m_OnPlaybackComplete)
                m_OnPlaybackComplete();
        }

        return 0; // Return dummy voice ID
    }

    f32 SoundGraphSound::NormalizedToFrequency(f32 normalizedValue)
    {
        f32 minFreq = 20.0f;
        f32 maxFreq = 20000.0f;
        return minFreq + (maxFreq - minFreq) * normalizedValue;
    }

    f32 SoundGraphSound::FrequencyToNormalized(f32 frequency)
    {
        f32 minFreq = 20.0f;
        f32 maxFreq = 20000.0f;
        return std::clamp((frequency - minFreq) / (maxFreq - minFreq), 0.0f, 1.0f);
    }

} // namespace OloEngine