#include "OloEnginePCH.h"
#include "AudioEngine.h"
#include "SoundGraph/SoundGraphPlayer.h"

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

namespace OloEngine
{
	ma_engine* AudioEngine::s_Engine;

	void AudioEngine::Init()
	{
		OLO_CORE_TRACE("[AudioEngine] Initializing.");
		ma_engine_config config = ::ma_engine_config_init();
		config.listenerCount = 1;

		s_Engine = new ma_engine();
		ma_result result = ::ma_engine_init(nullptr, s_Engine);
		OLO_CORE_ASSERT(result == MA_SUCCESS, "Failed to initialize audio engine!");

		// Initialize the sound graph manager with the engine's sample rate
		f64 sampleRate = static_cast<f64>(ma_engine_get_sample_rate(s_Engine));
		Audio::SoundGraphManager::GetInstance().Initialize(sampleRate);
	}

	void AudioEngine::Shutdown()
	{
		// Shutdown the sound graph manager first
		Audio::SoundGraphManager::GetInstance().Shutdown();

		::ma_engine_uninit(s_Engine);
		delete s_Engine;
	}

	[[nodiscard("Store this!")]] AudioEngineInternal AudioEngine::GetEngine()
	{
		return s_Engine;
	}
}
