#include "OloEnginePCH.h"
#include "AudioListener.h"

#include <miniaudio.h>

#include "AudioEngine.h"

namespace OloEngine
{
	void AudioListener::SetConfig(const AudioListenerConfig& config) const
	{
		auto* engine = (ma_engine*)AudioEngine::GetEngine();
		::ma_engine_listener_set_cone(engine, m_ListenerIndex, config.ConeInnerAngle, config.ConeOuterAngle, config.ConeOuterGain);
	}
	
	void AudioListener::SetPosition(const glm::vec3& position) const
	{
		auto* engine = (ma_engine*)AudioEngine::GetEngine();
		::ma_engine_listener_set_position(engine, m_ListenerIndex, position.x, position.y, position.z);

		static bool setupWorldUp = false;
		if (!setupWorldUp)
		{
			::ma_engine_listener_set_world_up(engine, m_ListenerIndex, 0, 1, 0);
			setupWorldUp = true;
		}
	}

	void AudioListener::SetDirection(const glm::vec3& forward) const
	{
		auto* engine = (ma_engine*)AudioEngine::GetEngine();
		::ma_engine_listener_set_direction(engine, m_ListenerIndex, forward.x, forward.y, forward.z);
	}

	void AudioListener::SetVelocity(const glm::vec3& velocity) const
	{
		auto* engine = (ma_engine*)AudioEngine::GetEngine();
		::ma_engine_listener_set_velocity(engine, m_ListenerIndex, velocity.x, velocity.y, velocity.z);
	}
}
