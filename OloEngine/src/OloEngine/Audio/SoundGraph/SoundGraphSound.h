#pragma once

#include "OloEngine/Audio/AudioSource.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Asset/Asset.h"  // For AssetHandle
#include <glm/glm.hpp>
#include <string>
#include <functional>
#include <vector>

// Forward declarations
namespace OloEngine
{
	class AudioEngine;

	namespace Audio::SoundGraph
	{
		class SoundGraphSource;
		class SoundGraph;
	}

	enum class SoundPlayState
	{
		Stopped = 0,
		Playing,
		Pausing,
		Stopping
	};

	struct SoundConfig
	{
		AssetHandle DataSourceAsset = 0;
		f32 VolumeMultiplier = 1.0f;
		f32 PitchMultiplier = 1.0f;
		bool bLooping = false;
		bool bSpatializationEnabled = false;
		
		// Filter values (0.0 - 1.0, normalized)
		f32 LPFilterValue = 1.0f;  // 1.0 = no filtering
		f32 HPFilterValue = 0.0f;  // 0.0 = no filtering
		
		bool PlayOnAwake = true;
		u8 Priority = 128;  // 0 = highest, 255 = lowest
	};

	/**
	 * Abstract base interface for audio playback objects
	 */
	class IPlayableAudio : public RefCounted
	{
	public:
		virtual ~IPlayableAudio() = default;
		
		// Core playback interface
		virtual bool Play() = 0;
		virtual bool Stop() = 0;
		virtual bool Pause() = 0;
		virtual bool IsPlaying() const = 0;
		
		// Volume and pitch control
		virtual void SetVolume(f32 newVolume) = 0;
		virtual void SetPitch(f32 newPitch) = 0;
		virtual f32 GetVolume() const = 0;
		virtual f32 GetPitch() const = 0;
	};

	/* =====================================
		SoundGraph Sound, represents playing voice
		Based on Hazel::SoundGraphSound
		------------------------------------
	*/

	namespace Audio
	{
		namespace SoundGraph
		{
			class SoundGraphSound : public IPlayableAudio
			{
			public:
				explicit SoundGraphSound();
				~SoundGraphSound();

				//--- Sound Source Interface
				bool Play() override;
				bool Stop() override;
				bool Pause() override;
				bool IsPlaying() const override;
				// ~ End of Sound Source Interface

				void SetVolume(f32 newVolume) override;
				void SetPitch(f32 newPitch) override;
				void SetLooping(bool looping);

				f32 GetVolume() const override;
				f32 GetPitch() const override;

				void SetLowPassFilter(f32 value);   // 0.0 - 1.0 normalized
				void SetHighPassFilter(f32 value);  // 0.0 - 1.0 normalized

				//==============================================================================
				/// Sound Parameter Interface
				void SetParameter(u32 parameterID, f32 value);
				void SetParameter(u32 parameterID, i32 value);
				void SetParameter(u32 parameterID, bool value);

				virtual bool FadeIn(f32 duration, f32 targetVolume);
				virtual bool FadeOut(f32 duration, f32 targetVolume);

				//==============================================================================
				/// Initialization
				/** Initialize from SoundGraph instance */
				bool InitializeFromGraph(const Ref<Audio::SoundGraph::SoundGraph>& soundGraph);
				
				/** Initialize from SoundGraph asset and data sources */
				bool InitializeDataSource(const std::vector<AssetHandle>& dataSources, const Ref<Audio::SoundGraph::SoundGraph>& soundGraph);

				void ReleaseResources();

				//==============================================================================
				/// 3D Audio
				void SetLocation(const glm::vec3& location);
				void SetVelocity(const glm::vec3& velocity);
				void SetOrientation(const glm::vec3& forward, const glm::vec3& up);

				const glm::vec3& GetLocation() const { return m_Position; }  // Alias for m_Position
				const glm::vec3& GetVelocity() const { return m_Velocity; }

				//==============================================================================
				/// Status
				bool IsReadyToPlay() const { return m_IsReadyToPlay; }
				bool IsFinished() const;
				bool IsStopping() const { return m_IsStopping; }

				//==============================================================================
				/// Update (called from main thread)
				void Update(f32 deltaTime);

				//==============================================================================
				/// Advanced Control
				f32 GetCurrentFadeVolume() const { return m_CurrentFadeVolume; }
				f32 GetCurrentPriority() const;
				f32 GetPlaybackPercentage() const;

				Audio::SoundGraph::SoundGraphSource* GetSource() const { return m_Source.get(); }

			private:
				/* Stop playback with short fade-out to prevent click.
				@param numSamples - length of the fade-out in PCM frames
				
				@returns true - if successfully initialized fade
				*/
				bool StopFade(u64 numSamples);

				/* Stop playback with short fade-out to prevent click.
				@param milliseconds - length of the fade-out in milliseconds
				
				@returns true - if successfully initialized fade
				*/
				bool StopFade(i32 milliseconds);

				enum class StopOptions : u16
				{
					None = 0,
					NotifyPlaybackComplete  = (1 << 0),
					ResetPlaybackPosition   = (1 << 1)
				};

				// Bitwise operators for StopOptions
				inline friend StopOptions operator|(StopOptions lhs, StopOptions rhs)
				{
					return static_cast<StopOptions>(static_cast<u16>(lhs) | static_cast<u16>(rhs));
				}

				inline friend StopOptions operator&(StopOptions lhs, StopOptions rhs)
				{
					return static_cast<StopOptions>(static_cast<u16>(lhs) & static_cast<u16>(rhs));
				}

				inline friend StopOptions operator~(StopOptions opt)
				{
					return static_cast<StopOptions>(~static_cast<u16>(opt));
				}

				inline friend StopOptions& operator|=(StopOptions& lhs, StopOptions rhs)
				{
					lhs = lhs | rhs;
					return lhs;
				}

				inline friend StopOptions& operator&=(StopOptions& lhs, StopOptions rhs)
				{
					lhs = lhs & rhs;
					return lhs;
				}

				/* "Hard-stop" playback without fade. This is called to immediately stop playback,
				as well as to reset the play state when "stop-fade" has ended.
				@param options - combination of StopOptions flags
				
				@returns voice ID of the sound source in pool
				*/
				i32 StopNow(StopOptions options = StopOptions::None);

				void InitializeEffects(const Ref<SoundConfig>& config);
				bool InitializeAudioCallback();

				// Audio frequency conversion utilities
				static f32 NormalizedToFrequency(f32 normalizedValue);
				static f32 FrequencyToNormalized(f32 frequency);

			private:
				friend class AudioEngine;
				friend class SourceManager;

				std::function<void()> m_OnPlaybackComplete;
				std::string m_DebugName;

				SoundPlayState m_PlayState{ SoundPlayState::Stopped };
				SoundPlayState m_NextPlayState{ SoundPlayState::Stopped };

				/* Data source. SoundGraphSource handles the audio processing and miniaudio integration */
				Scope<Audio::SoundGraph::SoundGraphSource> m_Source = nullptr;

				// Note: MiniAudio nodes moved to SoundGraphSource to avoid header conflicts
				// Effects chain handled internally by SoundGraphSource

				// Playback status
				u8 m_Priority = 128;  // 0 = highest priority, 255 = lowest
				
				/* Stored Fader "resting" value. Used to restore Fader before restarting playback if a fade has occurred. */
				f32 m_StoredFaderValue = 1.0f;
				f32 m_LastFadeOutDuration = 0.0f;
				
				f64 m_Volume = 1.0;
				f64 m_Pitch = 1.0;
				
				/* Stop-fade counter. Used to stop the sound after "stopping-fade" has finished. */
				f64 m_StopFadeTime = 0.0;
				
				// Filter states
				f32 m_LowPassValue = 1.0f;   // Normalized filter value
				f32 m_HighPassValue = 0.0f;  // Normalized filter value
				
				// Spatial audio properties
				glm::vec3 m_Position{ 0.0f };
				glm::vec3 m_Orientation{ 0.0f, 0.0f, 1.0f };
				glm::vec3 m_Velocity{ 0.0f };
				
				// Status flags  
				bool m_IsReadyToPlay = false;
				bool m_IsStopping = false;
				bool m_IsLooping = false;
				bool m_IsFinished = false;
				f32 m_CurrentFadeVolume = 1.0f;

				// Fade control
				bool m_IsFading = false;
				f32 m_FadeStartVolume = 1.0f;
				f32 m_FadeTargetVolume = 1.0f;
				f32 m_FadeDuration = 0.0f;
				f32 m_FadeCurrentTime = 0.0f;
			};

		} // namespace SoundGraph
	} // namespace Audio

} // namespace OloEngine