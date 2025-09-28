#include "OloEnginePCH.h"
#include "AudioEngine.h"

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
		
		if (result == MA_SUCCESS)
		{
			OLO_CORE_TRACE("[AudioEngine] Initialized successfully with sample rate {}", ma_engine_get_sample_rate(s_Engine));
		}
		else
		{
			OLO_CORE_ERROR("[AudioEngine] Failed to initialize audio engine! Error code: {}", static_cast<int>(result));
			delete s_Engine;
			s_Engine = nullptr;
		}
		
		OLO_CORE_ASSERT(result == MA_SUCCESS, "Failed to initialize audio engine!");
	}

	void AudioEngine::Shutdown()
	{
		OLO_CORE_TRACE("[AudioEngine] Shutting down.");

		::ma_engine_uninit(s_Engine);
		delete s_Engine;
		s_Engine = nullptr;

		OLO_CORE_TRACE("[AudioEngine] Shutdown complete.");
	}

	[[nodiscard("Store this!")]] AudioEngineInternal AudioEngine::GetEngine()
	{
		return s_Engine;
	}
}
