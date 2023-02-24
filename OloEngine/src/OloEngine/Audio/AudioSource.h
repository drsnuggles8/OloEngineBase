#pragma once

#include <glm/vec3.hpp>

struct ma_sound;

namespace OloEngine
{
	enum class AttenuationModelType
	{
		None = 0,
		Inverse,
		Linear,
		Exponential
	};

	struct AudioSourceConfig
	{
		f32 VolumeMultiplier = 1.0f;
		f32 PitchMultiplier = 1.0f;
		bool PlayOnAwake = true;
		bool Looping = false;

		bool Spatialization = false;
		AttenuationModelType AttenuationModel = AttenuationModelType::Inverse;
		f32 RollOff = 1.0f;
		f32 MinGain = 0.0f;
		f32 MaxGain = 1.0f;
		f32 MinDistance = 0.3f;
		f32 MaxDistance = 1000.0f;

		f32 ConeInnerAngle = glm::radians(360.0f);
		f32 ConeOuterAngle = glm::radians(360.0f);
		f32 ConeOuterGain = 0.0f;

		f32 DopplerFactor = 1.0f;
	};

	class AudioSource
	{
	public:
		AudioSource(const char* filepath);
		~AudioSource();

		AudioSource(const AudioSource& other) = default;
		AudioSource(AudioSource&& other) = default;

		[[nodiscard("Store this!")]] const char* GetPath() const { return m_Path.c_str(); }

		void Play() const;
		void Pause() const;
		void UnPause() const;
		void Stop() const;
		[[nodiscard("Store this!")]] bool IsPlaying() const;

		void SetConfig(const AudioSourceConfig& config);

		void SetVolume(const f32 volume) const;
		void SetPitch(const f32 pitch) const;
		void SetLooping(const bool state) const;
		void SetSpatialization(const bool state);
		void SetAttenuationModel(const AttenuationModelType type) const;
		void SetRollOff(const f32 rollOff) const;
		void SetMinGain(const f32 minGain) const;
		void SetMaxGain(const f32 maxGain) const;
		void SetMinDistance(const f32 minDistance) const;
		void SetMaxDistance(const f32 maxDistance) const;
		void SetCone(const f32 innerAngle, const f32 outerAngle, const f32 outerGain) const;
		void SetDopplerFactor(const f32 factor) const;

		void SetPosition(const glm::vec3& position) const;
		void SetDirection(const glm::vec3& forward) const;
		void SetVelocity(const glm::vec3& velocity) const;

	private:
		std::string m_Path;
		Scope<ma_sound> m_Sound;
		bool m_Spatialization = false;
	};
}
