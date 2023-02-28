#pragma once

#include <glm/vec3.hpp>

namespace OloEngine
{
	struct AudioListenerConfig
	{
		f32 ConeInnerAngle = glm::radians(360.0f);
		f32 ConeOuterAngle = glm::radians(360.0f);
		f32 ConeOuterGain = 0.0f;
	};

	class AudioListener
	{
	public:
		AudioListener() = default;
		~AudioListener() = default;

		void SetConfig(const AudioListenerConfig& config) const;
		void SetPosition(const glm::vec3& position) const;
		void SetDirection(const glm::vec3& forward) const;
		void SetVelocity(const glm::vec3& velocity) const;

	private:
		u32 m_ListenerIndex = 0;
	};
}
