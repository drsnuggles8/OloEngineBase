#include "OloEnginePCH.h"
#include "SoundGraphSound.h"
#include "SoundGraphSource.h" 
#include "SoundGraph.h"
#include "OloEngine/Audio/AudioEngine.h"
#include "OloEngine/Asset/AssetManager.h"

#include <choc/containers/choc_Value.h>
#include <algorithm>

namespace OloEngine::Audio::SoundGraph
{
    using EPlayState = SoundPlayState;

    //==============================================================================
    SoundGraphSound::SoundGraphSound()
    {
        OLO_PROFILE_FUNCTION();
        // Initialize member variables to safe defaults
        m_PlayState = SoundPlayState::Stopped;
        m_NextPlayState = SoundPlayState::Stopped;
        m_IsReadyToPlay = false;
        m_IsFinished = false;
        m_IsLooping = false;
        m_Volume = 1.0f;
        m_Pitch = 1.0f;
        m_IsFading = false;
        m_Priority = 128;
    }

    SoundGraphSound::~SoundGraphSound()
    {
        OLO_PROFILE_FUNCTION();
        ReleaseResources();
    }

    bool SoundGraphSound::InitializeAudioCallback()
    {
        OLO_PROFILE_FUNCTION();
        m_Source = CreateScope<Audio::SoundGraph::SoundGraphSource>();
        return true;
    }

    // Additional overload to match header declaration
    bool SoundGraphSound::InitializeFromGraph(const Ref<Audio::SoundGraph::SoundGraph>& soundGraph)
    {
        // Validate input parameters
        if (!soundGraph)
        {
            OLO_CORE_ERROR("SoundGraphSound::InitializeFromGraph - Invalid soundGraph provided");
            return false;
        }

        // Ensure we have a valid source to work with
        if (!m_Source)
        {
            OLO_CORE_ERROR("SoundGraphSound::InitializeFromGraph - No source available, call InitializeAudioCallback first");
            return false;
        }

        // Wire the sound graph into the source
        try
        {
            m_Source->ReplaceGraph(soundGraph);
            // Only set ready state if the source accepted the graph successfully
            m_IsReadyToPlay = true;
            return true;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("SoundGraphSound::InitializeFromGraph - Failed to replace graph: {}", e.what());
            m_IsReadyToPlay = false;
            return false;
        }
        catch (...)
        {
            OLO_CORE_ERROR("SoundGraphSound::InitializeFromGraph - Unknown error occurred while replacing graph");
            m_IsReadyToPlay = false;
            return false;
        }
    }

    bool SoundGraphSound::InitializeDataSource(const std::vector<AssetHandle>& dataSources, const Ref<Audio::SoundGraph::SoundGraph>& soundGraph)  
    {
        // Validate input parameters
        if (dataSources.empty())
        {
            OLO_CORE_ERROR("SoundGraphSound::InitializeDataSource - No data sources provided");
            return false;
        }

        if (!soundGraph)
        {
            OLO_CORE_ERROR("SoundGraphSound::InitializeDataSource - Invalid soundGraph provided");
            return false;
        }

        // Ensure we have a valid source to work with
        if (!m_Source)
        {
            OLO_CORE_ERROR("SoundGraphSound::InitializeDataSource - No source available, call InitializeAudioCallback first");
            return false;
        }

        // Validate all asset handles before proceeding
        for (const auto& handle : dataSources)
        {
            if (handle == 0) // UUID 0 is invalid/null handle
            {
                OLO_CORE_ERROR("SoundGraphSound::InitializeDataSource - Invalid asset handle in data sources");
                return false;
            }
        }

        // Initialize data sources first
        if (!m_Source->InitializeDataSources(dataSources))
        {
            OLO_CORE_ERROR("SoundGraphSound::InitializeDataSource - Failed to initialize data sources");
            m_IsReadyToPlay = false;
            return false;
        }

        // Wire the sound graph into the source
        try
        {
            m_Source->ReplaceGraph(soundGraph);
            // Only set ready state if both operations succeeded
            m_IsReadyToPlay = true;
            return true;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("SoundGraphSound::InitializeDataSource - Failed to replace graph: {}", e.what());
            // Clean up data sources since graph setup failed
            m_Source->UninitializeDataSources();
            m_IsReadyToPlay = false;
            return false;
        }
        catch (...)
        {
            OLO_CORE_ERROR("SoundGraphSound::InitializeDataSource - Unknown error occurred while replacing graph");
            // Clean up data sources since graph setup failed
            m_Source->UninitializeDataSources();
            m_IsReadyToPlay = false;
            return false;
        }
    }

    void SoundGraphSound::ReleaseResources()
    {
        OLO_PROFILE_FUNCTION();

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
        // Cancel any active fades
        m_IsFading = false;
        m_FadeCurrentTime = 0.0f;
        m_FadeDuration = 0.0f;
        m_FadeStartVolume = m_Volume;
        m_FadeTargetVolume = m_Volume;
        
        // Set play state and finished flag
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
        OLO_PROFILE_FUNCTION();

        m_Volume = std::clamp(newVolume, 0.0f, 1.0f);
        // Note: Actual volume control would be implemented via SoundGraph parameters
    }

    void SoundGraphSound::SetPitch(f32 newPitch)
    {
        OLO_PROFILE_FUNCTION();

        m_Pitch = std::clamp(newPitch, 0.1f, 4.0f);
        // Note: Actual pitch control would be implemented via SoundGraph parameters
    }

    void SoundGraphSound::SetLooping(bool looping)
    {
        OLO_PROFILE_FUNCTION();

        m_IsLooping = looping;
        // Note: Actual looping would be implemented via SoundGraph parameters
    }

    f32 SoundGraphSound::GetVolume() const
    {
        OLO_PROFILE_FUNCTION();

        return m_Volume;
    }

    f32 SoundGraphSound::GetPitch() const
    {
        OLO_PROFILE_FUNCTION();

        return m_Pitch;
    }

    void SoundGraphSound::SetLowPassFilter(f32 value)
    {
        OLO_PROFILE_FUNCTION();

        m_LowPassValue = std::clamp(value, 0.0f, 1.0f);
        // Note: Actual filtering would be implemented via SoundGraph parameters
    }

    void SoundGraphSound::SetHighPassFilter(f32 value)
    {
        OLO_PROFILE_FUNCTION();

        m_HighPassValue = std::clamp(value, 0.0f, 1.0f);
        // Note: Actual filtering would be implemented via SoundGraph parameters
    }

    //==============================================================================
    /// Parameter Interface - ONLY call SetParameter if source exists and is valid

    void SoundGraphSound::SetParameter(u32 parameterID, f32 value)
    {
        OLO_PROFILE_FUNCTION();
        
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
        OLO_PROFILE_FUNCTION();

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
        OLO_PROFILE_FUNCTION();

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
        OLO_PROFILE_FUNCTION();

        if (duration <= 0.0f)
            return false;

        m_IsFading = true;
        m_FadeStartVolume = m_Volume;
        m_FadeTargetVolume = std::clamp(targetVolume, 0.0f, 1.0f);
        m_FadeDuration = duration;
        m_FadeCurrentTime = 0.0f;

        return true;
    }

    bool SoundGraphSound::FadeOut(f32 duration, f32 targetVolume)
    {
        OLO_PROFILE_FUNCTION();
        if (duration <= 0.0f)
            return false;

        m_IsFading = true;
        m_FadeStartVolume = m_Volume;
        m_FadeTargetVolume = std::clamp(targetVolume, 0.0f, 1.0f);
        m_FadeDuration = duration;
        m_FadeCurrentTime = 0.0f;

        return true;
    }

    //==============================================================================
    /// 3D Audio - Stub implementations that just store values

    void SoundGraphSound::SetLocation(const glm::vec3& location)
    {
        OLO_PROFILE_FUNCTION();
        m_Position = location;
    }

    void SoundGraphSound::SetVelocity(const glm::vec3& velocity)
    {
        OLO_PROFILE_FUNCTION();
        m_Velocity = velocity;
    }

    void SoundGraphSound::SetOrientation(const glm::vec3& forward, const glm::vec3& up)
    {
        OLO_PROFILE_FUNCTION();
        (void)up;
        m_Orientation = forward;
    }
    
    //==============================================================================
    /// Status and Update

    void SoundGraphSound::Update(f32 deltaTime)
    {
        OLO_PROFILE_FUNCTION();
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
        OLO_PROFILE_FUNCTION();

        return m_IsFinished;
    }

    f32 SoundGraphSound::GetCurrentPriority() const
    {
        OLO_PROFILE_FUNCTION();
        
        f32 basePriority = static_cast<f32>(m_Priority) / 255.0f;
        f32 volumeMultiplier = m_Volume;
        
        if (m_IsFading && m_FadeDuration > 0.0f)
        {
            f32 t = std::clamp(m_FadeCurrentTime / m_FadeDuration, 0.0f, 1.0f);
            volumeMultiplier = m_FadeStartVolume + (m_FadeTargetVolume - m_FadeStartVolume) * t;
        }
        
        return basePriority * volumeMultiplier;
    }

    f32 SoundGraphSound::GetPlaybackPercentage() const
    {
        OLO_PROFILE_FUNCTION();
        if (!m_Source)
            return 0.0f;

        u64 maxFrames = m_Source->GetMaxTotalFrames();
        if (maxFrames == 0)
            return 0.0f;

        u64 currentFrame = m_Source->GetCurrentFrame();
        return static_cast<f32>(currentFrame) / static_cast<f32>(maxFrames);
    }

    //==============================================================================
    /// Private Methods - All stub implementations

    bool SoundGraphSound::StopFade(u64 numSamples)
    {
        OLO_PROFILE_FUNCTION();
        // Get actual sample rate from the source, fallback to 48000 if not available
        u32 sampleRate = m_Source ? m_Source->GetSampleRate() : 48000;
        if (sampleRate == 0)
            sampleRate = 48000; // Safety fallback if source hasn't been initialized
        
        // Compute milliseconds using f64 to avoid overflow
        f64 milliseconds = static_cast<f64>(numSamples) * 1000.0 / static_cast<f64>(sampleRate);
        
        // Clamp to valid i32 range before calling the overload
        constexpr f64 maxInt32 = static_cast<f64>(INT32_MAX);
        if (milliseconds > maxInt32)
        {
            milliseconds = maxInt32;
        }
        else if (milliseconds < 0.0)
        {
            milliseconds = 0.0;
        }
        
        return StopFade(static_cast<i32>(milliseconds));
    }

    bool SoundGraphSound::StopFade(i32 milliseconds)
    {
        OLO_PROFILE_FUNCTION();
        if (milliseconds <= 0)
        {
            return StopNow() >= 0;
        }
        return FadeOut(milliseconds / 1000.0f, 0.0f);
    }

    i32 SoundGraphSound::StopNow(StopOptions options)
    {
        OLO_PROFILE_FUNCTION();
        m_PlayState = SoundPlayState::Stopped;
        m_IsFading = false;
        m_IsFinished = true;
        
        if ((options & StopOptions::ResetPlaybackPosition) != StopOptions::None)
        {
            // Reset logic would go here
        }

        if ((options & StopOptions::NotifyPlaybackComplete) != StopOptions::None)
        {
            if (m_OnPlaybackComplete)
                m_OnPlaybackComplete();
        }

        return 0; // Return dummy voice ID
    }

    f32 SoundGraphSound::NormalizedToFrequency(f32 normalizedValue)
    {
        OLO_PROFILE_FUNCTION();
        f32 minFreq = 20.0f;
        f32 maxFreq = 20000.0f;
        return minFreq + (maxFreq - minFreq) * normalizedValue;
    }

    f32 SoundGraphSound::FrequencyToNormalized(f32 frequency)
    {
        OLO_PROFILE_FUNCTION();
        f32 minFreq = 20.0f;
        f32 maxFreq = 20000.0f;
        return std::clamp((frequency - minFreq) / (maxFreq - minFreq), 0.0f, 1.0f);
    }

} // namespace OloEngine